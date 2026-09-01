#include "ai_session.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <random>
#include <string>
#include <utility>

#include "ai_history.h"
#include "audio_capture.h"
#include "config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "runtime/sleep_coordinator.h"
#include "services/connectivity_service.h"
#include "stdpro_ws_transport.h"
#include "storage.h"
#include "wqn_api.h"

namespace {

constexpr char kTag[] = "wqn_ai";
constexpr int kMinAudioDurationMs = 1000;
constexpr size_t kMinAudioSamples =
    static_cast<size_t>(wqn::kAudioCaptureSampleRate) *
    static_cast<size_t>(kMinAudioDurationMs) / 1000U;
constexpr int kMaxAudioDurationMs = 20000;
constexpr int kMinAudioPeak = 80;
constexpr int kMinAudioRms = 8;
constexpr TickType_t kWifiReadyWait = pdMS_TO_TICKS(35000);

SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_submit_task = nullptr;
enum class AiWorkerCommand : uint8_t {
    kNone,
    kPrepareRecording,
    kSubmitSession,
};
AiWorkerCommand g_worker_command = AiWorkerCommand::kNone;
// True while the corresponding phase of the shared AI worker is parked. The
// worker's single static stack is reused for both preparation and submission.
bool g_submit_parked = true;
// [ai-worker-reserve] Statically-stored stack/TCB for the AI worker. Field data
// (wqn-device 2026-08-23): after the
// first TLS upload the internal largest free block settles at 6.4-6.9 KiB, so
// transient xTaskCreate calls deterministically fail on later turns. A single
// once-created worker removes both prepare and submit stacks from that failure
// surface without reserving a second permanent 6 KiB internal stack.
constexpr uint32_t kSubmitTaskStackBytes = 7168;
StaticTask_t g_submit_task_tcb = {};
StackType_t g_submit_task_stack[kSubmitTaskStackBytes / sizeof(StackType_t)] = {};
wqn::AiSessionState g_state;
std::string g_conversation_id;
bool g_changed = false;
bool g_loaded_today = false;
bool g_prepare_active = false;
bool g_prepare_parked = true;
bool g_recording_requested = false;
uint32_t g_prepare_generation = 0;
uint32_t g_prepare_command_generation = 0;
bool g_streaming_active = false;        // true while the AI worker is parsing SSE events
bool g_streaming_force_full_render = false; // when true the next UI tick does a full refresh
wqn::runtime::SleepLease g_ai_sleep_lease;
wqn::services::ConnectivityDemand g_ai_connectivity_demand;
std::string g_pending_tool_label;        // "🔧 create_todo…" or "✅ ..." for status bar
int64_t g_tool_clear_at_ms = 0;          // scheduled status-bar clear

bool g_turn_ws_capable = false;
std::string g_current_turn_req_id;
uint32_t g_current_turn_gen = 0;

void AudioCaptureTapHandler(const int16_t* samples, size_t count, void*)
{
    wqn::stdpro_ws::PushPcm(samples, count);
}

struct StdProTurnAssembly {
    uint64_t last_event_id = 0;
    bool user_committed = false;
    bool thinking_seen = false;
    bool thinking_done = false;
    wqn::ChatMessageId thinking_id = wqn::kInvalidChatMessageId;
    std::string thinking_text;
    wqn::ChatMessageId assistant_id = wqn::kInvalidChatMessageId;
    std::string assistant_text;
    bool text_started = false;
    bool assistant_terminal = false;
    // [tool-order] Text runs already committed as their own history entries by
    // a tool-boundary seal this turn. Drives the authoritative-text
    // reconciliation in ResolveSegmentTextLocked.
    int segments_sealed = 0;
};
StdProTurnAssembly g_turn;

void LogAiMemory(const char* stage)
{
    ESP_LOGI(
        kTag,
        "memory stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u dma_free=%u dma_largest=%u stack_hwm=%u",
        stage,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

std::string ThinkingLabel(const std::string& text)
{
    return text.empty() ? std::string() : std::string("💭 ") + text;
}

void ResetTurnAssemblyLocked()
{
    g_turn = StdProTurnAssembly{};
}

bool EnsureThinkingHistoryLocked(wqn::AiHistory& history, int64_t now_ms)
{
    if (!g_turn.user_committed || g_turn.thinking_text.empty()) {
        return false;
    }
    const std::string label = ThinkingLabel(g_turn.thinking_text);
    if (g_turn.thinking_id == wqn::kInvalidChatMessageId) {
        g_turn.thinking_id = history.AppendThinking(label, now_ms);
        return g_turn.thinking_id != wqn::kInvalidChatMessageId;
    }
    return history.ReplaceText(g_turn.thinking_id, wqn::ChatMessageKind::kThinking,
                               label, now_ms);
}

bool FinalizeThinkingLocked(wqn::AiHistory& history, const std::string& authoritative,
                            int64_t now_ms)
{
    if (!authoritative.empty()) {
        g_turn.thinking_text = authoritative;
    }
    return EnsureThinkingHistoryLocked(history, now_ms);
}

bool FinalizeAssistantLocked(wqn::AiHistory& history, const std::string& authoritative,
                             int64_t now_ms)
{
    if (!authoritative.empty()) {
        g_turn.assistant_text = authoritative;
    }
    if (!g_turn.user_committed || g_turn.assistant_text.empty()) {
        return false;
    }
    if (g_turn.assistant_id == wqn::kInvalidChatMessageId) {
        g_turn.assistant_id = history.AppendAssistant(g_turn.assistant_text, now_ms);
        return g_turn.assistant_id != wqn::kInvalidChatMessageId;
    }
    return history.ReplaceText(g_turn.assistant_id, wqn::ChatMessageKind::kAssistant,
                               g_turn.assistant_text, now_ms);
}

// [tool-order] Commit the open text run as its own assistant history entry so
// a following tool block lands BELOW the text that preceded it (agent-style
// interleaving), instead of the whole reply being appended after every tool
// block at turn end. Also re-arms segment state so post-tool deltas open a
// fresh entry instead of being dropped by the terminal guard or rewritten
// into an earlier position via ReplaceText. No-op when nothing is pending:
// a run already entered history through text.end is only closed out here.
void SealAssistantSegmentLocked(wqn::AiHistory& history, int64_t now_ms)
{
    if (!g_turn.assistant_terminal && g_turn.user_committed &&
        !g_turn.assistant_text.empty()) {
        FinalizeAssistantLocked(history, std::string(), now_ms);
        ++g_turn.segments_sealed;
    }
    g_turn.assistant_id = wqn::kInvalidChatMessageId;
    g_turn.assistant_text.clear();
    g_state.assistant_partial.clear();
    g_turn.assistant_terminal = false;
    g_turn.text_started = false;
}

// [tool-order] Authoritative-text precedence for text.end / turn.done / final.
// Turns without a seal keep the legacy order (full_text > text > deltas).
// After a tool-boundary seal the payload may be cumulative for the whole
// turn, so adopting it verbatim would duplicate already-sealed entries: when
// the open segment carries streamed deltas those win; only a run with no
// streamed content of its own adopts the payload.
std::string ResolveSegmentTextLocked(const std::string& full, const std::string& text)
{
    const std::string& streamed = g_turn.assistant_text;
    if (g_turn.segments_sealed == 0 || streamed.empty()) {
        return !full.empty() ? full : (!text.empty() ? text : streamed);
    }
    ESP_LOGD(kTag, "authoritative full_text ignored after segment seal");
    return streamed;
}

void MarkChanged()
{
    g_changed = true;
}

void ReleaseAiSleepLeaseIfIdleLocked()
{
    const bool state_active =
        g_state.status == wqn::AiSessionStatus::kPreparingCapture ||
        g_state.status == wqn::AiSessionStatus::kListening ||
        g_state.status == wqn::AiSessionStatus::kWaitingReply ||
        g_state.status == wqn::AiSessionStatus::kStreaming;
    if (!g_prepare_active && g_prepare_parked && g_submit_parked &&
        !g_streaming_active && !state_active) {
        g_ai_connectivity_demand.Reset();
        g_ai_sleep_lease.Reset();
    }
}

void FinishSubmitTaskLocked()
{
    g_submit_parked = true;
    if (g_worker_command == AiWorkerCommand::kSubmitSession) {
        g_worker_command = AiWorkerCommand::kNone;
    }
    ReleaseAiSleepLeaseIfIdleLocked();
}

void SetStateLocked(wqn::AiSessionStatus status, const std::string& pending, const std::string& user, const std::string& reply)
{
    g_state.status = status;
    if (!pending.empty()) {
        g_state.pending_text = pending;
    }
    if (!user.empty()) {
        g_state.user_text = user;
    }
    if (!reply.empty()) {
        g_state.assistant_text = reply;
    }
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
}

void SetErrorLocked(const std::string& message)
{
    g_state.status = wqn::AiSessionStatus::kError;
    g_state.pending_text.clear();
    if (g_state.user_text.empty()) {
        g_state.user_text = "未完成识别";
    }
    g_state.assistant_text = message;
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    g_state.toast_visible = false;
    MarkChanged();
    ReleaseAiSleepLeaseIfIdleLocked();
}

std::string RenderStreamingStatusLocked()
{
    // Used by the UI when ai.status == kStreaming (v2 SSE).
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const int elapsed_s = g_state.status_since_ms > 0
        ? static_cast<int>((now_ms - g_state.status_since_ms) / 1000)
        : 0;
    std::string label = "服务器处理中";
    if (elapsed_s > 0) {
        label += "·";
        label += std::to_string(elapsed_s);
        label += "s";
    }
    if (!g_state.user_partial.empty()) {
        label += " · 听写 ";
        label += std::to_string(g_state.user_partial.size());
        label += "字";
    }
    if (!g_state.assistant_partial.empty()) {
        label += " · 回复 ";
        label += std::to_string(g_state.assistant_partial.size());
        label += "字";
    }
    return label;
}

void SetCancelledBeforeRecordingLocked()
{
    g_state.status = wqn::AiSessionStatus::kIdle;
    g_state.pending_text.clear();
    g_state.user_text.clear();
    g_state.assistant_text = "WiFi 未就绪，已取消录音";
    g_state.status_detail.clear();
    g_state.function_call_summaries.clear();
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    g_state.toast_visible = false;
    MarkChanged();
    ReleaseAiSleepLeaseIfIdleLocked();
}

// ============================================================================
// v2 SSE consumer
// ============================================================================
//
// The shared AI worker drives the SSE stream on its own task stack. Each callback runs on
// that stack, so we take g_lock only briefly and we never call esp_http_client
// or cJSON inside the lock.
//
// Incremental text/tool updates must not rewrite the entire assistant_text;
// they go to assistant_partial, and the UI tick renders the partial at the
// configured throttle.
void OnSseEvent(const wqn::WqnAiSseEvent& ev)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro);

    if (ev.event_id != 0 && ev.event_id <= g_turn.last_event_id) {
        xSemaphoreGive(g_lock);
        return;
    }
    if (ev.event_id != 0) {
        g_turn.last_event_id = ev.event_id;
    }

    switch (ev.kind) {
        case wqn::WqnAiSseEvent::Kind::kUnknown:
            ESP_LOGD(kTag, "ignore unknown SSE event id=%llu",
                     static_cast<unsigned long long>(ev.event_id));
            break;
        case wqn::WqnAiSseEvent::Kind::kReady:
            g_state.status = wqn::AiSessionStatus::kStreaming;
            g_state.pending_text = "已连接 · 等待模型…";
            g_state.toast_label = "● 服务器处理中";
            g_state.toast_visible = true;
            g_state.toast_since_ms = now_ms;
            if (!ev.conversation_id.empty()) g_conversation_id = ev.conversation_id;
            g_state.status_since_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kStage:
            if (!ev.stage.empty()) g_state.pending_text = ev.stage;
            g_state.status_since_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kAsrDelta:
            g_state.user_partial += ev.delta;
            g_state.last_render_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kAsrComplete:
            g_state.user_text = ev.text;
            g_state.user_partial.clear();
            g_streaming_force_full_render = true;
            g_state.status_since_ms = now_ms;
            if (!g_turn.user_committed && !ev.text.empty()) {
                g_turn.user_committed =
                    history.AppendUser(ev.text, now_ms) != wqn::kInvalidChatMessageId;
            }
            EnsureThinkingHistoryLocked(history, now_ms);
            if (g_turn.assistant_terminal) {
                FinalizeThinkingLocked(history, std::string(), now_ms);
                FinalizeAssistantLocked(history, g_turn.assistant_text, now_ms);
            }
            break;
        case wqn::WqnAiSseEvent::Kind::kAsrFailed:
            g_state.user_partial.clear();
            g_state.user_text = ev.error_message.empty() ? "识别失败" : ev.error_message;
            g_state.status_since_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kThinkingStart:
            g_turn.thinking_seen = true;
            break;
        case wqn::WqnAiSseEvent::Kind::kThinkingDelta:
            if (!g_turn.thinking_done && !ev.delta.empty()) {
                g_turn.thinking_seen = true;
                g_turn.thinking_text += ev.delta;
                if (g_turn.thinking_id == wqn::kInvalidChatMessageId) {
                    EnsureThinkingHistoryLocked(history, now_ms);  // first visible prefix only
                    g_streaming_force_full_render = true;
                }
            }
            break;
        case wqn::WqnAiSseEvent::Kind::kThinkingDone: {
            const std::string authoritative = !ev.full_text.empty() ? ev.full_text
                : (!ev.text.empty() ? ev.text : g_turn.thinking_text);
            g_turn.thinking_seen = !authoritative.empty();
            g_turn.thinking_done = true;
            FinalizeThinkingLocked(history, authoritative, now_ms);
            g_streaming_force_full_render = true;
            break;
        }
        case wqn::WqnAiSseEvent::Kind::kTextStart:
            if (!g_turn.text_started) {
                g_turn.text_started = true;
                g_state.assistant_partial.clear();
                g_turn.assistant_text.clear();
            } else {
                // [tool-order] Server re-opened the text channel without an
                // explicit end: seal the open run so the next one lands in
                // its own entry below.
                SealAssistantSegmentLocked(history, now_ms);
                g_turn.text_started = true;
            }
            g_state.last_render_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kTextDelta:
            if (ev.delta.empty()) break;
            if (g_turn.assistant_terminal || !g_turn.text_started) {
                // [tool-order] Deltas resumed after a terminal/boundary:
                // open a fresh segment so they land below the sealed run
                // instead of being dropped or merged into an earlier entry.
                SealAssistantSegmentLocked(history, now_ms);
                g_turn.text_started = true;
            }
            g_state.assistant_partial += ev.delta;
            g_turn.assistant_text += ev.delta;
            g_state.last_render_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kTextEnd: {
            const std::string final_text = ResolveSegmentTextLocked(ev.full_text, ev.text);
            g_state.assistant_text = final_text;
            g_state.assistant_partial.clear();
            g_turn.assistant_terminal = true;
            FinalizeThinkingLocked(history, std::string(), now_ms);
            FinalizeAssistantLocked(history, final_text, now_ms);
            g_streaming_force_full_render = true;
            g_state.status_since_ms = now_ms;
            break;
        }
        case wqn::WqnAiSseEvent::Kind::kToolStart: {
            // [tool-order] Seal before appending so pre-tool text stays above
            // this block.
            SealAssistantSegmentLocked(history, now_ms);
            std::string label = "🔧 " + (ev.tool_name.empty() ? std::string("tool") : ev.tool_name) + "…";
            g_pending_tool_label = label;
            g_tool_clear_at_ms = 0;
            g_state.function_call_summaries.push_back(ev.tool_name.empty() ? "tool" : ev.tool_name);
            g_state.status_detail = label;
            g_state.status_since_ms = now_ms;
            history.AppendToolStart(ev.tool_name, ev.tool_display, now_ms);
            break;
        }
        case wqn::WqnAiSseEvent::Kind::kToolResult:
        case wqn::WqnAiSseEvent::Kind::kToolError: {
            // [tool-order] Pop the placeholder before sealing: sealing may
            // append a run streamed while the tool executed, and that text
            // must sit between the placeholder and the result block.
            history.PopLastIf(wqn::ChatMessageKind::kToolStart);
            SealAssistantSegmentLocked(history, now_ms);
            std::string label = ev.tool_ok ? "✅ " : "❌ ";
            label += ev.tool_display.empty() ? ev.tool_name : ev.tool_display;
            g_pending_tool_label = label;
            g_tool_clear_at_ms = now_ms + 2000;
            if (!g_state.function_call_summaries.empty() &&
                g_state.function_call_summaries.back() == ev.tool_name) {
                g_state.function_call_summaries.back() =
                    ev.tool_display.empty() ? ev.tool_name : ev.tool_display;
            }
            g_state.status_detail = label;
            g_state.status_since_ms = now_ms;
            history.AppendToolResult(ev.tool_name, ev.tool_display,
                                     ev.tool_display, ev.tool_ok,
                                     ev.tool_elapsed_ms, now_ms);
            break;
        }
        case wqn::WqnAiSseEvent::Kind::kState:
            g_state.status_detail = ev.error_message.empty() ? ev.stage : ev.error_message;
            g_state.status_since_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kTurnDone:
            g_turn.assistant_terminal = true;
            FinalizeThinkingLocked(history, std::string(), now_ms);
            FinalizeAssistantLocked(
                history, ResolveSegmentTextLocked(std::string(), std::string()), now_ms);
            g_streaming_force_full_render = true;
            break;
        case wqn::WqnAiSseEvent::Kind::kError: {
            const std::string msg = ev.error_message.empty() ? ev.error_code : ev.error_message;
            g_state.status = wqn::AiSessionStatus::kError;
            g_state.pending_text.clear();
            if (g_state.assistant_text.empty()) g_state.assistant_text = msg;
            g_state.toast_visible = false;
            g_streaming_active = false;
            g_state.status_since_ms = now_ms;
            break;
        }
        case wqn::WqnAiSseEvent::Kind::kFinal: {
            const std::string final_text = ResolveSegmentTextLocked(ev.full_text, ev.text);
            if (final_text.empty()) {
                // Nothing streamed or authoritative for the open run; keep the
                // last known reply text instead of blanking the session.
            } else {
                g_turn.assistant_terminal = true;
                FinalizeThinkingLocked(history, std::string(), now_ms);
                FinalizeAssistantLocked(history, final_text, now_ms);
                g_state.assistant_text = final_text;
            }
            g_state.assistant_partial.clear();
            g_state.user_partial.clear();
            g_state.pending_text.clear();
            g_state.status = wqn::AiSessionStatus::kReplyReady;
            g_state.toast_visible = false;
            if (!ev.conversation_id.empty()) g_conversation_id = ev.conversation_id;
            g_state.conversation_id = g_conversation_id;
            g_state.page = 0;
            g_state.scroll_offset_lines = 0;
            g_streaming_force_full_render = true;
            g_streaming_active = false;
            g_state.status_since_ms = now_ms;
            break;
        }
    }
    // [ui-throttle] Streaming deltas land every ~15 ms; the EPD cannot render
    // that fast and every changed-flag round-trip costs a full state copy on
    // the UI task. Coalesce delta-only marks to 50 ms; terminal / stage /
    // tool events always mark immediately so completion never lags. Runs
    // under g_lock with one producer per turn, so the watermark is stable.
    static int64_t s_last_delta_mark_ms = -1000;
    const bool is_streaming_delta =
        ev.kind == wqn::WqnAiSseEvent::Kind::kTextDelta ||
        ev.kind == wqn::WqnAiSseEvent::Kind::kAsrDelta ||
        ev.kind == wqn::WqnAiSseEvent::Kind::kThinkingDelta;
    if (!is_streaming_delta || now_ms - s_last_delta_mark_ms >= 50) {
        if (is_streaming_delta) {
            s_last_delta_mark_ms = now_ms;
        }
        MarkChanged();
    }
    xSemaphoreGive(g_lock);
}

// Trampoline that lets us pass `&OnSseEvent` to the C-style callback.
void TrampolineSseEvent(const wqn::WqnAiSseEvent& ev, void* /*user*/)
{
    OnSseEvent(ev);
}

// Request-id used by the SSE idempotency header. Same shape as the v1
// `request_id` (16 hex chars) so server-side logs read consistently.
std::string GenerateRequestId()
{
    static std::mt19937 rng{static_cast<unsigned>(esp_timer_get_time())};
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%08x%08x", rng(), rng());
    return std::string(buf, 16);
}

bool IsCurrentPrepareTaskLocked(uint32_t generation)
{
    return g_prepare_active && generation == g_prepare_generation;
}

void FinishPrepareTaskLocked(uint32_t generation)
{
    if (generation == g_prepare_generation) {
        g_prepare_active = false;
    }
    g_prepare_parked = true;
    if (g_worker_command == AiWorkerCommand::kPrepareRecording) {
        g_worker_command = AiWorkerCommand::kNone;
    }
    ReleaseAiSleepLeaseIfIdleLocked();
}

bool HasEffectiveSpeech(const wqn::AudioCaptureChunk& audio)
{
    return audio.duration_ms >= kMinAudioDurationMs && audio.sample_count >= kMinAudioSamples &&
           audio.peak >= kMinAudioPeak &&
           audio.rms >= kMinAudioRms;
}

std::string MessageForAiRequestFailure(const wqn::WqnAiChatResponse& response, esp_err_t result)
{
    if (response.error_code == "no_speech") {
        return "未识别到语音";
    }
    if (response.error_code == "asr_failed") {
        return "识别失败";
    }
    if (response.error_code == "asr_timeout") {
        return "识别超时，请稍后重试";
    }
    if (response.error_code == "model_failed") {
        return "模型回复失败";
    }
    if (response.error_code == "chat_timeout") {
        return "AI 回复超时，请稍后重试";
    }
    if (response.error_code == "provider_unavailable") {
        return "AI 服务暂时不可用，请稍后重试";
    }
    if (response.error_code == "unauthorized") {
        return "设备授权已失效，请重新配对";
    }
    if (response.error_code == "rate_limited") {
        return "请求太频繁，请稍后再试";
    }
    if (response.error_code == "disabled") {
        return "AI 服务未启用";
    }
    if (!response.error_message.empty()) {
        return std::string("AI 请求失败: ") + response.error_message;
    }
    return std::string("AI 请求失败: ") + esp_err_to_name(result);
}

std::string BuildAiActionSummary(const std::vector<wqn::WqnAiAction>& actions)
{
    std::string summary;
    for (const auto& action : actions) {
        if (action.type == "todo_created") {
            const std::string title = action.title.empty() ? "Todo" : action.title;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已添加 Todo：" + title;
            continue;
        }
        if (action.type == "todo_status_updated" &&
            (action.status == "pending" || action.status == "cancelled")) {
            const std::string title = action.title.empty() ? "Todo" : action.title;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += action.status == "pending" ? "已恢复 Todo：" : "已取消 Todo：";
            summary += title;
            continue;
        }
        if (action.type == "todo_status_updated" && action.status == "completed") {
            const std::string title = action.title.empty() ? "Todo" : action.title;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已完成 Todo：" + title;
            continue;
        }
        if (action.type == "word_review_recorded") {
            const std::string word = action.word.empty() ? action.title : action.word;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已记录单词：";
            summary += word.empty() ? "Word" : word;
            continue;
        }
        if (action.type == "word_added_to_mistakes") {
            const std::string word = action.word.empty() ? action.title : action.word;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已加入错词本：";
            summary += word.empty() ? "Word" : word;
            continue;
        }
        if (action.type == "word_deck_created") {
            const std::string title = action.title.empty() ? "Word Deck" : action.title;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已创建词库：" + title;
            continue;
        }
        if (action.type == "word_added_to_deck") {
            const std::string word = action.word.empty() ? action.title : action.word;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已加入词库：";
            summary += word.empty() ? "Word" : word;
            continue;
        }
        if (action.type != "notebook_note_created") {
            continue;
        }

        std::string line = "已记录";
        if (!action.title.empty()) {
            line += "：";
            line += action.title;
        }
        if (!summary.empty()) {
            summary += "\n";
        }
        summary += line;
    }
    return summary;
}

std::string BuildStatusDetail(const wqn::WqnAiChatResponse& response)
{
    std::string detail;
    if (!response.asr.text.empty()) {
        detail = "识别完成";
        if (response.asr.elapsed_ms > 0) {
            detail += " ";
            detail += std::to_string(response.asr.elapsed_ms);
            detail += "ms";
        }
    }
    if (response.latency_ms > 0) {
        if (!detail.empty()) {
            detail += " · ";
        }
        detail += "总耗时 ";
        detail += std::to_string(response.latency_ms);
        detail += "ms";
    }
    if (!response.status_trace.empty()) {
        const auto& last = response.status_trace.back();
        if (!last.stage.empty()) {
            if (!detail.empty()) {
                detail += " · ";
            }
            detail += last.stage;
            if (!last.status.empty()) {
                detail += ":";
                detail += last.status;
            }
        }
    }
    return detail;
}

void SaveTodaySessionLocked(const wqn::WqnAiChatResponse& /*response*/)
{
    // v2: PSRAM-only persistence per the plan. The session lives in wqn::AiHistory
    // and is intentionally lost on reboot. We do NOT touch NVS from here; the
    // historical SaveAiSessionForDay() call was the source of
    // ESP_ERR_NVS_NOT_ENOUGH_SPACE on full NVS, so we skip it.
    ESP_LOGD(kTag, "SaveTodaySessionLocked: no-op, history lives in PSRAM");
}

void LoadTodaySessionLocked()
{
    if (g_loaded_today) {
        return;
    }
    g_loaded_today = true;

    // v2: PSRAM-only. The session lives entirely in wqn::AiHistory and is
    // intentionally lost on reboot. We do NOT call LoadAiSessionForDay()
    // (which would also fight ESP_ERR_NVS_NOT_ENOUGH_SPACE).
    //
    // On boot the history is empty. Recent replies are rebuilt from the
    // resume-conversation flow on the cloud side; we keep
    // g_conversation_id so the next session can rejoin the chat.
    (void)wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro);
    ESP_LOGI(kTag, "LoadTodaySessionLocked: PSRAM history ready, size=%zu",
             wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro).size());
}

// One AI submission: stop capture, validate the clip, hand off to the WS
// transport (FINAL + REMOTE_HANDOFF) or fall back to HTTP SSE upload.
// Runs on the persistent shared AI worker; returns when terminal state is
// published and the worker parks again.
void SubmitSession()
{
    LogAiMemory("submit-start");
    wqn::AudioCaptureChunk audio;
    esp_err_t result = wqn::StopAudioCapture(&audio);

    const std::string turn_req_id = g_current_turn_req_id;
    const bool is_ws_turn = g_turn_ws_capable;
    const uint32_t turn_gen = g_current_turn_gen;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK) {
        if (is_ws_turn && !turn_req_id.empty()) {
            wqn::stdpro_ws::AbortTurn(turn_req_id, turn_gen);
            g_turn_ws_capable = false;
        }
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("录音停止失败");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        return;
    }

    ESP_LOGI(kTag,
             "record_release: audio captured duration_ms=%d mono_samples=%u peak=%d rms=%d",
             audio.duration_ms,
             static_cast<unsigned>(audio.sample_count),
             static_cast<int>(audio.peak),
             audio.rms);

    if (audio.duration_ms < kMinAudioDurationMs || audio.sample_count < kMinAudioSamples) {
        if (is_ws_turn && !turn_req_id.empty()) {
            wqn::stdpro_ws::AbortTurn(turn_req_id, turn_gen);
            g_turn_ws_capable = false;
        }
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("录音太短");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        return;
    }

    if (!HasEffectiveSpeech(audio)) {
        if (is_ws_turn && !turn_req_id.empty()) {
            wqn::stdpro_ws::AbortTurn(turn_req_id, turn_gen);
            g_turn_ws_capable = false;
        }
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("未检测到有效语音");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        return;
    }

    if (audio.duration_ms > kMaxAudioDurationMs) {
        audio.duration_ms = kMaxAudioDurationMs;
    }
    SetStateLocked(wqn::AiSessionStatus::kWaitingReply, "正在识别...", "", "");
    g_state.toast_label = "● 识别中…";
    g_state.toast_visible = true;
    g_state.toast_since_ms = esp_timer_get_time() / 1000;
    g_state.toast_recording_ms = 0;
    MarkChanged();
    const std::string tier_str = g_state.tier == wqn::AiTier::kPro ? "pro" : "std";
    const wqn::ThinkingLevel thinking_level = g_state.thinking_level;
    const std::string conversation_id = g_conversation_id;
    g_streaming_active = true;
    g_streaming_force_full_render = false;
    ResetTurnAssemblyLocked();
    g_pending_tool_label.clear();
    g_tool_clear_at_ms = 0;
    g_state.assistant_partial.clear();
    g_state.user_partial.clear();
    g_state.function_call_summaries.clear();
    g_state.status_detail.clear();
    xSemaphoreGive(g_lock);

    std::string token;
    result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK || !wqn::IsValidAccessToken(token)) {
        if (is_ws_turn && !turn_req_id.empty()) {
            wqn::stdpro_ws::AbortTurn(turn_req_id, turn_gen);
            g_turn_ws_capable = false;
        }
        wqn::ReleaseAudioCapturePower();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_streaming_active = false;
        SetErrorLocked("设备未配对");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        return;
    }

    esp_err_t submit_result = ESP_OK;
    wqn::WqnAiChatResponse response;
    bool used_streaming = true;
    bool handoff_to_ws_succeeded = false;

    if (is_ws_turn) {
        const auto handoff_res = wqn::stdpro_ws::SendFinalAndWait(
            turn_req_id, audio.duration_ms, 2000);

        if (handoff_res == wqn::stdpro_ws::FinalHandoffResult::kFinalSent) {
            ESP_LOGI(kTag, "final_sent: entering REMOTE_HANDOFF for req_id=%s",
                     turn_req_id.c_str());
            handoff_to_ws_succeeded = true;
            // REMOTE_HANDOFF: automatic HTTP fallback is strictly FORBIDDEN.
            // Wait for WS SSE stream to finish
            const auto wait_res =
                wqn::stdpro_ws::WaitForTurnRelease(turn_gen, turn_req_id, WQN_AI_SSE_TIMEOUT_MS);
            if (wait_res.wait_status == wqn::stdpro_ws::TurnWaitStatus::kTimedOut) {
                ESP_LOGW(kTag, "WS SSE turn timed out in REMOTE_HANDOFF");
                submit_result = ESP_ERR_TIMEOUT;
            }
        } else if (handoff_res == wqn::stdpro_ws::FinalHandoffResult::kAmbiguous) {
            ESP_LOGW(kTag, "FINAL result ambiguous, skipping HTTP resubmit to prevent duplicate turn");
            handoff_to_ws_succeeded = true;
            wqn::stdpro_ws::WaitForTurnRelease(turn_gen, turn_req_id, 5000);
        } else {
            ESP_LOGW(kTag, "FINAL failed to send over WS, falling back to HTTP");
            handoff_to_ws_succeeded = false;
        }
    }

    if (!handoff_to_ws_succeeded) {
        // Fallback HTTP POST path
        wqn::WqnAiStreamRequest req;
        req.token = token;
        req.pcm_data = audio.samples;
        req.pcm_sample_count = audio.sample_count;
        req.duration_ms = audio.duration_ms;
        req.tier = tier_str;
        req.conversation_id = conversation_id;
        {
            const char* effort = "medium";
            switch (thinking_level) {
                case wqn::ThinkingLevel::kOff: effort = "low"; break;
                case wqn::ThinkingLevel::kLow: effort = "low"; break;
                case wqn::ThinkingLevel::kMed: effort = "medium"; break;
                case wqn::ThinkingLevel::kHigh: effort = "high"; break;
                default: break;
            }
            req.reasoning_effort = effort;
            req.enable_thinking = (thinking_level != wqn::ThinkingLevel::kOff);
        }
        req.request_id = turn_req_id.empty() ? GenerateRequestId() : turn_req_id;
        req.callback = &TrampolineSseEvent;
        req.user_ctx = nullptr;
        LogAiMemory("before-sse-upload");
        submit_result = wqn::UploadAiAudioChatStream(req, &response);
        LogAiMemory("after-sse-upload");
    }

    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_streaming_active = false;

    if (used_streaming) {
        // The SSE consumer has already pushed everything into g_state.
        if (submit_result != ESP_OK || g_state.status == wqn::AiSessionStatus::kError) {
            if (g_state.status != wqn::AiSessionStatus::kError) {
                const std::string msg = response.error_message.empty()
                    ? response.error_code
                    : response.error_message;
                SetErrorLocked(msg.empty() ? "AI 请求失败" : ("AI 请求失败: " + msg));
            }
        } else if (g_state.status != wqn::AiSessionStatus::kReplyReady) {
            // Stream finished cleanly but never delivered a `final` event: surface a
            // soft error so the user sees something other than a frozen spinner.
            SetErrorLocked(response.error_message.empty() ? "服务器未返回完成事件" : response.error_message);
        } else {
            // Persist today's session for cache.
            if (!response.conversation_id.empty()) {
                g_conversation_id = response.conversation_id;
                g_state.conversation_id = g_conversation_id;
            }
            // Build a synthetic legacy response so the persistence path is shared
            // with the v1 flow (SaveTodaySessionLocked expects that shape).
            if (!response.transcript.empty()) g_state.user_text = response.transcript;
            if (!response.reply_text.empty()) g_state.assistant_text = response.reply_text;
            response.transcript = g_state.user_text;
            response.reply_text = g_state.assistant_text;
            response.latency_ms = response.latency_ms;
            response.conversation_id = g_conversation_id;
            SaveTodaySessionLocked(response);
        }
    } else {
        // v1 one-shot: replicate the legacy behaviour verbatim.
        if (submit_result == ESP_OK) {
            g_conversation_id = response.conversation_id;
            g_state.status = wqn::AiSessionStatus::kReplyReady;
            g_state.pending_text.clear();
            g_state.toast_visible = false;
            if (!response.transcript.empty()) {
                g_state.user_text = response.transcript;
            } else if (g_state.user_text.empty()) {
                g_state.user_text = "未返回转写";
            }
            if (!response.reply_text.empty()) {
                g_state.assistant_text = response.reply_text;
            } else {
                g_state.assistant_text = "模型未返回文本回复";
            }
            g_state.status_detail = BuildStatusDetail(response);
            g_state.function_call_summaries.clear();
            for (const auto& call : response.function_calls) {
                if (!call.display.empty()) {
                    g_state.function_call_summaries.push_back(call.display);
                }
            }
            const std::string action_summary = BuildAiActionSummary(response.actions);
            if (!action_summary.empty()) {
                if (g_state.function_call_summaries.empty()) {
                    g_state.function_call_summaries.push_back(action_summary);
                }
                if (!g_state.assistant_text.empty()) {
                    g_state.assistant_text += "\n";
                }
                g_state.assistant_text += action_summary;
            }
            g_state.page = 0;
            g_state.conversation_id = g_conversation_id;
            g_state.status_since_ms = esp_timer_get_time() / 1000;

            // Append User Question to History
            if (!g_state.user_text.empty()) {
                wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro)
                    .AppendUser(g_state.user_text, g_state.status_since_ms);
            }

            // Append Tool/Action summaries if present
            for (const auto& summary : g_state.function_call_summaries) {
                wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro)
                    .AppendToolResult("action", summary, "done", true, 0,
                                      g_state.status_since_ms);
            }

            // Append Assistant Reply to History
            if (!g_state.assistant_text.empty()) {
                wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro)
                    .AppendAssistant(g_state.assistant_text, g_state.status_since_ms);
            }

            SaveTodaySessionLocked(response);
            MarkChanged();
            ESP_LOGI(kTag,
                     "AI response ready: latency_ms=%d transcript_bytes=%u reply_bytes=%u actions=%u",
                     response.latency_ms,
                     static_cast<unsigned>(response.transcript.size()),
                     static_cast<unsigned>(response.reply_text.size()),
                     static_cast<unsigned>(response.actions.size()));
        } else {
            SetErrorLocked(MessageForAiRequestFailure(response, submit_result));
        }
    }

    // Peak-stack evidence for whichever submission path ran (WS handoff wait
    // or HTTP/TLS fallback). HWM is monotonic for this task instance.
    LogAiMemory("submit-end");
    FinishSubmitTaskLocked();
    xSemaphoreGive(g_lock);
}

void PrepareRecordingSession(uint32_t generation)
{
    std::string token;
    enum class PrepareFailure {
        kNone,
        kNoToken,
        kInvalidToken,
        kWifi,
    };
    PrepareFailure failure = PrepareFailure::kNone;
    wqn::services::ConnectivityWaitResult wifi_result =
        wqn::services::ConnectivityWaitResult::kCancelled;

    esp_err_t result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK || token.empty()) {
        failure = PrepareFailure::kNoToken;
        result = ESP_ERR_INVALID_STATE;
    } else if (!wqn::IsValidAccessToken(token)) {
        failure = PrepareFailure::kInvalidToken;
        result = ESP_ERR_INVALID_STATE;
    } else {
        wqn::services::ConnectivityDemand connectivity_demand =
            wqn::services::AcquireConnectivityDemand(
                wqn::services::ConnectivityDemandReason::kAiInteractive,
                "ai-session",
                __FILE__,
                __LINE__);
        wqn::services::ConnectivityDemandTicket ticket;
        xSemaphoreTake(g_lock, portMAX_DELAY);
        if (connectivity_demand && IsCurrentPrepareTaskLocked(generation) &&
            g_recording_requested) {
            g_ai_connectivity_demand = std::move(connectivity_demand);
            ticket = g_ai_connectivity_demand.ticket();
        }
        xSemaphoreGive(g_lock);
        wifi_result = wqn::services::WaitForConnectivity(ticket, kWifiReadyWait);
        result = wqn::services::ConnectivityWaitResultToEspErr(wifi_result);
        if (result != ESP_OK) {
            failure = PrepareFailure::kWifi;
        }
    }

    bool should_start_capture = false;
    std::string req_id;
    std::string tier_str;
    std::string conv_id;
    bool enable_thinking = false;
    const char* effort = "medium";

    if (result == ESP_OK) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        should_start_capture = g_recording_requested && IsCurrentPrepareTaskLocked(generation) &&
                               g_state.status == wqn::AiSessionStatus::kPreparingCapture && g_submit_parked;
        if (should_start_capture) {
            g_state.pending_text = "正在启动录音...";
            g_state.status_since_ms = esp_timer_get_time() / 1000;
            // [capture-first] Identity commits here; the WS capability flag is
            // published only after StartTurn so no other stage can act on a
            // turn that does not exist yet. I2S DMA must be placed before the
            // TLS handshake claims its share of the pool.
            req_id = GenerateRequestId();
            g_current_turn_req_id = req_id;
            tier_str = (g_state.tier == wqn::AiTier::kPro) ? "pro" : "std";
            conv_id = g_conversation_id;
            enable_thinking = (g_state.thinking_level != wqn::ThinkingLevel::kOff);
            switch (g_state.thinking_level) {
                case wqn::ThinkingLevel::kOff: effort = "low"; break;
                case wqn::ThinkingLevel::kLow: effort = "low"; break;
                case wqn::ThinkingLevel::kMed: effort = "medium"; break;
                case wqn::ThinkingLevel::kHigh: effort = "high"; break;
                default: break;
            }
            MarkChanged();
        }
        xSemaphoreGive(g_lock);
    }

    if (result != ESP_OK || !should_start_capture) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        const bool current = IsCurrentPrepareTaskLocked(generation);
        if (current && result != ESP_OK && g_recording_requested &&
            g_state.status == wqn::AiSessionStatus::kPreparingCapture) {
            if (failure == PrepareFailure::kInvalidToken) {
                SetErrorLocked("设备授权已失效，请重新配对");
                ESP_LOGW(kTag, "AI recording blocked: invalid token");
            } else if (failure == PrepareFailure::kNoToken) {
                SetErrorLocked("设备未配对，请先在 Web 端创建配对");
                ESP_LOGW(kTag, "AI recording blocked: no valid token");
            } else {
                switch (wifi_result) {
                    case wqn::services::ConnectivityWaitResult::kNeedsProvisioning:
                        SetErrorLocked("未配置 WiFi，请先在设置中配网");
                        break;
                    case wqn::services::ConnectivityWaitResult::kAuthFailed:
                        SetErrorLocked("WiFi 密码错误，请重新配网");
                        break;
                    case wqn::services::ConnectivityWaitResult::kTimedOut:
                        SetErrorLocked("WiFi 连接超时，请稍后重试");
                        break;
                    case wqn::services::ConnectivityWaitResult::kCancelled:
                        SetCancelledBeforeRecordingLocked();
                        break;
                    default:
                        SetErrorLocked("WiFi 暂时不可用，请稍后重试");
                        break;
                }
                ESP_LOGW(
                    kTag,
                    "AI recording blocked: WiFi result=%s error=%s",
                    wqn::services::ConnectivityWaitResultName(wifi_result),
                    esp_err_to_name(result));
            }
        } else if (current && !g_recording_requested && g_state.status == wqn::AiSessionStatus::kPreparingCapture) {
            SetCancelledBeforeRecordingLocked();
        }
        if (current) {
            g_recording_requested = false;
            g_turn_ws_capable = false;
        }
        FinishPrepareTaskLocked(generation);
        xSemaphoreGive(g_lock);
        return;
    }

    // [capture-first] WiFi association is already complete, but bring up I2S
    // before the memory-heavy WS/TLS handshake. The RX channel needs ~3.5 KiB
    // of contiguous DMA memory while TLS transiently holds ~17 KiB
    // (device-measured: post-connect dma_largest pinned at 736 B). PCM captured
    // before voice.turn.start commits is dropped by PushPcm's turn-state gate.
    result = wqn::StartAudioCapture();

    bool ws_ready = false;
    if (result == ESP_OK) {
        // Skip the expensive handshake entirely when the user already
        // cancelled during codec bring-up.
        bool proceed_network = false;
        xSemaphoreTake(g_lock, portMAX_DELAY);
        proceed_network =
            g_recording_requested && IsCurrentPrepareTaskLocked(generation) &&
            g_state.status == wqn::AiSessionStatus::kPreparingCapture && g_submit_parked;
        xSemaphoreGive(g_lock);

        if (proceed_network) {
            ws_ready = (wqn::stdpro_ws::EnsureConnected(token, 2500) == ESP_OK);
            ESP_LOGI(kTag,
                     "[dma-attrib] post-connect ws=%d dma_free=%u dma_largest=%u",
                     ws_ready ? 1 : 0,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
            if (ws_ready) {
                ESP_LOGI(kTag, "STD/PRO WebSocket transport ready for this turn");
            } else {
                ESP_LOGW(kTag, "STD/PRO WebSocket transport unavailable; using HTTP fallback");
            }
        }
    }

    if (ws_ready) {
        wqn::stdpro_ws::SetSseCallback(&TrampolineSseEvent, nullptr);
        esp_err_t start_turn_err = wqn::stdpro_ws::StartTurn(
            req_id, tier_str, conv_id, enable_thinking, effort, &g_current_turn_gen);
        if (start_turn_err == ESP_OK) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_turn_ws_capable = true;
            xSemaphoreGive(g_lock);
        } else {
            ESP_LOGW(kTag, "StartTurn failed (%s); disabling WS for this turn",
                     esp_err_to_name(start_turn_err));
            if (start_turn_err == ESP_ERR_TIMEOUT) {
                // [turn-lifecycle] The owner may still commit the turn after
                // the caller timed out; an identity-bound abort no-ops when
                // nothing was committed and tears down a late commit.
                wqn::stdpro_ws::AbortTurn(req_id);
            }
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_turn_ws_capable = false;
            xSemaphoreGive(g_lock);
        }
    }

    bool stop_started_capture = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool still_requested = g_recording_requested && IsCurrentPrepareTaskLocked(generation) &&
                                 g_state.status == wqn::AiSessionStatus::kPreparingCapture && g_submit_parked;
    if (result == ESP_OK && still_requested) {
        g_recording_requested = false;
        g_state.status = wqn::AiSessionStatus::kListening;
        g_state.user_text.clear();
        g_state.assistant_text.clear();
        g_state.pending_text = "正在录音...";
        g_state.status_detail.clear();
        g_state.function_call_summaries.clear();
        g_state.conversation_id = g_conversation_id;
        g_state.page = 0;
        g_state.scroll_offset_lines = 0;
        g_state.toast_label = "● 录音中 00:00";
        g_state.toast_visible = true;
        g_state.toast_since_ms = esp_timer_get_time() / 1000;
        g_state.toast_recording_ms = 0;
        g_state.status_since_ms = g_state.toast_since_ms;
        MarkChanged();
        FinishPrepareTaskLocked(generation);
        xSemaphoreGive(g_lock);
        ESP_LOGI(kTag, "record_start: generation=%lu req_id=%s ws=%d; toast=录音中",
                 static_cast<unsigned long>(generation), req_id.c_str(), ws_ready);
        return;
    }

    const bool current = IsCurrentPrepareTaskLocked(generation);
    if (result != ESP_OK && still_requested) {
        SetErrorLocked(std::string("录音启动失败: ") + esp_err_to_name(result));
    } else if (current && !still_requested && g_state.status == wqn::AiSessionStatus::kPreparingCapture) {
        SetCancelledBeforeRecordingLocked();
    }
    stop_started_capture = result == ESP_OK;
    // [turn-lifecycle] Abort on the committed identity even when a concurrent
    // stop bumped the generation: StartTurn may already have committed this
    // task's turn on the transport, and only this req_id can tear it down.
    if (g_turn_ws_capable && !req_id.empty()) {
        wqn::stdpro_ws::AbortTurn(req_id, g_current_turn_gen);
        g_turn_ws_capable = false;
    }
    if (current) {
        g_recording_requested = false;
    }
    FinishPrepareTaskLocked(generation);
    xSemaphoreGive(g_lock);

    if (stop_started_capture) {
        wqn::AudioCaptureChunk discarded;
        wqn::StopAudioCapture(&discarded);
        wqn::ReleaseAudioCapturePower();
    }
}

void AiSessionWorkerTask(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(g_lock, portMAX_DELAY);
        const AiWorkerCommand command = g_worker_command;
        const uint32_t generation = g_prepare_command_generation;
        xSemaphoreGive(g_lock);
        switch (command) {
            case AiWorkerCommand::kPrepareRecording:
                PrepareRecordingSession(generation);
                break;
            case AiWorkerCommand::kSubmitSession:
                SubmitSession();
                break;
            case AiWorkerCommand::kNone:
                break;
        }
    }
}

}  // namespace

namespace wqn {

esp_err_t InitAiSession()
{
    SetAiAudioCaptureTapEnabled(true);
    const esp_err_t capture_buffer_result = InitAudioCaptureBuffer();
    if (capture_buffer_result != ESP_OK) {
        // Keep the rest of the device bootable. StartAudioCapture retries and
        // reports a precise memory error if the early reservation ever fails.
        ESP_LOGW(kTag, "early AI capture buffer reservation failed: %s",
                 esp_err_to_name(capture_buffer_result));
    }
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutex();
        if (g_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (g_submit_task == nullptr) {
        g_submit_task = xTaskCreateStatic(
            AiSessionWorkerTask,
            "wqn_ai_worker",
            kSubmitTaskStackBytes,
            nullptr,
            5,
            g_submit_task_stack,
            &g_submit_task_tcb);
        if (g_submit_task == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(kTag, "ai-session build: %s %s", __DATE__, __TIME__);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    LoadTodaySessionLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

void SetAiAudioCaptureTapEnabled(bool enabled)
{
    wqn::SetAudioCaptureTap(enabled ? &AudioCaptureTapHandler : nullptr, nullptr);
}

esp_err_t StartAiRecordingSession()
{
    ESP_RETURN_ON_ERROR(InitAiSession(), kTag, "init AI session");

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.status == AiSessionStatus::kPreparingCapture ||
        g_state.status == AiSessionStatus::kListening || g_state.status == AiSessionStatus::kWaitingReply ||
        !g_submit_parked || !g_prepare_parked || g_prepare_active ||
        g_worker_command != AiWorkerCommand::kNone) {
        ESP_LOGW(kTag,
                 "record start rejected: status=%d submit_parked=%d prepare_parked=%d prepare_active=%d command=%d",
                 static_cast<int>(g_state.status), g_submit_parked ? 1 : 0,
                 g_prepare_parked ? 1 : 0, g_prepare_active ? 1 : 0,
                 static_cast<int>(g_worker_command));
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    wqn::runtime::SleepLease sleep_lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kAiSession, "ai-session", __FILE__, __LINE__);
    if (!sleep_lease) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_ai_sleep_lease = std::move(sleep_lease);
    g_state.status = AiSessionStatus::kPreparingCapture;
    g_state.user_text.clear();
    g_state.assistant_text.clear();
    g_state.pending_text = "正在连接 WiFi...";
    g_state.status_detail.clear();
    g_state.function_call_summaries.clear();
    g_state.conversation_id = g_conversation_id;
    g_state.page = 0;
    g_state.scroll_offset_lines = 0;
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    g_prepare_active = true;
    g_prepare_parked = false;
    g_recording_requested = true;
    const uint32_t prepare_generation = ++g_prepare_generation;
    g_prepare_command_generation = prepare_generation;
    g_worker_command = AiWorkerCommand::kPrepareRecording;
    MarkChanged();
    xSemaphoreGive(g_lock);
    xTaskNotifyGive(g_submit_task);
    return ESP_OK;
}

esp_err_t StopAiRecordingAndSubmit()
{
    ESP_RETURN_ON_ERROR(InitAiSession(), kTag, "init AI session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_submit_parked) {
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.status == AiSessionStatus::kPreparingCapture && g_prepare_active) {
        g_recording_requested = false;
        ++g_prepare_generation;
        g_prepare_active = false;
        // Cancels the typed WiFi wait within its 200 ms wake bound without
        // touching the shared worker task from the UI task.
        g_ai_connectivity_demand.Reset();
        SetCancelledBeforeRecordingLocked();
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.status == AiSessionStatus::kPreparingCapture) {
        // The preparation task has not created a capture buffer yet.
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.status == AiSessionStatus::kWaitingReply) {
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.status != AiSessionStatus::kListening) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_prepare_parked || g_worker_command != AiWorkerCommand::kNone ||
        g_submit_task == nullptr) {
        ESP_LOGW(kTag,
                 "record stop rejected: prepare_parked=%d command=%d worker=%p",
                 g_prepare_parked ? 1 : 0, static_cast<int>(g_worker_command),
                 static_cast<void*>(g_submit_task));
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    SetStateLocked(AiSessionStatus::kWaitingReply, "正在停止录音...", "", "");
    g_submit_parked = false;
    g_worker_command = AiWorkerCommand::kSubmitSession;
    xSemaphoreGive(g_lock);

    LogAiMemory("before-submit-dispatch");
    xTaskNotifyGive(g_submit_task);
    return ESP_OK;
}

bool CopyAiSessionToUi(AiSessionState* state)
{
    if (state == nullptr || g_lock == nullptr) {
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

void SetAiTier(AiTier tier)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.tier = tier;
    MarkChanged();
    xSemaphoreGive(g_lock);
}

AiTier GetAiTier()
{
    if (g_lock == nullptr) {
        return AiTier::kStd;
    }
    AiTier result = AiTier::kStd;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    result = g_state.tier;
    xSemaphoreGive(g_lock);
    return result;
}

// [shell->wire] Toggle setters (mirror SetAiTier): update g_state under lock +
// MarkChanged so CopyAiSessionToUi flushes the new value to the UI and the
// request builder (above) reads the up-to-date g_state.thinking_level.
void SetAiThinkingLevel(ThinkingLevel level)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.thinking_level = level;
    MarkChanged();
    xSemaphoreGive(g_lock);
}

void SetAiTtsOn(bool on)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.tts_on = on;
    MarkChanged();
    xSemaphoreGive(g_lock);
}

void SetAiExpandContent(bool expanded)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.expand_content = expanded;
    MarkChanged();
    xSemaphoreGive(g_lock);
}

void ClearAiConversationContext()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // [context-clear] STD/PRO context is "temporarily shared" for one
    // AI-screen visit. On leave we drop the local conversation so the next
    // visit starts fresh; the cloud keeps the saved conversation in
    // esp32_ai_conversations for the future reuse/reopen UI flow.
    g_conversation_id.clear();
    g_state.conversation_id.clear();
    g_state.user_text.clear();
    g_state.assistant_text.clear();
    g_state.assistant_partial.clear();
    g_state.user_partial.clear();
    g_state.pending_text.clear();
    g_state.status_detail.clear();
    g_state.function_call_summaries.clear();
    g_pending_tool_label.clear();
    g_state.status = wqn::AiSessionStatus::kIdle;
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    g_state.page = 0;
    g_state.toast_visible = false;
    g_state.toast_label.clear();
    wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro).Clear();
    ReleaseAiSleepLeaseIfIdleLocked();
    MarkChanged();
    xSemaphoreGive(g_lock);
}

bool CopyAiStreamingStatus(AiStreamingStatusView* view)
{
    if (view == nullptr || g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    view->streaming_active = g_streaming_active;
    view->status = g_state.status;
    view->status_since_ms = g_state.status_since_ms;
    view->last_render_ms = g_state.last_render_ms;
    view->pending_label = RenderStreamingStatusLocked();
    view->tool_label = g_pending_tool_label;
    view->force_full_render = g_streaming_force_full_render;
    if (g_streaming_force_full_render) {
        g_streaming_force_full_render = false;
    }
    if (g_tool_clear_at_ms > 0 && esp_timer_get_time() / 1000 >= g_tool_clear_at_ms) {
        g_pending_tool_label.clear();
        g_tool_clear_at_ms = 0;
        view->tool_label.clear();
        // [tool-display] Keep status_detail in sync with the cleared tool
        // label so the inline status chip stops showing the stale marker.
        if (g_state.status_detail.find('\xF0') != std::string::npos) {
            // Cheap check for an emoji 4-byte prefix (🔧 / ✅ / ❌ are all
            // 4-byte UTF-8). Avoids stomping on human-readable stage labels
            // like "识别完成 320ms" copied in by kState.
            g_state.status_detail.clear();
        }
    }
    xSemaphoreGive(g_lock);
    return true;
}

bool IsAiSessionActive()
{
    if (g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool active =
        g_prepare_active || !g_prepare_parked || !g_submit_parked ||
        g_streaming_active || g_state.status == AiSessionStatus::kPreparingCapture ||
        g_state.status == AiSessionStatus::kListening ||
        g_state.status == AiSessionStatus::kWaitingReply;
    xSemaphoreGive(g_lock);
    return active;
}

// ============================================================================
// v2 toast + scroll helpers
// ============================================================================
//
// These run on the UI task side (key dispatch, status changes). They live in
// ai_session.cpp because they share state with the SSE consumer (`g_lock`,
// `g_state`). They always take the AI mutex briefly so the UI tick sees a
// consistent snapshot.
//
// All toast labels are emitted without an elapsed-seconds counter, per the
// v2 design contract. The recording state retains a local elapsed counter so
// the chip can still flash "录音中 00:04" without that counter being part of
// the canonical pending_text / toast_label string.
void ShowAiToast(const std::string& label)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.toast_label = label;
    g_state.toast_visible = true;
    g_state.toast_since_ms = esp_timer_get_time() / 1000;
    if (label.find("录音") != std::string::npos) {
        g_state.toast_recording_ms = 0;
    }
    MarkChanged();
    xSemaphoreGive(g_lock);
}

void HideAiToast()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.toast_visible) {
        g_state.toast_visible = false;
        g_state.toast_label.clear();
        g_state.toast_recording_ms = 0;
        MarkChanged();
    }
    xSemaphoreGive(g_lock);
}

void SetAiRecordingLabel(int32_t elapsed_ms)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.toast_visible && g_state.status == wqn::AiSessionStatus::kListening) {
        g_state.toast_recording_ms = elapsed_ms;
        char buf[40];
        const int seconds = static_cast<int>(elapsed_ms / 1000);
        const int minutes = seconds / 60;
        const int s = seconds % 60;
        std::snprintf(buf, sizeof(buf), "\xe2\x97\x8f 录音中 %02d:%02d", minutes, s);
        g_state.toast_label = buf;
        MarkChanged();
    }
    xSemaphoreGive(g_lock);
}

void ResetAiScroll()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.scroll_offset_lines != 0) {
        g_state.scroll_offset_lines = 0;
        MarkChanged();
    }
    xSemaphoreGive(g_lock);
}

void SetAiScrollOffsetLinesClamped(int32_t target, int32_t min_scroll, int32_t max_scroll)
{
    if (g_lock == nullptr) {
        return;
    }
    if (min_scroll > max_scroll) {
        return;  // degenerate bounds: fail open, leave the offset untouched
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // Read-clamp-write stays inside ONE lock hold so a streaming auto-follow
    // cannot interleave between reading and writing the offset (a split
    // Get/Set across locks reintroduces a TOCTOU on the scroll state).
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
    if (g_state.scroll_offset_lines != target) {
        g_state.scroll_offset_lines = target;
        MarkChanged();
    }
    xSemaphoreGive(g_lock);
}

void StampScrollNoOpHint()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.scroll_no_op_hint_ms = esp_timer_get_time() / 1000;
    MarkChanged();
    xSemaphoreGive(g_lock);
}

int32_t GetAiScrollOffsetLines()
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    int32_t val = g_state.scroll_offset_lines;
    xSemaphoreGive(g_lock);
    return val;
}

bool IsAiToastVisible()
{
    if (g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool visible = g_state.toast_visible;
    xSemaphoreGive(g_lock);
    return visible;
}

const std::string& CurrentAiToastLabel()
{
    return g_state.toast_label;
}

}  // namespace wqn

#else

namespace wqn {

esp_err_t InitAiSession()
{
    return ESP_OK;
}

void SetAiAudioCaptureTapEnabled(bool) {}

esp_err_t StartAiRecordingSession()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t StopAiRecordingAndSubmit()
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool CopyAiSessionToUi(AiSessionState*)
{
    return false;
}

void ClearAiConversationContext() {}

void SetAiTier(AiTier tier)
{
    (void)tier;
}

AiTier GetAiTier()
{
    return AiTier::kStd;
}

void SetAiThinkingLevel(ThinkingLevel) {}
void SetAiTtsOn(bool) {}
void SetAiExpandContent(bool) {}

int32_t GetAiScrollOffsetLines()
{
    return 0;
}

bool CopyAiStreamingStatus(AiStreamingStatusView* view)
{
    if (view != nullptr) {
        view->streaming_active = false;
        view->force_full_render = false;
        view->status = AiSessionStatus::kIdle;
        view->status_since_ms = 0;
        view->last_render_ms = 0;
    }
    return false;
}

void ShowAiToast(const std::string&) {}
void HideAiToast() {}
void SetAiRecordingLabel(int32_t) {}
void ResetAiScroll() {}
void SetAiScrollOffsetLinesClamped(int32_t, int32_t, int32_t) {}
void StampScrollNoOpHint() {}
bool IsAiToastVisible() { return false; }
const std::string& CurrentAiToastLabel()
{
    static const std::string empty;
    return empty;
}

}  // namespace wqn

#endif
