#include "opencode_session.h"

#include <algorithm>
#include <utility>

#if CONFIG_WQN_AGENT_ENABLE

#include "ai_session.h"
#include "ai_history.h"
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
constexpr size_t kMaxPromptBytes = 4096;
constexpr uint32_t kWorkerStackBytes = 9216;

enum class WorkerCommand : uint8_t {
    kNone,
    kLoadSessions,
    kCreateSession,
    kPrepareCapture,
    kTranscribe,
    kRunPrompt,
    kObserveSession,
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
wqn::OpenCodeOutboundQueue g_outbound_replies;
wqn::runtime::SleepLease g_agent_sleep_lease;
wqn::services::ConnectivityDemand g_connectivity_demand;

void DiscardOutboundReplies();

void MarkChangedLocked()
{
    g_changed = true;
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
            g_state.pending_permission_id.clear();
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
        // AudioCapture is shared with the legacy AI page, whose tap forwards
        // PCM to its Std/Pro WebSocket. Agent capture is ASR-only until the
        // user confirms, so isolate the microphone before starting it.
        if (wqn::IsAudioCaptureRunning()) {
            mic_busy = true;
            result = ESP_ERR_INVALID_STATE;
        } else {
            wqn::SetAiAudioCaptureTapEnabled(false);
            result = wqn::StartAudioCapture();
            if (result != ESP_OK) {
                wqn::SetAiAudioCaptureTapEnabled(true);
            }
        }
    }
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
        wqn::SetAiAudioCaptureTapEnabled(true);
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
    }
    xSemaphoreGive(g_lock);
}

void Transcribe()
{
    wqn::AudioCaptureChunk audio;
    esp_err_t result = wqn::StopAudioCapture(&audio);
    wqn::SetAiAudioCaptureTapEnabled(true);
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
        result = wqn::TranscribeOpenCodeAudio(token, audio, &transcript, &api_result);
    }
    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_recording_requested = false;
    if (result == ESP_OK &&
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
        SetErrorLocked(api_result.detail.empty() ? "语音转写失败" : api_result.detail);
    }
    xSemaphoreGive(g_lock);
}

// ---- History mirror --------------------------------------------------------
// [agent] The Agent tier is rendered by the AI page's chat-bubble viewport, so
// every stream event that changes what the user reads is mirrored into the
// kAgent history channel. The channel is chosen explicitly rather than read
// from the visible tier: a run started on one tier must never append to
// another tier's conversation. Everything below runs with g_lock held.

wqn::ChatMessageId g_agent_assistant_id = wqn::kInvalidChatMessageId;
wqn::ChatMessageId g_agent_tool_id = wqn::kInvalidChatMessageId;
std::string g_agent_tool_name;
// Last status/preview seen for the open tool block. The gateway's `agent.tool`
// event carries a free-form status string, not an explicit start/end pair, so
// the newest detail wins and the block is closed by whatever arrives next.
std::string g_agent_tool_detail;
int64_t g_agent_tool_since_ms = 0;

void ResetAgentHistoryTurnLocked()
{
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
    g_agent_tool_id = wqn::kInvalidChatMessageId;
    g_agent_tool_name.clear();
    g_agent_tool_detail.clear();
    g_agent_tool_since_ms = 0;
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
    CloseAgentToolBlockLocked(true, now_ms);
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
    if (!text.empty()) {
        wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).AppendAssistant(text, now_ms);
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
            g_state.pending_permission_id.clear();
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
                g_state.ui.response_text.append(event.text.data(), std::min(remaining, event.text.size()));
            }
            MirrorAgentTextLocked(now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kText:
            g_state.ui.response_text = event.text.substr(0, kMaxAgentTextBytes);
            MirrorAgentTextLocked(now_ms);
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
            // Coalesce by tool name: the gateway re-emits `agent.tool` as a
            // tool progresses, and one history block per tool keeps the
            // transcript readable on a 400x300 panel.
            if (g_agent_tool_id != wqn::kInvalidChatMessageId &&
                g_agent_tool_name == event.tool) {
                g_agent_tool_detail = event.preview.empty() ? event.status : event.preview;
            } else {
                CloseAgentToolBlockLocked(true, now_ms);
                g_agent_tool_id = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent)
                                      .AppendToolStart(event.tool, std::string_view(), now_ms);
                g_agent_tool_name = event.tool;
                g_agent_tool_detail = event.preview.empty() ? event.status : event.preview;
                g_agent_tool_since_ms = now_ms;
            }
            MarkChangedLocked();
            break;
        }
        case wqn::OpenCodeEventKind::kPermission:
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingPermission;
            g_state.ui.status_label = "等待权限";
            g_state.pending_permission_id = event.permission_id;
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
        case wqn::OpenCodeEventKind::kError:
            g_run_failed = true;
            g_state.pending_permission_id.clear();
            g_state.ui.phase = wqn::AiFeaturePhase::kError;
            g_state.ui.status_label = "执行失败";
            g_state.ui.activity_text = event.text;
            g_state.ui.action_hint = "长按确认重试新任务";
            AppendAgentErrorLocked(event.text, now_ms);
            MarkChangedLocked();
            break;
    }
    xSemaphoreGive(g_lock);
}

void OnOpenCodeReplyFailed(
    const wqn::OpenCodeOutboundReply& reply, esp_err_t error, void*)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase == wqn::AiFeaturePhase::kRunning &&
        g_state.pending_permission_id.empty()) {
        // No newer ask superseded this one: restore it so the reply can be
        // retried instead of leaving the run blocked behind a silent failure.
        g_state.pending_permission_id = reply.permission_id;
        g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingPermission;
        g_state.ui.status_label = "权限回复失败";
        g_state.ui.activity_text = reply.approve ? "批准未送达，可重试"
                                                 : "拒绝未送达，可重试";
        g_state.ui.action_hint = "↑ 批准 · ↓ 拒绝";
        MarkChangedLocked();
    } else {
        ESP_LOGW(kTag, "permission reply failed: %s", esp_err_to_name(error));
    }
    xSemaphoreGive(g_lock);
}

void RunPrompt()
{
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
            g_run_prompt,
            &g_outbound_replies,
            OnOpenCodeReplyFailed,
            nullptr,
            OnOpenCodeEvent,
            nullptr,
            &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK && !g_run_failed) {
        SetErrorLocked(api_result.detail.empty() ? "Agent 执行连接失败" : api_result.detail);
    } else {
        g_state.stream_active = false;
        ReleaseWorkOwnershipLocked();
        MarkChangedLocked();
    }
    g_run_session_id.clear();
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
        g_state.pending_permission_id.clear();
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
            &g_outbound_replies,
            OnOpenCodeReplyFailed,
            nullptr,
            OnOpenCodeEvent,
            nullptr,
            &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK && !g_run_failed) {
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
            case WorkerCommand::kRunPrompt:
                RunPrompt();
                break;
            case WorkerCommand::kObserveSession:
                ObserveSession();
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
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.sessions.empty() || g_state.selected_session >= g_state.sessions.size()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const AgentSessionOption& selected = g_state.sessions[g_state.selected_session];
    g_state.current_session_id = selected.id;
    g_state.current_session_title = selected.title;
    g_state.session_locked = true;
    g_observing = false;
    g_state.pending_permission_id.clear();
    g_state.ui.context_label = selected.title;
    g_state.ui.phase = AiFeaturePhase::kIdle;
    g_state.ui.status_label = "就绪";
    g_state.ui.activity_text = "长按确认键语音输入";
    g_state.ui.action_hint = "长按确认录音 · ↑/↓ 滚动 · 双击观察";
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
    if (g_command != WorkerCommand::kNone || g_state.current_session_id.empty()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && !ArmWorkerLocked(WorkerCommand::kObserveSession)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_run_failed = false;
        g_observing = true;
        // The worker reads the target session from g_run_session_id, the same
        // handoff slot ConfirmOpenCodePrompt uses.
        g_run_session_id = g_state.current_session_id;
        g_state.pending_permission_id.clear();
        // Observe locks the session so the interaction view (not the picker)
        // renders while the stream is attached; the lock persists afterwards
        // so the observed session can immediately be prompted as well.
        g_state.session_locked = true;
        g_state.ui.phase = AiFeaturePhase::kRunning;
        g_state.ui.status_label = "观察中";
        g_state.ui.response_text.clear();
        g_state.ui.activity_text = "正在连接 Session 事件流";
        g_state.ui.action_hint.clear();
        g_state.ui.scroll_offset_lines = 0;
        g_state.stream_active = true;
        // Attaching mid-stream: any assistant id from the previous run is stale.
        ResetAgentHistoryTurnLocked();
        MarkChangedLocked();
    } else {
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
        g_state.pending_permission_id, approve});
    g_state.pending_permission_id.clear();
    g_state.ui.phase = AiFeaturePhase::kRunning;
    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
    g_state.ui.activity_text = approve ? "已批准权限" : "已拒绝权限";
    g_state.ui.action_hint.clear();
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
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
        if (!append_to_pending_prompt) {
            g_state.ui.prompt_text.clear();
            g_state.ui.scroll_offset_lines = 0;
        }
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
        if (!ArmWorkerLocked(WorkerCommand::kRunPrompt)) {
            g_run_session_id.clear();
            g_run_prompt.clear();
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result == ESP_OK) {
        g_run_failed = false;
        g_state.ui.phase = AiFeaturePhase::kSubmitting;
        g_state.ui.status_label = "正在提交";
        g_state.ui.response_text.clear();
        g_state.ui.activity_text = "WQN 正在中转到 OpenCode";
        g_state.ui.action_hint.clear();
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        g_state.ui.scroll_offset_lines = 0;
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
    }
    xSemaphoreGive(g_lock);
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

bool IsOpenCodeSessionActive()
{
    if (g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool active = AiFeaturePhaseIsBusy(g_state.ui.phase);
    xSemaphoreGive(g_lock);
    return active;
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
void ScrollOpenCodeResponse(int, int32_t, int32_t) {}
bool CopyOpenCodeSessionToUi(AgentSessionState*) { return false; }
bool IsOpenCodeSessionActive() { return false; }

}  // namespace wqn

#endif  // CONFIG_WQN_AGENT_ENABLE
