#include "opencode_session.h"

#include <algorithm>
#include <atomic>
#include <utility>

#if CONFIG_WQN_AGENT_ENABLE

#include "ai_session.h"
#include "ai_history.h"
#include "agent_voice_pipe.h"
#include "audio_capture.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opencode_client.h"
#include "runtime/sleep_coordinator.h"
#include "services/connectivity_service.h"
#include "storage.h"

namespace {

constexpr char kTag[] = "wqn_agent";
constexpr TickType_t kConnectivityWait = pdMS_TO_TICKS(20000);
constexpr size_t kMaxAgentTextBytes = 12 * 1024;
// Reasoning gets its own budget rather than sharing the answer's: a long
// chain-of-thought must not starve the reply the user actually asked for, and
// the history channel is evicted oldest-first, so an unbounded thinking buffer
// would push the whole conversation out of the ring.
constexpr size_t kMaxThinkingBytes = 2 * 1024;
constexpr size_t kMaxPromptBytes = 4096;
constexpr uint32_t kWorkerStackBytes = 9216;
// [voice-pipe] How long Transcribe() waits for the agent WS turn to produce an
// ASR result after FINAL. The ASR-only pipeline is fast (streamed deltas),
// and a stuck turn should surface as an error long before the STD driver's
// 10-minute chat budget.
constexpr uint32_t kAgentVoiceFinalizeTimeoutMs = 15000;

enum class WorkerCommand : uint8_t {
    kNone,
    kLoadSessions,
    kCreateSession,
    kPrepareCapture,
    kTranscribe,
    kCancelVoice,
    kRunPrompt,
    kObserveSession,
    kLoadHistory,
};

StaticSemaphore_t g_lock_storage = {};
SemaphoreHandle_t g_lock = nullptr;
StaticTask_t g_worker_tcb = {};
StackType_t g_worker_stack[kWorkerStackBytes / sizeof(StackType_t)] = {};
TaskHandle_t g_worker = nullptr;
wqn::AgentSessionState g_state;
WorkerCommand g_command = WorkerCommand::kNone;
bool g_changed = false;
bool g_recording_requested = false;
bool g_run_failed = false;
bool g_observing = false;
std::string g_run_session_id;
std::string g_run_prompt;
// [detail] Detail tier for the command in flight, handed over the same way as
// g_run_session_id: written under g_lock at arm time, read by the worker
// without the lock. Not cleared on finish -- every arm overwrites it.
uint8_t g_run_detail = wqn::kOpenCodeDetailDefault;
// One-shot for the post-run list refresh: RunPrompt sets it before chaining
// kLoadHistory, so the backfill knows it is replacing the live transcript of a
// finished run (not filling the empty channel a lock starts from). Consumed by
// LoadHistory, which also leaves the run-terminal UI alone when it is set.
bool g_history_refresh = false;
wqn::OpenCodeOutboundQueue g_outbound_replies;
// The ask a queued reply answers. The UI closes its option bar the moment the
// reply is queued, so the live ask id is not proof that a reply went out: this
// is. Only a failed POST consults it, and it holds exactly one reply because
// the worker drains the queue serially.
std::string g_reply_flight_permission_id;
std::string g_reply_flight_question_id;
// Cancel handshake for an attached stream. The UI thread only sets the request;
// the worker is the one that makes the interrupt POST, so it is also the one
// that records whether the interrupt actually reached the gateway.
std::atomic<bool> g_interrupt_requested{false};
std::atomic<bool> g_interrupt_delivered{false};
// [voice-pipe] Hard-abort request for the capture phases. Set by
// CancelAgentVoiceInput (UI thread); consumed by the kCancelVoice worker
// command -- or, when the capture worker is still bringing the WS turn up
// inside kPrepareCapture, by that handler's own check after the turn exists.
bool g_voice_abort_requested = false;
// Last ASR error message from the agent voice stream, stashed by
// OnAgentVoiceSse for the worker's error branch. Reset at the start of every
// capture so a stale message can never be reported for a new turn.
std::string g_voice_last_error;
wqn::runtime::SleepLease g_agent_sleep_lease;
wqn::services::ConnectivityDemand g_connectivity_demand;

void DiscardOutboundReplies();
// Declared up here: clearing a pending ask is part of every terminal status and
// of every list load, which run long before the definition below.
void ClearPendingPermissionLocked();
void ClearPendingQuestionLocked();
void ClearReplyFlightLocked();
void ClearDeferredQuestionLocked();
void PromoteDeferredQuestionLocked();
void ClearAllAsksLocked();

void MarkChangedLocked()
{
    g_changed = true;
}

// The longest prefix of `text` that fits `max_bytes` without ending inside a
// UTF-8 sequence. Truncating a Chinese answer at a byte count with substr()
// splits a 3-byte character into stray bytes, and the history channel then
// renders mojibake (or drops the tail of the message). Backing up from the
// cut to the lead byte and keeping the whole character when it fits keeps the
// buffer valid UTF-8 at every prefix length.
size_t Utf8SafePrefixBytes(std::string_view text, size_t max_bytes)
{
    if (text.size() <= max_bytes) {
        return text.size();
    }
    size_t start = max_bytes;
    while (start > 0 &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        --start;
    }
    const unsigned char lead = static_cast<unsigned char>(text[start]);
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
    }
    return start + length <= max_bytes ? start + length : start;
}

void SetPhaseLocked(wqn::AiFeaturePhase phase, const std::string& status)
{
    g_state.ui.phase = phase;
    g_state.ui.status_label = status;
    MarkChangedLocked();
}

void ReleaseWorkOwnershipLocked()
{
    g_connectivity_demand.Reset();
    g_agent_sleep_lease.Reset();
}

void SetErrorLocked(const std::string& message)
{
    g_state.ui.phase = wqn::AiFeaturePhase::kError;
    g_state.ui.status_label = "错误";
    g_state.ui.activity_text = message;
    g_state.ui.action_hint = "长按确认重新录音";
    g_state.ui.requires_confirmation = false;
    g_state.confirmation_armed_at_ms = 0;
    g_state.stream_active = false;
    // Every SetErrorLocked call site is turn-terminal or pre-turn, and the
    // stream that would carry a reply is gone: an ask left armed here can never
    // be answered and would hold the option bar against the next turn's asks.
    ClearAllAsksLocked();
    MarkChangedLocked();
    ReleaseWorkOwnershipLocked();
}

bool ArmWorkerLocked(WorkerCommand command)
{
    if (g_command != WorkerCommand::kNone || g_worker == nullptr) {
        return false;
    }
    g_command = command;
    xTaskNotifyGive(g_worker);
    return true;
}

// Chains the next command from inside the worker command that is running now.
// ArmWorkerLocked cannot be used for this: it refuses while g_command is still
// occupied by the running command (WorkerTask only clears it after the handler
// returns), so the history->observe chain used to fall through to its error
// branch on every single observe. The worker is the only caller, which is why
// no g_worker null check is needed.
void ChainWorkerCommandLocked(WorkerCommand command)
{
    g_command = command;
    xTaskNotifyGive(g_worker);
}

esp_err_t LoadToken(std::string* token)
{
    if (token == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = wqn::LoadAccessToken(token);
    if (result != ESP_OK || !wqn::IsValidAccessToken(*token)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t AcquireNetwork()
{
    wqn::services::ConnectivityDemand demand =
        wqn::services::AcquireConnectivityDemand(
            wqn::services::ConnectivityDemandReason::kAiInteractive,
            "opencode-agent",
            __FILE__,
            __LINE__);
    if (!demand) {
        return ESP_ERR_INVALID_STATE;
    }
    const wqn::services::ConnectivityDemandTicket ticket = demand.ticket();
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_connectivity_demand = std::move(demand);
    xSemaphoreGive(g_lock);
    return wqn::services::ConnectivityWaitResultToEspErr(
        wqn::services::WaitForConnectivity(ticket, kConnectivityWait));
}

void FinishCommand(WorkerCommand completed)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_command == completed) {
        g_command = WorkerCommand::kNone;
    }
    xSemaphoreGive(g_lock);
}

void LoadSessions()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    std::vector<wqn::OpenCodeSessionInfo> sessions;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::ListOpenCodeSessions(token, &sessions, &api_result);
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result == ESP_OK) {
        g_state.sessions.clear();
        g_state.sessions.reserve(sessions.size());
        for (wqn::OpenCodeSessionInfo& source : sessions) {
            g_state.sessions.push_back(wqn::AgentSessionOption{
                std::move(source.id), std::move(source.title), source.updated_at});
        }
        if (g_state.sessions.empty()) {
            SetErrorLocked("没有可用的 OpenCode Session");
        } else {
            g_state.selected_session = std::min(
                g_state.selected_session, g_state.sessions.size() - 1);
            g_state.session_locked = false;
            g_state.current_session_id.clear();
            g_state.current_session_title.clear();
            ClearAllAsksLocked();
            // A fresh list means no session's transcript is on screen any more,
            // so the next observe must backfill again.
            g_state.history_loaded_session_id.clear();
            g_observing = false;
            g_state.ui.context_label.clear();
            g_state.ui.prompt_text.clear();
            g_state.ui.response_text.clear();
            g_state.ui.scroll_offset_lines = 0;
            g_state.ui.requires_confirmation = false;
            g_state.confirmation_armed_at_ms = 0;
            g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
            g_state.ui.status_label = "选择 Session";
            g_state.ui.activity_text = "上下选择，确认锁定";
            g_state.ui.action_hint = "↑/↓ 选择 · 确认锁定 · 长按新建";
            MarkChangedLocked();
            ReleaseWorkOwnershipLocked();
        }
    } else {
        SetErrorLocked(api_result.detail.empty() ? "Session 列表加载失败" : api_result.detail);
    }
    xSemaphoreGive(g_lock);
}

// ---- Voice pipe (WS turn) --------------------------------------------------
// [voice-pipe] The Agent capture rides the shared Std/Pro WebSocket with
// tier=agent: the capture tap (owned by ai_session, armed once in InitAiSession)
// forwards PCM into stdpro_ws, which drops it until a turn reaches kRecording.
// agent_voice_pipe owns the turn itself; these helpers bridge it to the worker
// and the UI.

// UI-side observer for the live voice stream. Runs on the transport task, so
// every state write takes g_lock. The module's trampoline already accumulates
// the transcript for the worker; this only shapes what the screen shows while
// the turn is live.
void OnAgentVoiceSse(const wqn::WqnAiSseEvent& ev, void* /*user_ctx*/)
{
    using Kind = wqn::WqnAiSseEvent::Kind;
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const wqn::AiFeaturePhase phase = g_state.ui.phase;
    const bool capturing = phase == wqn::AiFeaturePhase::kRecording ||
                           phase == wqn::AiFeaturePhase::kTranscribing;
    switch (ev.kind) {
        case Kind::kAsrDelta: {
            if (!capturing) {
                break;
            }
            g_state.ui.voice_partial += ev.delta;
            // Same delta coalescing as the STD consumer (ai_session.cpp
            // OnSseEvent): every change mark costs a full state copy on the UI
            // task, and deltas land far faster than the panel can repaint.
            static int64_t s_last_delta_mark_ms = -1000;
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_last_delta_mark_ms >= 50) {
                s_last_delta_mark_ms = now_ms;
                MarkChangedLocked();
            }
            break;
        }
        case Kind::kAsrComplete:
            if (capturing) {
                g_state.ui.voice_partial = ev.text;
                MarkChangedLocked();
            }
            break;
        case Kind::kAsrFailed:
            g_voice_last_error = ev.error_message.empty()
                ? std::string("语音识别失败")
                : ev.error_message;
            break;
        case Kind::kError:
            g_voice_last_error = ev.error_message.empty() ? ev.error_code
                                                          : ev.error_message;
            break;
        default:
            break;
    }
    xSemaphoreGive(g_lock);
}

// Bring up the WS turn that carries this capture. Called from the capture
// worker after the microphone is running; every failure is non-fatal --
// Transcribe() falls back to the batch endpoint when no turn exists.
void StartVoiceTurn(const std::string& token)
{
    const std::string request_id = wqn::GenerateRequestId();
    uint32_t turn_gen = 0;
    const esp_err_t result = wqn::AgentVoiceTurnStart(token, request_id, &turn_gen);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "agent voice turn not started (%s); batch fallback armed",
                 esp_err_to_name(result));
    }
}

// Hard abort: drop the captured audio, tear the WS turn down (a no-op when it
// was never established) and release everything the capture acquired. The UI
// half (idle label) is set by CancelAgentVoiceInput, which is the only caller.
void TearDownVoiceCapture()
{
    wqn::AgentVoiceTurnAbort();
    wqn::AudioCaptureChunk discarded;
    wqn::StopAudioCapture(&discarded);
    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_recording_requested = false;
    g_voice_abort_requested = false;
    g_state.ui.voice_partial.clear();
    ReleaseWorkOwnershipLocked();
    xSemaphoreGive(g_lock);
}

void PrepareCapture()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    bool should_capture = false;
    bool mic_busy = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    should_capture = result == ESP_OK && g_recording_requested;
    xSemaphoreGive(g_lock);
    if (should_capture) {
        // [voice-pipe] The Agent capture rides the shared Std/Pro WebSocket
        // (tier=agent), so the capture tap STAYS installed: it forwards PCM
        // into stdpro_ws, which drops it until the turn reaches kRecording.
        // The tap is armed once in InitAiSession and nothing may disable it
        // here -- that is what keeps this capture out of the batch-only path.
        if (wqn::IsAudioCaptureRunning()) {
            mic_busy = true;
            result = ESP_ERR_INVALID_STATE;
        } else {
            result = wqn::StartAudioCapture();
        }
    }
    bool capture_started = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK) {
        SetErrorLocked(mic_busy
            ? "麦克风被其他功能占用"
            : (result == ESP_ERR_INVALID_STATE
                ? "设备未配对或网络不可用"
                : "录音启动失败"));
    } else if (!g_recording_requested) {
        wqn::AudioCaptureChunk discarded;
        xSemaphoreGive(g_lock);
        wqn::StopAudioCapture(&discarded);
        wqn::ReleaseAudioCapturePower();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        if (!g_state.ui.prompt_text.empty() && g_state.ui.requires_confirmation) {
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingConfirmation;
            g_state.ui.status_label = "确认后发送";
            g_state.ui.activity_text = "追加录音已取消，原转写尚未执行";
            g_state.ui.action_hint = "↑ 发送 · ↓ 取消 · 长按确认追加";
            g_state.confirmation_armed_at_ms = esp_timer_get_time() / 1000;
        } else {
            g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
            g_state.ui.status_label = "已取消录音";
        }
        MarkChangedLocked();
        ReleaseWorkOwnershipLocked();
    } else {
        g_state.ui.phase = wqn::AiFeaturePhase::kRecording;
        g_state.ui.status_label = "录音中";
        g_state.ui.activity_text = "松开确认键开始转写";
        g_state.ui.action_hint = "松开确认键停止";
        MarkChangedLocked();
        capture_started = true;
    }
    xSemaphoreGive(g_lock);

    if (!capture_started) {
        return;
    }
    // Bring the WS turn up OUTSIDE the lock: EnsureConnected can block for
    // seconds and must never hold the state mutex. The turn belongs to this
    // capture -- Transcribe() finalizes it, and falls back to the batch
    // endpoint when it was never established.
    wqn::AgentVoiceRegisterDeltaCallback(&OnAgentVoiceSse, nullptr);
    StartVoiceTurn(token);

    // [voice-pipe] A Down-cancel that landed while this worker owned the
    // command slot could not arm kCancelVoice; honor it here, now that the
    // turn exists, so no half-open turn is left behind.
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool abort_requested = g_voice_abort_requested;
    xSemaphoreGive(g_lock);
    if (abort_requested) {
        TearDownVoiceCapture();
    }
}

void Transcribe()
{
    wqn::AudioCaptureChunk audio;
    esp_err_t result = wqn::StopAudioCapture(&audio);
    if (result == ESP_OK && (audio.empty() || audio.duration_ms < 1000)) {
        result = ESP_ERR_INVALID_SIZE;
    }
    std::string token;
    if (result == ESP_OK) {
        result = LoadToken(&token);
    }
    std::string transcript;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        // [voice-pipe] A live WS turn carries this clip: FINAL hands the audio
        // over and the ASR text comes back from the stream. When no turn was
        // established (connect/start failed, or the clip never left the
        // device), ESP_ERR_INVALID_STATE means the batch endpoint is the only
        // safe path. A soft cancel returns ESP_OK with an empty transcript and
        // is handled by the cancel branch below.
        const esp_err_t ws_result = wqn::AgentVoiceTurnFinalize(
            audio.duration_ms, kAgentVoiceFinalizeTimeoutMs, &transcript);
        if (ws_result == ESP_ERR_INVALID_STATE) {
            result = wqn::TranscribeOpenCodeAudio(token, audio, &transcript, &api_result);
        } else {
            result = ws_result;
        }
    }
    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_recording_requested = false;
    const bool cancelled = wqn::AgentVoiceCancelRequested();
    wqn::AgentVoiceClearCancel();
    g_state.ui.voice_partial.clear();
    if (cancelled) {
        // [voice-pipe] The UI already flipped to idle when the user cancelled;
        // there is nothing to commit and nothing to report. The capture's
        // network demand and sleep lease still have to go back, though.
        ReleaseWorkOwnershipLocked();
    } else if (result == ESP_OK &&
        g_state.ui.prompt_text.size() + transcript.size() +
                (g_state.ui.prompt_text.empty() ? 0 : 1) >
            kMaxPromptBytes) {
        SetErrorLocked("转写内容过长，请取消后缩短输入");
    } else if (result == ESP_OK) {
        if (!g_state.ui.prompt_text.empty()) {
            g_state.ui.prompt_text += "\n";
        }
        g_state.ui.prompt_text += transcript;
        g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingConfirmation;
        g_state.ui.status_label = "确认后发送";
        g_state.ui.activity_text = "语音已转写，尚未执行";
        g_state.ui.action_hint = "↑ 发送 · ↓ 取消 · 长按确认追加";
        g_state.ui.requires_confirmation = true;
        g_state.confirmation_armed_at_ms = esp_timer_get_time() / 1000;
        MarkChangedLocked();
        ReleaseWorkOwnershipLocked();
    } else if (result == ESP_ERR_INVALID_SIZE) {
        SetErrorLocked("录音过短或未检测到语音");
    } else {
        // The stream's error text (when one arrived) beats the generic label;
        // the batch path has its own detail in api_result.
        const std::string& detail = !api_result.detail.empty()
            ? api_result.detail
            : g_voice_last_error;
        SetErrorLocked(detail.empty() ? "语音转写失败" : detail);
    }
    xSemaphoreGive(g_lock);
}

// ---- History mirror --------------------------------------------------------
// [agent] The Agent tier is rendered by the AI page's chat-bubble viewport, so
// every stream event that changes what the user reads is mirrored into the
// kAgent history channel. The channel is chosen explicitly rather than read
// from the visible tier: a run started on one tier must never append to
// another tier's conversation. Everything below runs with g_lock held.

// The renderer draws a thinking bubble from the message kind alone; the marker
// prefix is the caller's, matching ai_session.cpp / flash_session.cpp.
std::string AgentThinkingLabel(const std::string& text)
{
    return text.empty() ? std::string() : std::string("\xE2\x9C\x8D ") + text;
}

wqn::ChatMessageId g_agent_assistant_id = wqn::kInvalidChatMessageId;
wqn::ChatMessageId g_agent_tool_id = wqn::kInvalidChatMessageId;
wqn::ChatMessageId g_agent_thinking_id = wqn::kInvalidChatMessageId;
std::string g_agent_tool_name;
// Call id of the open block, when the gateway sends one. See the kTool branch.
std::string g_agent_tool_call;
// Reasoning has its own channel and its own buffer. It is deliberately NOT
// accumulated into `g_state.ui.response_text`: even if the gateway mapped a
// reasoning frame as `agent.text`, the answer the user reads would stay clean.
std::string g_agent_thinking_text;
// Whether the open tool block succeeded. The gateway's `agent.tool` status is
// the only signal available; `error` is the failure case.
bool g_agent_tool_ok = true;
// Last status/preview seen for the open tool block. The gateway's `agent.tool`
// event carries a free-form status string, not an explicit start/end pair, so
// the newest detail wins and the block is closed by whatever arrives next.
std::string g_agent_tool_detail;
int64_t g_agent_tool_since_ms = 0;

void ResetAgentHistoryTurnLocked()
{
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
    g_agent_tool_id = wqn::kInvalidChatMessageId;
    g_agent_thinking_id = wqn::kInvalidChatMessageId;
    g_agent_tool_name.clear();
    g_agent_tool_call.clear();
    g_agent_thinking_text.clear();
    g_agent_tool_detail.clear();
    g_agent_tool_since_ms = 0;
    g_agent_tool_ok = true;
}

// [follow] Arm the Agent viewport follow for a fresh turn. Every turn-start
// path (submit, permission/question reply, voice capture, observe) calls this
// so the viewport watches the run: the per-tick step
// (UiRuntime::DispatchAiViewportFollow) pins it to the live tail while the run
// executes and retires the follow -- parking the viewport on the answer's first
// line -- as soon as a non-empty answer body lands. `user_moved=false` marks
// this turn's viewport as untouched, which is what the follow step needs to
// decide whether it may move it at all.
//
// Caller must hold g_lock; the caller's own MarkChangedLocked() covers the
// state change (the follow flags are read from the same snapshot).
void ArmAgentFollowLocked()
{
    g_state.follow_active = true;
    g_state.user_moved = false;
}

// Closes the open tool placeholder into a result block. Because the gateway
// has no tool-end event, this is driven by the next event of any kind (text, a
// different tool, run end, error) instead of by guessing the status vocabulary
// -- that keeps exactly one block per tool and never leaks a "running"
// placeholder into the rendered history.
void CloseAgentToolBlockLocked(bool ok, int64_t now_ms)
{
    if (g_agent_tool_id == wqn::kInvalidChatMessageId) {
        return;
    }
    const std::string name = g_agent_tool_name;
    const std::string detail = g_agent_tool_detail;
    const int64_t since_ms = g_agent_tool_since_ms;
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent);
    if (history.PopLastIf(wqn::ChatMessageKind::kToolStart)) {
        const int32_t elapsed_ms =
            (since_ms > 0 && now_ms >= since_ms) ? static_cast<int32_t>(now_ms - since_ms) : 0;
        history.AppendToolResult(name, std::string_view(), detail, ok, elapsed_ms, now_ms);
    }
    g_agent_tool_id = wqn::kInvalidChatMessageId;
    g_agent_tool_name.clear();
    g_agent_tool_call.clear();
    g_agent_tool_detail.clear();
    g_agent_tool_since_ms = 0;
    // Post-tool text must open a NEW assistant entry. Rewriting the pre-tool
    // entry in place would drag it below the tool block and overwrite its own
    // text with the post-tool text.
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
}

// Streams the accumulated gateway text into a single assistant entry: the
// first delta creates it, later deltas replace it in place. Replacing matters
// -- the gateway emits dozens of deltas per reply and appending each one would
// blow through the ring buffer's byte budget before the reply finished.
void MirrorAgentTextLocked(int64_t now_ms)
{
    CloseAgentToolBlockLocked(g_agent_tool_ok, now_ms);
    const std::string& text = g_state.ui.response_text;
    if (text.empty()) {
        return;
    }
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent);
    if (g_agent_assistant_id == wqn::kInvalidChatMessageId) {
        g_agent_assistant_id = history.AppendAssistant(text, now_ms);
        return;
    }
    history.ReplaceText(g_agent_assistant_id, wqn::ChatMessageKind::kAssistant, text, now_ms);
}

// Same in-place contract as MirrorAgentTextLocked, on the thinking channel. The
// gateway can emit hundreds of reasoning deltas for one turn, and each new
// `kThinking` message would evict the ring buffer's head -- i.e. the rest of
// the conversation -- before the reply even finished.
void MirrorAgentThinkingLocked(int64_t now_ms)
{
    CloseAgentToolBlockLocked(g_agent_tool_ok, now_ms);
    if (g_agent_thinking_text.empty()) {
        return;
    }
    // Post-tool reasoning must open a NEW entry for the same reason post-tool
    // text does (see CloseAgentToolBlockLocked).
    if (g_agent_thinking_id == wqn::kInvalidChatMessageId) {
        g_agent_thinking_id = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent)
                                  .AppendThinking(AgentThinkingLabel(g_agent_thinking_text), now_ms);
        return;
    }
    wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).ReplaceText(
        g_agent_thinking_id, wqn::ChatMessageKind::kThinking,
        AgentThinkingLabel(g_agent_thinking_text), now_ms);
}

// Records the submitted prompt and arms a fresh turn. Called before the worker
// starts so the user bubble is on screen while the gateway is still connecting.
void AppendAgentUserLocked(std::string_view text, int64_t now_ms)
{
    ResetAgentHistoryTurnLocked();
    if (!text.empty()) {
        wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).AppendUser(text, now_ms);
    }
}

void AppendAgentErrorLocked(std::string_view text, int64_t now_ms)
{
    CloseAgentToolBlockLocked(false, now_ms);
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
    g_agent_thinking_id = wqn::kInvalidChatMessageId;
    g_agent_thinking_text.clear();
    if (!text.empty()) {
        wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).AppendAssistant(text, now_ms);
    }
}

// Drops a pending permission and the session that raised it. The two travel
// together: the reply route is scoped to the owning session, so keeping one
// without the other answers on whatever session happens to be attached next.
void ClearPendingPermissionLocked()
{
    g_state.pending_permission_id.clear();
    g_state.pending_permission_session.clear();
}

void ClearPendingQuestionLocked()
{
    g_state.pending_question_id.clear();
    g_state.pending_question_title.clear();
    g_state.pending_question_options.clear();
    g_state.pending_question_session.clear();
}

// Forget which reply is on the wire once the ask it answers is over. A stream
// that outlives the ask (a permission answered from the OpenCode side, a run
// that ended while a reply was in flight) must not let a later failure restore
// an ask the user already moved past.
void ClearReplyFlightLocked()
{
    g_reply_flight_permission_id.clear();
    g_reply_flight_question_id.clear();
}

// One question the device could not show yet, because a permission ask holds the
// option bar. It is promoted the moment that bar is free, so an ask that arrives
// behind another is answered rather than lost.
bool g_deferred_question_valid = false;
std::string g_deferred_question_id;
std::string g_deferred_question_title;
std::vector<wqn::OpenCodeQuestionOption> g_deferred_question_options;
// The session the deferred question came from, promoted with it below.
std::string g_deferred_question_session;

// Hand the deferred ask the option bar it has been waiting for. Only a
// permission reply calls this, because only a permission reply can free the bar:
// a question arm means the permission slot is already empty, and a terminal
// status clears the deferred ask instead of promoting it.
void PromoteDeferredQuestionLocked()
{
    if (!g_deferred_question_valid || !g_state.pending_permission_id.empty() ||
        !g_state.pending_question_id.empty()) {
        return;
    }
    g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingQuestion;
    g_state.ui.status_label = "等待回答";
    g_state.pending_question_id = std::move(g_deferred_question_id);
    g_state.pending_question_title = std::move(g_deferred_question_title);
    g_state.pending_question_options = std::move(g_deferred_question_options);
    g_state.pending_question_session = std::move(g_deferred_question_session);
    g_state.ui.activity_text = g_state.pending_question_title;
    g_state.ui.action_hint = "↑/↓ 选择 · 确认回答";
    g_deferred_question_valid = false;
    MarkChangedLocked();
}

void ClearDeferredQuestionLocked()
{
    g_deferred_question_valid = false;
    g_deferred_question_id.clear();
    g_deferred_question_title.clear();
    g_deferred_question_options.clear();
    g_deferred_question_session.clear();
}

// Drops every ask the device is holding: both slots, the deferred question and
// the in-flight reply bookkeeping. This is what a turn boundary needs -- an ask
// that outlives its run can never be answered, holds the option bar, and would
// otherwise swallow the next turn's first ask.
void ClearAllAsksLocked()
{
    ClearPendingPermissionLocked();
    ClearPendingQuestionLocked();
    ClearDeferredQuestionLocked();
    ClearReplyFlightLocked();
}

// Replays one session's backfilled turns into the kAgent channel, oldest first.
// The gateway already chose the window and truncated each field, so this is a
// direct projection: no dedupe against live deltas, because the caller resets
// the turn lock first and subscribes to the stream only afterwards.
void BackfillAgentHistoryLocked(const std::vector<wqn::OpenCodeHistoryMessage>& messages,
                                int64_t now_ms)
{
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent);
    for (const wqn::OpenCodeHistoryMessage& message : messages) {
        if (message.role == "user") {
            if (!message.text.empty()) {
                history.AppendUser(message.text, now_ms);
            }
            continue;
        }
        if (message.role != "assistant") {
            continue;
        }
        if (!message.thinking.empty()) {
            history.AppendThinking(AgentThinkingLabel(message.thinking), now_ms);
        }
        if (!message.text.empty()) {
            history.AppendAssistant(message.text, now_ms);
        }
        for (const wqn::OpenCodeHistoryTool& tool : message.tools) {
            if (tool.name.empty()) {
                continue;
            }
            // Append-then-close keeps one finished block per call. The tool
            // placeholder has no separate start event on this path, so there is
            // no turn lock to maintain either.
            history.AppendToolStart(tool.name, std::string_view(), now_ms);
            history.PopLastIf(wqn::ChatMessageKind::kToolStart);
            history.AppendToolResult(tool.name, std::string_view(), tool.preview,
                                     tool.status != "error", 0, now_ms);
        }
    }
}

void OnOpenCodeEvent(const wqn::OpenCodeEvent& event, void*)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    switch (event.kind) {
        case wqn::OpenCodeEventKind::kAccepted:
            SetPhaseLocked(wqn::AiFeaturePhase::kRunning, "Agent 执行中");
            g_state.ui.activity_text = "OpenCode 已接收任务";
            break;
        case wqn::OpenCodeEventKind::kAttached:
            SetPhaseLocked(wqn::AiFeaturePhase::kRunning, "观察中");
            g_state.ui.activity_text = "已连接 Session 事件流";
            break;
        case wqn::OpenCodeEventKind::kStatus:
            // [P3c] A pending ask is only cleared by a terminal status. Clearing
            // it on every status frame disarmed a live ask whenever the gateway
            // emitted an incidental one (retry, compaction), and the device then
            // had no way to answer it -- the run blocked until the 30-minute
            // stream timeout and surfaced as stream_incomplete.
            if (event.status == "idle" || event.status == "error") {
                // Includes the deferred question: with the run over it can
                // never be promoted, and leaving it queued would arm a dead
                // ask on the next turn.
                ClearAllAsksLocked();
            }
            if (event.status == "idle") {
                g_state.stream_active = false;
                // Run finished: close a tool block the gateway never closed.
                CloseAgentToolBlockLocked(!g_run_failed, now_ms);
                if (!g_run_failed) {
                    SetPhaseLocked(wqn::AiFeaturePhase::kComplete, "执行完成");
                    if (g_observing) {
                        g_state.ui.status_label = "观察结束";
                        g_state.ui.activity_text = "无运行中任务或任务已结束";
                    }
                    g_state.ui.action_hint = g_observing
                        ? "长按确认录音 · 双击确认观察"
                        : "长按确认发起新任务";
                }
            } else if (event.status == "error") {
                // A terminal error status: v2's gateway does not emit this
                // today (failures arrive as agent.error + idle), but the
                // contract allows it, and without this branch it fell into the
                // generic else below -- a failed run was relabelled
                // "Agent 执行中" and `g_run_failed` stayed false, so the
                // trailing idle then closed the turn as a SUCCESS.
                g_run_failed = true;
                g_state.stream_active = false;
                CloseAgentToolBlockLocked(false, now_ms);
                SetPhaseLocked(wqn::AiFeaturePhase::kError, "执行失败");
                g_state.ui.action_hint = "长按确认重试新任务";
                if (!event.text.empty()) {
                    g_state.ui.activity_text = event.text;
                }
            } else if (event.status == "retry") {
                SetPhaseLocked(wqn::AiFeaturePhase::kRunning,
                               g_observing ? "观察中" : "Agent 重试中");
                g_state.ui.activity_text = event.text;
            } else {
                SetPhaseLocked(wqn::AiFeaturePhase::kRunning,
                               g_observing ? "观察中" : "Agent 执行中");
                // Status text (gateway hints, upstream status messages) is
                // transient context; the next tool/text event replaces it.
                if (!event.text.empty()) {
                    g_state.ui.activity_text = event.text;
                }
            }
            break;
        case wqn::OpenCodeEventKind::kTextDelta:
            if (g_state.ui.response_text.size() < kMaxAgentTextBytes) {
                const size_t remaining = kMaxAgentTextBytes - g_state.ui.response_text.size();
                g_state.ui.response_text.append(
                    event.text.data(), Utf8SafePrefixBytes(event.text, remaining));
            }
            MirrorAgentTextLocked(now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kText:
            g_state.ui.response_text.assign(
                event.text.data(),
                Utf8SafePrefixBytes(event.text, kMaxAgentTextBytes));
            MirrorAgentTextLocked(now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kReasoningDelta:
            // Reasoning is bounded by its own budget, never by the answer's.
            if (g_agent_thinking_text.size() < kMaxThinkingBytes) {
                const size_t remaining = kMaxThinkingBytes - g_agent_thinking_text.size();
                g_agent_thinking_text.append(
                    event.text.data(), Utf8SafePrefixBytes(event.text, remaining));
                MirrorAgentThinkingLocked(now_ms);
            }
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kReasoning:
            g_agent_thinking_text.assign(
                event.text.data(),
                Utf8SafePrefixBytes(event.text, kMaxThinkingBytes));
            MirrorAgentThinkingLocked(now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kTool: {
            g_state.ui.activity_text = event.tool;
            if (!event.status.empty()) {
                g_state.ui.activity_text += " · " + event.status;
            }
            if (!event.preview.empty()) {
                g_state.ui.activity_text += " · " + event.preview;
            }
            // Coalesce one tool call into one block. The key is the call id when
            // the gateway learned it: a run that reads two files re-emits the
            // same tool name twice, and merging by name alone collapsed both
            // into a single block whose detail was the last call's. When the id
            // is unknown (mid-run attach) fall back to the name, which is the
            // most a v0 conversation can distinguish.
            const bool same_call = g_agent_tool_id != wqn::kInvalidChatMessageId &&
                                   !event.call_id.empty() && g_agent_tool_call == event.call_id;
            const bool same_name = g_agent_tool_id != wqn::kInvalidChatMessageId &&
                                   event.call_id.empty() && g_agent_tool_name == event.tool;
            if (same_call || same_name) {
                // Same block: this status is the one it will be closed with.
                g_agent_tool_ok = event.status != "error";
                g_agent_tool_detail = event.preview.empty() ? event.status : event.preview;
            } else {
                // Close the previous block with the status it ended on. Setting
                // `g_agent_tool_ok` before this close marked a successful tool
                // as failed (and a failed one as successful) whenever two
                // different calls arrived back to back.
                CloseAgentToolBlockLocked(g_agent_tool_ok, now_ms);
                g_agent_tool_ok = event.status != "error";
                g_agent_tool_id = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent)
                                      .AppendToolStart(event.tool, std::string_view(), now_ms);
                g_agent_tool_name = event.tool;
                g_agent_tool_call = event.call_id;
                g_agent_tool_detail = event.preview.empty() ? event.status : event.preview;
                g_agent_tool_since_ms = now_ms;
            }
            MarkChangedLocked();
            break;
        }
        case wqn::OpenCodeEventKind::kPermission:
            // The option bar has one mode, so a permission supersedes a question
            // the device is still holding. The gateway re-arms a question that
            // is still pending once this permission is answered (it re-reads
            // liveness every poll round), so dropping it here loses nothing --
            // and keeping it would leave both asks live at once, with the
            // question unanswerable behind the permission.
            ClearPendingQuestionLocked();
            ClearDeferredQuestionLocked();
            // Only the question flight: a question reply already on the wire
            // must not restore a question this permission has superseded. The
            // permission flight (if any) is a different reply.
            g_reply_flight_question_id.clear();
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingPermission;
            g_state.ui.status_label = "等待权限";
            g_state.pending_permission_id = event.permission_id;
            // Which session the ask belongs to, not which one we attached to. A
            // subagent's permission must be answered on the subagent's own id or
            // the reply 404s.
            g_state.pending_permission_session = event.session_id;
            g_state.ui.activity_text = event.text;
            if (!event.preview.empty()) {
                g_state.ui.activity_text += " · " + event.preview;
            }
            g_state.ui.action_hint = "↑ 批准 · ↓ 拒绝";
            // Deliberately NOT mirrored into history: the ask is a live
            // interaction rendered by the dashed bubble + option bar, and its
            // outcome already shows up as the tool blocks and text that follow.
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kQuestion:
            // A question and a permission can never be live at once: the option
            // bar has one mode, and the session state has one pending ask.
            // Deferring -- not dropping -- is what keeps the run alive: a dropped
            // ask is lost for good, because the gateway has already marked it
            // seen and will not re-send it, so the run sits behind a question the
            // user was never shown.
            if (!g_state.pending_permission_id.empty()) {
                if (g_deferred_question_valid) {
                    ESP_LOGW(kTag, "question %s dropped: a permission and a question are already pending",
                             event.question_id.c_str());
                } else {
                    g_deferred_question_valid = true;
                    g_deferred_question_id = event.question_id;
                    g_deferred_question_title = event.text;
                    g_deferred_question_options = event.question_options;
                    // Deferring carries the session with the ask: the promote
                    // below copies it straight into the pending ask, so a
                    // subagent's question stays answerable on its own id.
                    g_deferred_question_session = event.session_id;
                    ESP_LOGI(kTag, "question %s held until the permission ask is answered",
                             event.question_id.c_str());
                }
                break;
            }
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingQuestion;
            g_state.ui.status_label = "等待回答";
            g_state.pending_question_id = event.question_id;
            g_state.pending_question_title = event.text;
            g_state.pending_question_options = event.question_options;
            g_state.pending_question_session = event.session_id;
            g_state.ui.activity_text = event.text;
            g_state.ui.action_hint = "↑/↓ 选择 · 确认回答";
            // Not mirrored for the same reason a permission ask is not.
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kError:
            // An error is not the end of a run by itself: the gateway projects a
            // retryable upstream step failure as `agent.error` and keeps the
            // stream open. Treating every error as terminal locked the run into
            // 失败, so a step that retried successfully still ended shown as a
            // failure -- and `g_run_failed` then closed every later tool block as
            // an error too.
            AppendAgentErrorLocked(event.text, now_ms);
            if (event.fatal) {
                // The run is over, so the ask and its in-flight reply are over
                // with it: nothing can answer them any more.
                ClearAllAsksLocked();
                g_run_failed = true;
                g_state.ui.phase = wqn::AiFeaturePhase::kError;
                g_state.ui.status_label = "执行失败";
                g_state.ui.action_hint = "长按确认重试新任务";
            } else {
                // Keep running: the error is recorded, the run continues. A live
                // ask stays armed on purpose -- disarming it would leave the run
                // blocked behind an ask the gateway has already marked seen and
                // will not re-send.
                g_state.ui.activity_text = event.text;
                if (g_state.pending_permission_id.empty() &&
                    g_state.pending_question_id.empty()) {
                    g_state.ui.phase = wqn::AiFeaturePhase::kRunning;
                    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
                    g_state.ui.action_hint.clear();
                }
            }
            MarkChangedLocked();
            break;
    }
    xSemaphoreGive(g_lock);
}

void OnOpenCodeReplyFailed(
    const wqn::OpenCodeOutboundReply& reply, esp_err_t error, void*)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // Match against the reply that is actually in flight, not against the live
    // ask: the option bar is closed as soon as the reply is queued, so the old
    // "is this ask still pending?" test read "superseded" on every outcome and
    // the restore below was unreachable. A failed reply left the run blocked
    // behind an ask the device could no longer answer, until the stream timed
    // out -- which v1 could recover from and v2 could not.
    // The flight id is the evidence, not the phase. Requiring kRunning made the
    // restore unreachable for a permission reply that arrives while a deferred
    // question holds the bar: the permission reply path promotes that question
    // (kRunning -> kAwaitingQuestion) before the POST is even attempted, so the
    // phase was already kAwaitingQuestion by the time the failure came back.
    // The flight id is safe on its own because every terminal path -- run end,
    // fatal error, observe teardown -- calls ClearReplyFlightLocked(), so a live
    // flight id can only mean "our reply, and its ask is not known to be over".
    const bool in_flight = reply.is_question
        ? g_reply_flight_question_id == reply.question_id
        : g_reply_flight_permission_id == reply.permission_id;
    if (in_flight) {
        if (reply.is_question) {
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingQuestion;
            g_state.ui.status_label = "回答失败";
            g_state.ui.activity_text = "回答未送达，可重试";
            g_state.ui.action_hint = "↑/↓ 选择 · 确认回答";
        } else {
            // The permission was never answered, so it takes the bar back. A
            // question that has meanwhile claimed the bar -- promoted from the
            // deferred slot when the reply cleared the permission, or arrived
            // fresh while the reply was on the wire -- goes back to deferred
            // rather than staying armed under a permission bar it cannot be
            // answered from. Leaving it armed would strand it: the pending id
            // blocks the question slot for the rest of the run, and
            // PromoteDeferredQuestionLocked refuses to promote while it is set.
            if (!g_state.pending_question_id.empty()) {
                if (g_deferred_question_valid) {
                    // Two questions plus a permission in one round. The gateway
                    // arms at most one ask, so this cannot happen against a
                    // compliant relay; keeping the armed one is still the safer
                    // choice, because it is the one the user is looking at.
                    ESP_LOGW(kTag,
                             "question %s displaced by %s in the deferred slot",
                             g_deferred_question_id.c_str(),
                             g_state.pending_question_id.c_str());
                }
                g_deferred_question_valid = true;
                g_deferred_question_id = std::move(g_state.pending_question_id);
                g_deferred_question_title = std::move(g_state.pending_question_title);
                g_deferred_question_options =
                    std::move(g_state.pending_question_options);
                g_deferred_question_session =
                    std::move(g_state.pending_question_session);
                ESP_LOGI(kTag, "question %s held again: the permission reply failed",
                         g_deferred_question_id.c_str());
                ClearPendingQuestionLocked();
            }
            g_state.pending_permission_id = reply.permission_id;
            // Restore the owning session along with the id, so the retry POSTs
            // to the session that raised the ask rather than to the one attached
            // when the failure came back.
            g_state.pending_permission_session = reply.session_id;
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingPermission;
            g_state.ui.status_label = "权限回复失败";
            g_state.ui.activity_text = reply.approve ? "批准未送达，可重试"
                                                     : "拒绝未送达，可重试";
            g_state.ui.action_hint = "↑ 批准 · ↓ 拒绝";
        }
        MarkChangedLocked();
    } else {
        ESP_LOGW(kTag, "%s reply failed: %s",
                 reply.is_question ? "question" : "permission",
                 esp_err_to_name(error));
    }
    xSemaphoreGive(g_lock);
}

void RunPrompt()
{
    g_interrupt_requested.store(false, std::memory_order_release);
    g_interrupt_delivered.store(false, std::memory_order_release);
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::RunOpenCodePrompt(
            token,
            g_run_session_id,
            g_run_detail,
            g_run_prompt,
            &g_outbound_replies,
            OnOpenCodeReplyFailed,
            nullptr,
            &g_interrupt_requested,
            &g_interrupt_delivered,
            OnOpenCodeEvent,
            nullptr,
            &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool refreshing_history = false;
    if (g_interrupt_delivered.load(std::memory_order_acquire)) {
        // Stopping the run on request is a success, not a failure.
        // The run is over, so any ask it raised is dead too -- and the outbound
        // queue that would carry the reply is discarded right below, so a bar
        // left armed here would answer nothing.
        ClearAllAsksLocked();
        g_state.ui.phase = wqn::AiFeaturePhase::kComplete;
        g_state.ui.status_label = "已中止";
        g_state.ui.activity_text = "任务已按确认键中止";
        g_state.ui.action_hint = "长按确认发起新任务";
        g_state.stream_active = false;
        ReleaseWorkOwnershipLocked();
        MarkChangedLocked();
    } else if (result != ESP_OK && !g_run_failed) {
        SetErrorLocked(api_result.detail.empty() ? "Agent 执行连接失败" : api_result.detail);
    } else {
        g_state.stream_active = false;
        // [detail] The brief tier hides tool work from the live stream, so the
        // digest that stands in for it exists only in the history projection.
        // Refresh the list behind the finished run instead of releasing the
        // session here: the chained kLoadHistory hands the ownership back when
        // it is done, and it needs g_run_session_id until then.
        if (g_run_detail < 1 && !g_run_failed) {
            refreshing_history = true;
            g_history_refresh = true;
            ChainWorkerCommandLocked(WorkerCommand::kLoadHistory);
        } else {
            ReleaseWorkOwnershipLocked();
        }
        MarkChangedLocked();
    }
    if (!refreshing_history) {
        g_run_session_id.clear();
    }
    g_run_prompt.clear();
    xSemaphoreGive(g_lock);
    DiscardOutboundReplies();
}

void CreateSession()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    wqn::OpenCodeSessionInfo created;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::CreateOpenCodeSession(token, &created, &api_result);
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result == ESP_OK) {
        g_state.current_session_id = created.id;
        g_state.current_session_title = created.title;
        g_state.session_locked = true;
        g_observing = false;
        ClearAllAsksLocked();
        // Nothing to backfill in an empty session; mark it loaded so the first
        // observe attaches straight to the stream.
        g_state.history_loaded_session_id = created.id;
        g_state.ui.context_label = created.title;
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "就绪";
        g_state.ui.activity_text = "新 Session 已创建";
        g_state.ui.action_hint = "长按确认录音 · 双击确认观察";
        g_state.ui.prompt_text.clear();
        g_state.ui.response_text.clear();
        g_state.ui.scroll_offset_lines = 0;
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        ResetAgentHistoryTurnLocked();
        MarkChangedLocked();
        ReleaseWorkOwnershipLocked();
    } else {
        SetErrorLocked(api_result.detail.empty() ? "Session 创建失败" : api_result.detail);
    }    xSemaphoreGive(g_lock);
}

void ObserveSession()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK && g_run_session_id.empty()) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        result = wqn::WatchOpenCodeSession(
            token,
            g_run_session_id,
            g_run_detail,
            &g_outbound_replies,
            OnOpenCodeReplyFailed,
            nullptr,
            &g_interrupt_requested,
            &g_interrupt_delivered,
            OnOpenCodeEvent,
            nullptr,
            &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_interrupt_delivered.load(std::memory_order_acquire)) {
        // Same reasoning as the run path: the run is over and the outbound
        // queue is discarded below, so a surviving ask could not be answered.
        ClearAllAsksLocked();
        g_state.stream_active = false;
        ReleaseWorkOwnershipLocked();
        MarkChangedLocked();
    } else if (result != ESP_OK && !g_run_failed) {
        SetErrorLocked(api_result.detail.empty() ? "观察连接失败" : api_result.detail);
    } else {
        g_state.stream_active = false;
        ReleaseWorkOwnershipLocked();
        MarkChangedLocked();
    }
    g_run_session_id.clear();
    g_observing = false;
    xSemaphoreGive(g_lock);
    DiscardOutboundReplies();
}

// Backfills the transcript of the session the caller is about to attach to.
// Failure is a warning, not an error: observing a live run must still work when
// the history read fails, and the live deltas that follow are the authoritative
// view anyway.
void LoadHistory()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    std::vector<wqn::OpenCodeHistoryMessage> messages;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::GetOpenCodeHistory(
            token, g_run_session_id, g_run_detail, &messages, &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool refresh = g_history_refresh;
    g_history_refresh = false;
    // A lock that switched sessions while this read was in flight invalidates
    // it: the transcript must never claim a session it was not read from.
    if (result == ESP_OK && g_run_session_id != g_state.current_session_id) {
        ESP_LOGW(kTag, "history backfill dropped: %s is no longer current",
                 g_run_session_id.c_str());
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        if (refresh) {
            // Post-run refresh: the channel holds the live transcript of the
            // run that just finished, and the backfill replaces it with the
            // projection -- at the brief tier the digest exists only here.
            // Clearing only on success means a failed read leaves the live
            // transcript on screen.
            wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
        }
        // Reset first, then append, then let the caller subscribe: the reverse
        // order lets the first live delta land on a backfilled message id and
        // overwrite history with new text.
        ResetAgentHistoryTurnLocked();
        BackfillAgentHistoryLocked(messages, esp_timer_get_time() / 1000);
        g_state.history_loaded_session_id = g_run_session_id;
        if (!g_observing && !refresh) {
            // Lock-triggered backfill: the stream was never the goal, so hand
            // the session back ready to prompt. A post-run refresh is not the
            // lock's backfill: the status bar already reported the run's own
            // terminal state, and a background re-read must not relabel it.
            g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
            g_state.ui.status_label = "就绪";
            g_state.ui.activity_text = "长按确认键语音输入";
        }
        MarkChangedLocked();
    } else {
        ESP_LOGW(kTag, "history backfill failed for %s: %s (%s)",
                 g_run_session_id.c_str(), api_result.error_code.c_str(),
                 api_result.detail.c_str());
        if (!refresh) {
            // Failure must not leave a busy phase behind: an observing caller
            // would sit on "观察中" with no stream attached, and a lock would
            // stay busy. A post-run refresh has neither problem -- the run
            // already reached its terminal UI and the live transcript is
            // intact -- so its failure is the log line above and nothing more.
            g_state.ui.phase = wqn::AiFeaturePhase::kError;
            g_state.ui.status_label = g_observing ? "观察失败" : "历史读取失败";
            g_state.ui.activity_text =
                g_observing ? "历史读取失败" : "可重新选择 Session 重试";
            g_state.stream_active = false;
            g_observing = false;
            MarkChangedLocked();
        }
    }
    if (result == ESP_OK && g_observing) {
        // Chain the observe stream behind the backfill: the stream must not
        // attach until the history it precedes is already in the channel.
        // This runs inside the kLoadHistory command, so the chain must bypass
        // ArmWorkerLocked's "worker is free" guard -- FinishCommand only
        // clears the command it just ran, so it leaves this one armed.
        g_run_session_id = g_state.current_session_id;
        g_state.ui.activity_text = "正在连接 Session 事件流";
        MarkChangedLocked();
        ChainWorkerCommandLocked(WorkerCommand::kObserveSession);
        xSemaphoreGive(g_lock);
        return;
    }
    g_run_session_id.clear();
    xSemaphoreGive(g_lock);
    ReleaseWorkOwnershipLocked();
}

void WorkerTask(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(g_lock, portMAX_DELAY);
        const WorkerCommand command = g_command;
        xSemaphoreGive(g_lock);
        switch (command) {
            case WorkerCommand::kLoadSessions:
                LoadSessions();
                break;
            case WorkerCommand::kCreateSession:
                CreateSession();
                break;
            case WorkerCommand::kPrepareCapture:
                PrepareCapture();
                break;
            case WorkerCommand::kTranscribe:
                Transcribe();
                break;
            case WorkerCommand::kCancelVoice:
                TearDownVoiceCapture();
                break;
            case WorkerCommand::kRunPrompt:
                RunPrompt();
                break;
            case WorkerCommand::kObserveSession:
                ObserveSession();
                break;
            case WorkerCommand::kLoadHistory:
                LoadHistory();
                break;
            case WorkerCommand::kNone:
                break;
        }
        FinishCommand(command);
    }
}

esp_err_t AcquireAgentLeaseLocked()
{
    if (g_agent_sleep_lease) {
        return ESP_OK;
    }
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kAiSession,
        "opencode-agent",
        __FILE__,
        __LINE__);
    if (!lease) {
        return ESP_ERR_INVALID_STATE;
    }
    g_agent_sleep_lease = std::move(lease);
    return ESP_OK;
}

void DiscardOutboundReplies()
{
    wqn::OpenCodeOutboundReply discard;
    while (g_outbound_replies.Pop(&discard)) {
    }
}

}  // namespace

namespace wqn {

esp_err_t InitOpenCodeSession()
{
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
        if (g_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        g_state.ui.title = "OpenCode";
        g_state.ui.status_label = "未加载";
        g_state.ui.activity_text = "进入页面后加载 Session";
        g_state.ui.action_hint = "长按上下键切换页面";
        g_changed = true;
    }
    if (g_worker == nullptr) {
        g_worker = xTaskCreateStatic(
            WorkerTask,
            "wqn_agent",
            kWorkerStackBytes,
            nullptr,
            5,
            g_worker_stack,
            &g_worker_tcb);
        if (g_worker == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t RequestOpenCodeSessionList()
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && !ArmWorkerLocked(WorkerCommand::kLoadSessions)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_state.ui.phase = AiFeaturePhase::kLoading;
        g_state.ui.status_label = "加载 Session";
        g_state.ui.activity_text = "正在连接 WQN Agent 网关";
        g_state.ui.action_hint.clear();
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t MoveOpenCodeSessionSelection(int direction)
{
    if (g_lock == nullptr || direction == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.session_locked || g_state.sessions.empty()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const int count = static_cast<int>(g_state.sessions.size());
    const int current = static_cast<int>(g_state.selected_session);
    g_state.selected_session = static_cast<size_t>((current + direction + count) % count);
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t LockSelectedOpenCodeSession()
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.sessions.empty() || g_state.selected_session >= g_state.sessions.size()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }
    // Ownership first: locking arms the backfill, so a lock that cannot start
    // one must not touch any state at all. Mutating first and failing later
    // would leave the view pointing at a session whose transcript and stream
    // still belong to the previous one.
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const AgentSessionOption& selected = g_state.sessions[g_state.selected_session];
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK) {
        // The worker reads the target session from g_run_session_id, the same
        // handoff slot ObserveOpenCodeSession uses.
        g_run_session_id = selected.id;
        g_run_detail = g_state.detail_level;
        if (!ArmWorkerLocked(WorkerCommand::kLoadHistory)) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result != ESP_OK) {
        g_run_session_id.clear();
        ReleaseWorkOwnershipLocked();
        xSemaphoreGive(g_lock);
        return result;
    }
    g_state.current_session_id = selected.id;
    g_state.current_session_title = selected.title;
    g_state.session_locked = true;
    g_observing = false;
    ClearAllAsksLocked();
    // The mirrored transcript belongs to the session that produced it: drop the
    // backfill marker so the load armed above re-reads it for this session.
    g_state.history_loaded_session_id.clear();
    g_state.ui.context_label = selected.title;
    g_state.ui.phase = AiFeaturePhase::kLoading;
    g_state.ui.status_label = "读取历史";
    g_state.ui.activity_text = "正在读取历史对话";
    g_state.ui.action_hint = "长按确认录音 · ↑/↓ 滚动 · 双击观察";
    g_state.ui.prompt_text.clear();
    g_state.ui.response_text.clear();
    g_state.ui.requires_confirmation = false;
    g_state.ui.scroll_offset_lines = 0;
    g_state.confirmation_armed_at_ms = 0;
    // [agent] Switching sessions switches conversation: the mirrored transcript
    // belongs to the session that produced it, so drop it rather than letting
    // the new session render the previous one's bubbles.
    wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
    ResetAgentHistoryTurnLocked();
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t CreateNewOpenCodeSession()
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && !ArmWorkerLocked(WorkerCommand::kCreateSession)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_observing = false;
        g_state.ui.phase = AiFeaturePhase::kLoading;
        g_state.ui.status_label = "创建 Session";
        g_state.ui.activity_text = "正在通过 WQN 网关新建";
        g_state.ui.action_hint.clear();
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t ObserveOpenCodeSession()
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.current_session_id.empty()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    // Backfill runs once per session, before the stream attaches. Repeating it
    // on every reconnect would append the same turns twice and fight the local
    // copy of the prompt the device already appended when it was submitted.
    const bool needs_history =
        g_state.history_loaded_session_id != g_state.current_session_id;
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && g_command != WorkerCommand::kNone) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_run_failed = false;
        g_observing = true;
        // The worker reads the target session from g_run_session_id, the same
        // handoff slot ConfirmOpenCodePrompt uses.
        g_run_session_id = g_state.current_session_id;
        // [detail] The chained kObserveSession reuses this value: the command
        // gate is single-slot, so nothing can re-arm between the backfill and
        // the attach that follows it.
        g_run_detail = g_state.detail_level;
        // Attaching is a fresh turn: an ask the previous attach left armed is
        // stale, and a gateway re-attach re-discovers its own pending asks --
        // holding one would swallow the fresh copy for the slot it occupies.
        ClearAllAsksLocked();
        // Observe locks the session so the interaction view (not the picker)
        // renders while the stream is attached; the lock persists afterwards
        // so the observed session can immediately be prompted as well.
        g_state.session_locked = true;
        g_state.ui.phase = AiFeaturePhase::kRunning;
        g_state.ui.status_label = "观察中";
        g_state.ui.response_text.clear();
        g_state.ui.activity_text = needs_history
            ? "正在读取历史对话"
            : "正在连接 Session 事件流";
        g_state.ui.action_hint.clear();
        g_state.ui.scroll_offset_lines = 0;
        // [follow] Attaching is a fresh turn (see ArmAgentFollowLocked): the
        // viewport watches the stream and parks on the newest answer once it
        // has a body. A session whose newest entry is already a full answer
        // retires the follow on the very next tick, so nothing moves.
        ArmAgentFollowLocked();
        g_state.stream_active = true;
        // Attaching mid-stream: any assistant id from the previous run is stale.
        ResetAgentHistoryTurnLocked();
        if (!ArmWorkerLocked(needs_history ? WorkerCommand::kLoadHistory
                                           : WorkerCommand::kObserveSession)) {
            g_run_session_id.clear();
            result = ESP_ERR_INVALID_STATE;
        }
        MarkChangedLocked();
    }
    if (result != ESP_OK) {
        g_observing = false;
        g_state.stream_active = false;
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t ReplyPendingOpenCodePermission(bool approve)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase != AiFeaturePhase::kAwaitingPermission ||
        g_state.pending_permission_id.empty()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_outbound_replies.Push(wqn::OpenCodeOutboundReply{
        g_state.pending_permission_id, approve, false, {},
        g_state.pending_permission_session, {}});
    // The reply on the wire is the only thing that proves the id was delivered.
    // The UI closes its option bar here, so the live id is deliberately not it.
    g_reply_flight_permission_id = g_state.pending_permission_id;
    ClearPendingPermissionLocked();
    g_state.ui.phase = AiFeaturePhase::kRunning;
    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
    g_state.ui.activity_text = approve ? "已批准权限" : "已拒绝权限";
    g_state.ui.action_hint.clear();
    // [follow] The run continues after the ask: watch it again from the tail
    // and park on the answer body when it lands.
    ArmAgentFollowLocked();
    PromoteDeferredQuestionLocked();
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t ReplyPendingOpenCodeQuestion(int index)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase != AiFeaturePhase::kAwaitingQuestion ||
        g_state.pending_question_id.empty() ||
        index < 0 || index >= static_cast<int>(g_state.pending_question_options.size())) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_ARG;
    }
    // The answer is the option's value, never its label and never a field id:
    // the gateway is what knows which upstream field the option came from.
    const std::string answer = g_state.pending_question_options[index].value;
    wqn::OpenCodeOutboundReply reply;
    reply.is_question = true;
    reply.question_id = g_state.pending_question_id;
    // The owning session travels with the ask, so a subagent's question is
    // answered on the subagent's own id rather than on the attached one.
    reply.session_id = g_state.pending_question_session;
    reply.answer = answer;
    // Capture the id before the queue takes the reply. `Push` takes it by
    // value, and a libstdc++ string move empties the source, so reading
    // `reply.question_id` after the push below yields "". The flight id would
    // then never match the id inside the queue, `in_flight` would be false
    // forever, and a failed question reply could not restore the bar -- the one
    // path that never had this bug until the shared id was introduced.
    const std::string flight_id = reply.question_id;
    g_outbound_replies.Push(std::move(reply));
    g_reply_flight_question_id = flight_id;
    g_state.ui.phase = AiFeaturePhase::kRunning;
    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
    g_state.ui.activity_text = "已回答：" + answer;
    g_state.ui.action_hint.clear();
    // [follow] Same as the permission reply: the run resumes, so watch it.
    ArmAgentFollowLocked();
    ClearDeferredQuestionLocked();
    // The pending ask stays armed until a terminal status: if the reply POST
    // fails, OnOpenCodeReplyFailed restores exactly this state for a retry.
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

void InterruptOpenCodeRun()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_state.stream_active) {
        // No stream to break: an interrupt for a run nobody started is a no-op,
        // and the worker would consume the flag on its next (unrelated) stream.
        g_interrupt_requested.store(false, std::memory_order_release);
        xSemaphoreGive(g_lock);
        return;
    }
    // Flag only. The streaming worker makes the interrupt POST itself, so the
    // UI thread never opens a connection of its own while a stream is attached.
    g_interrupt_requested.store(true, std::memory_order_release);
    g_state.ui.status_label = "正在中止";
    g_state.ui.activity_text = "已请求中止当前任务";
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
}

esp_err_t StartOpenCodeVoiceInput()
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_state.session_locked || !AiFeatureCanStartVoiceInput(g_state.ui.phase)) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool append_to_pending_prompt =
        g_state.ui.phase == AiFeaturePhase::kAwaitingConfirmation;
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK) {
        g_recording_requested = true;
        if (!ArmWorkerLocked(WorkerCommand::kPrepareCapture)) {
            g_recording_requested = false;
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result == ESP_OK) {
        // [voice-pipe] A fresh capture starts from a clean voice slate: no
        // partial text, no stale stream error, no abort left armed.
        g_voice_last_error.clear();
        g_voice_abort_requested = false;
        g_state.ui.voice_partial.clear();
        if (!append_to_pending_prompt) {
            g_state.ui.prompt_text.clear();
            g_state.ui.scroll_offset_lines = 0;
        }
        // [follow] Capture arms the follow so the viewport is already watching
        // when the transcript is confirmed. The per-tick step skips the capture
        // phases outright (the codec is being configured and must not share the
        // panel with a repaint), so this only takes effect once the transcript
        // is back -- by which point the newest content is what the user needs.
        ArmAgentFollowLocked();
        g_state.confirmation_armed_at_ms = 0;
        g_state.ui.phase = AiFeaturePhase::kLoading;
        g_state.ui.status_label = "准备录音";
        g_state.ui.activity_text = "正在连接 WiFi 与麦克风";
        g_state.ui.action_hint = "保持按住确认键";
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t StopOpenCodeVoiceInput()
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase == AiFeaturePhase::kLoading && g_recording_requested) {
        g_recording_requested = false;
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.ui.phase != AiFeaturePhase::kRecording) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_recording_requested = false;
    g_command = WorkerCommand::kTranscribe;
    g_state.ui.phase = AiFeaturePhase::kTranscribing;
    g_state.ui.status_label = "语音转写中";
    g_state.ui.activity_text = "转写完成后必须确认才会发送";
    g_state.ui.action_hint.clear();
    MarkChangedLocked();
    xTaskNotifyGive(g_worker);
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

void CancelAgentVoiceInput()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const wqn::AiFeaturePhase phase = g_state.ui.phase;
    if (phase == wqn::AiFeaturePhase::kRecording) {
        // Hard abort: the audio is discarded and the WS turn is torn down. The
        // worker usually owns no command in this phase, but the tail of
        // kPrepareCapture (bringing the WS turn up) still does -- it checks the
        // flag after the turn exists and tears it down itself.
        g_voice_abort_requested = true;
        if (g_command == WorkerCommand::kNone) {
            ArmWorkerLocked(WorkerCommand::kCancelVoice);
        }
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已取消录音";
        g_state.ui.activity_text = "录音已丢弃";
        g_state.ui.action_hint = "长按确认重新录音";
        g_state.ui.voice_partial.clear();
        MarkChangedLocked();
    } else if (phase == wqn::AiFeaturePhase::kTranscribing) {
        // Soft cancel: the finalize stops waiting and drops whatever the
        // stream returns; the turn is left to finish on the transport's own
        // time. A prompt re-record falls back to the batch endpoint when the
        // transport is still busy.
        wqn::AgentVoiceRequestCancel();
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已取消";
        g_state.ui.activity_text = "语音内容未发送";
        g_state.ui.action_hint = "长按确认重新录音";
        g_state.ui.voice_partial.clear();
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

esp_err_t ConfirmOpenCodePrompt(int64_t confirmed_at_ms)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!AiFeatureCanSubmit(g_state.ui) || g_state.current_session_id.empty() ||
        g_state.confirmation_armed_at_ms <= 0 ||
        confirmed_at_ms < g_state.confirmation_armed_at_ms) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK) {
        g_run_session_id = g_state.current_session_id;
        g_run_prompt = g_state.ui.prompt_text;
        g_run_detail = g_state.detail_level;
        if (!ArmWorkerLocked(WorkerCommand::kRunPrompt)) {
            g_run_session_id.clear();
            g_run_prompt.clear();
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result == ESP_OK) {
        g_run_failed = false;
        // A submitted run is a fresh turn. Ask state the previous turn left
        // behind cannot be answered any more -- the gateway it belonged to is
        // gone -- and the re-attached gateway re-discovers its own pending asks.
        // That includes the two live slots: a bar left armed from the previous
        // turn would hide this run's first ask and answer nothing.
        ClearAllAsksLocked();
        g_state.ui.phase = AiFeaturePhase::kSubmitting;
        g_state.ui.status_label = "正在提交";
        g_state.ui.response_text.clear();
        g_state.ui.activity_text = "WQN 正在中转到 OpenCode";
        g_state.ui.action_hint.clear();
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        g_state.ui.scroll_offset_lines = 0;
        // [follow] Submit starts the turn the viewport should watch.
        ArmAgentFollowLocked();
        g_state.stream_active = true;
        // [agent] Mirror the submitted prompt into the kAgent channel before the
        // worker starts, so the user bubble is on screen while the gateway is
        // still connecting.
        AppendAgentUserLocked(g_run_prompt, esp_timer_get_time() / 1000);
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

void CancelOpenCodePrompt()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase == AiFeaturePhase::kAwaitingConfirmation) {
        g_state.ui.prompt_text.clear();
        g_state.ui.phase = AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已取消";
        g_state.ui.activity_text = "语音内容未发送";
        g_state.ui.action_hint = "长按确认重新录音";
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        // Nothing was sent, so the armed turn never reached history.
        ResetAgentHistoryTurnLocked();
        MarkChangedLocked();
        xSemaphoreGive(g_lock);
        return;
    }
    const bool attached = g_state.stream_active;
    xSemaphoreGive(g_lock);
    if (attached) {
        // A submitted run is stopped through the interrupt path; the stream
        // worker performs the POST and reports back through ReadAgentEventStream.
        InterruptOpenCodeRun();
    }
}

// [scroll-clamp] The offset lives in the AI page viewport's coordinate space,
// where 0 is the anchor (newest question at the top) and NEGATIVE values scroll
// further down to the tail of the newest reply. Only the renderer knows how tall
// the history is, so the backend cannot clamp this itself: the caller passes the
// bounds it already computed for the STD/Pro path (GetAiScrollBounds) and both
// tiers share one clamp.
//
// The old `std::max<int32_t>(0, next)` was correct for the retired standalone
// page, which measured the offset as "distance from latest" and therefore
// wanted a floor of 0. On the AI page that same floor made the bottom of the
// newest reply unreachable, which read as "cannot scroll to the very bottom".
void ScrollOpenCodeResponse(int direction, int32_t min_scroll, int32_t max_scroll)
{
    if (g_lock == nullptr || direction == 0) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    int32_t next = g_state.ui.scroll_offset_lines + direction * 4;
    if (next < min_scroll) {
        next = min_scroll;
    }
    if (next > max_scroll) {
        next = max_scroll;
    }
    // Only mark changed when the offset actually moved: a no-op press at either
    // bound must not cost an EPD refresh.
    if (next != g_state.ui.scroll_offset_lines) {
        g_state.ui.scroll_offset_lines = next;
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

bool CopyOpenCodeSessionToUi(AgentSessionState* state)
{
    if (g_lock == nullptr || state == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool changed = g_changed;
    if (changed) {
        *state = g_state;
        g_changed = false;
    }
    xSemaphoreGive(g_lock);
    return changed;
}

void SetOpenCodeScrollOffsetClamped(int32_t target, int32_t min_scroll, int32_t max_scroll)
{
    if (g_lock == nullptr) {
        return;
    }
    if (min_scroll > max_scroll) {
        return;  // degenerate bounds: fail open, leave the offset untouched
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // Read-clamp-write stays inside ONE lock hold: a split Get/Set across locks
    // would let a streaming auto-follow interleave between them.
    if (target > max_scroll) {
        target = max_scroll;
    }
    if (target < min_scroll) {
        target = min_scroll;
    }
    constexpr int32_t kMaxScrollRows = 4096;
    if (target > kMaxScrollRows) {
        target = kMaxScrollRows;
    }
    if (target < -kMaxScrollRows) {
        target = -kMaxScrollRows;
    }
    // Only mark changed when the offset actually moved: the per-tick follow
    // calls this every 50 ms while it is pinned, and a redundant mark would
    // repaint the panel on every tick.
    if (g_state.ui.scroll_offset_lines != target) {
        g_state.ui.scroll_offset_lines = target;
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

void SetOpenCodeFollowState(bool active, bool user_moved)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.follow_active != active || g_state.user_moved != user_moved) {
        g_state.follow_active = active;
        g_state.user_moved = user_moved;
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

void SetOpenCodeDetailLevel(uint8_t level)
{
    if (g_lock == nullptr || level > 2) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.detail_level != level) {
        g_state.detail_level = level;
        // The transcript cache key is the session id alone, so a tier change
        // invalidates what is mirrored: clearing it makes the next Lock/Observe
        // take the needs_history branch and re-read the session at the new tier.
        // The already-mirrored turns keep the old tier until then -- deliberately
        // no eager clear, so the user does not watch the panel empty out.
        g_state.history_loaded_session_id.clear();
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

}  // namespace wqn

#else  // !CONFIG_WQN_AGENT_ENABLE

// [agent] Agent tier compiled out. The public API keeps its signatures so the
// AI page's tier switch, status-bar slots, scroll branch and history mirror all
// build unchanged -- they simply never fire, because NextAiTier does not offer
// kAgent in this configuration. Every entry point refuses instead of touching
// the network, so a stale call cannot reach a gateway this build has no client
// for. Mirrors the stub half of ai_session.cpp.
namespace wqn {

esp_err_t InitOpenCodeSession() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t RequestOpenCodeSessionList() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t MoveOpenCodeSessionSelection(int) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t LockSelectedOpenCodeSession() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t CreateNewOpenCodeSession() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ObserveOpenCodeSession() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ReplyPendingOpenCodePermission(bool) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StartOpenCodeVoiceInput() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StopOpenCodeVoiceInput() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ConfirmOpenCodePrompt(int64_t) { return ESP_ERR_NOT_SUPPORTED; }
void CancelOpenCodePrompt() {}
void CancelAgentVoiceInput() {}
void ScrollOpenCodeResponse(int, int32_t, int32_t) {}
void SetOpenCodeScrollOffsetClamped(int32_t, int32_t, int32_t) {}
void SetOpenCodeFollowState(bool, bool) {}
void SetOpenCodeDetailLevel(uint8_t) {}
bool CopyOpenCodeSessionToUi(AgentSessionState*) { return false; }

}  // namespace wqn

#endif  // CONFIG_WQN_AGENT_ENABLE
