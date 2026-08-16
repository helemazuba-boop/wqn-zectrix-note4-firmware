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
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "runtime/sleep_coordinator.h"
#include "services/connectivity_service.h"
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
constexpr TickType_t kWifiReadyWait = pdMS_TO_TICKS(15000);

SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_submit_task = nullptr;
TaskHandle_t g_prepare_task = nullptr;
wqn::AiSessionState g_state;
std::string g_conversation_id;
bool g_changed = false;
bool g_loaded_today = false;
bool g_prepare_active = false;
bool g_recording_requested = false;
uint32_t g_prepare_generation = 0;
bool g_streaming_active = false;        // true while SubmitTask is parsing SSE events
bool g_streaming_force_full_render = false; // when true the next UI tick does a full refresh
wqn::runtime::SleepLease g_ai_sleep_lease;
std::string g_pending_tool_label;        // "🔧 create_todo…" or "✅ ..." for status bar
int64_t g_tool_clear_at_ms = 0;          // scheduled status-bar clear

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
};
StdProTurnAssembly g_turn;

void LogAiMemory(const char* stage)
{
    ESP_LOGI(
        kTag,
        "memory stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u stack_hwm=%u",
        stage,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
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
    if (!g_prepare_active && g_prepare_task == nullptr && g_submit_task == nullptr &&
        !g_streaming_active && !state_active) {
        g_ai_sleep_lease.Reset();
    }
}

void FinishSubmitTaskLocked()
{
    g_submit_task = nullptr;
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
// SubmitTask drives the SSE stream on its own task stack. Each callback runs on
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
            }
            g_state.last_render_ms = now_ms;
            break;
        case wqn::WqnAiSseEvent::Kind::kTextDelta:
            if (!g_turn.assistant_terminal) {
                g_state.assistant_partial += ev.delta;
                g_turn.assistant_text += ev.delta;
                g_state.last_render_ms = now_ms;
            }
            break;
        case wqn::WqnAiSseEvent::Kind::kTextEnd: {
            const std::string final_text = !ev.full_text.empty() ? ev.full_text
                : (!ev.text.empty() ? ev.text : g_turn.assistant_text);
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
            history.PopLastIf(wqn::ChatMessageKind::kToolStart);
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
            FinalizeAssistantLocked(history, g_turn.assistant_text, now_ms);
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
            const std::string final_text = !ev.full_text.empty() ? ev.full_text
                : (!g_turn.assistant_text.empty() ? g_turn.assistant_text : g_state.assistant_text);
            g_turn.assistant_terminal = true;
            FinalizeThinkingLocked(history, std::string(), now_ms);
            FinalizeAssistantLocked(history, final_text, now_ms);
            g_state.assistant_text = final_text;
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
    MarkChanged();
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
    if (g_prepare_task == xTaskGetCurrentTaskHandle() || generation == g_prepare_generation) {
        g_prepare_task = nullptr;
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

void SubmitTask(void*)
{
    LogAiMemory("submit-start");
    wqn::AudioCaptureChunk audio;
    esp_err_t result = wqn::StopAudioCapture(&audio);

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK) {
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("录音停止失败");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag,
             "audio captured: duration_ms=%d mono_samples=%u peak=%d rms=%d",
             audio.duration_ms,
             static_cast<unsigned>(audio.sample_count),
             static_cast<int>(audio.peak),
             audio.rms);

    if (audio.duration_ms < kMinAudioDurationMs || audio.sample_count < kMinAudioSamples) {
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("录音太短");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    if (!HasEffectiveSpeech(audio)) {
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("未检测到有效语音");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    if (audio.duration_ms > kMaxAudioDurationMs) {
        audio.duration_ms = kMaxAudioDurationMs;
    }
    SetStateLocked(wqn::AiSessionStatus::kWaitingReply, "正在上传语音...", "", "");
    g_state.toast_label = "● 上传…";
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
        wqn::ReleaseAudioCapturePower();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_streaming_active = false;
        SetErrorLocked("设备未配对");
        FinishSubmitTaskLocked();
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t submit_result = ESP_OK;
    wqn::WqnAiChatResponse response;
    bool used_streaming = false;

#if CONFIG_WQN_AI_STREAMING_ENABLE
    // v2 SSE path
    wqn::WqnAiStreamRequest req;
    req.token = token;
    req.pcm_data = audio.samples;
    req.pcm_sample_count = audio.sample_count;
    req.duration_ms = audio.duration_ms;
    req.tier = tier_str;
    req.conversation_id = conversation_id;
    // Thinking params from the snapshot (tier + thinking_level captured under
    // lock above). The cloud maps the bounded level to provider parameters.
    {
        const char* effort = "medium";
        switch (thinking_level) {
            case wqn::ThinkingLevel::kOff: effort = "low"; break;  // StepFun has no off; low is closest
            case wqn::ThinkingLevel::kLow: effort = "low"; break;
            case wqn::ThinkingLevel::kMed: effort = "medium"; break;
            case wqn::ThinkingLevel::kHigh: effort = "high"; break;
            default: break;
        }
        req.reasoning_effort = effort;
        req.enable_thinking = (thinking_level != wqn::ThinkingLevel::kOff);
    }
    req.request_id = GenerateRequestId(); // helper below
    req.callback = &TrampolineSseEvent;
    req.user_ctx = nullptr;
    LogAiMemory("before-sse-upload");
    submit_result = wqn::UploadAiAudioChatStream(req, &response);
    LogAiMemory("after-sse-upload");
    used_streaming = true;
#else
    // v1 fallback path (kept behind CONFIG_WQN_AI_V1_FALLBACK for debug compare).
    submit_result = wqn::UploadAiAudioChat(
        token,
        reinterpret_cast<const uint8_t*>(audio.samples),
        audio.sample_count * sizeof(int16_t),
        audio.duration_ms,
        conversation_id,
        tier_str,
        &response);
#endif

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

    FinishSubmitTaskLocked();
    xSemaphoreGive(g_lock);
    vTaskDelete(nullptr);
}

void PrepareRecordingTask(void* parameter)
{
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parameter));
    std::string token;
    enum class PrepareFailure {
        kNone,
        kNoToken,
        kInvalidToken,
        kWifi,
    };
    PrepareFailure failure = PrepareFailure::kNone;

    esp_err_t result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK || token.empty()) {
        failure = PrepareFailure::kNoToken;
        result = ESP_ERR_INVALID_STATE;
    } else if (!wqn::IsValidAccessToken(token)) {
        failure = PrepareFailure::kInvalidToken;
        result = ESP_ERR_INVALID_STATE;
    } else {
        result = wqn::services::StartConnectivity();
        if (result == ESP_OK && !wqn::services::IsConnectivityOnline()) {
            result = wqn::services::WaitForConnectivity(kWifiReadyWait);
        }
        if (result != ESP_OK) {
            failure = PrepareFailure::kWifi;
        }
    }

    bool should_start_capture = false;
    if (result == ESP_OK) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        should_start_capture = g_recording_requested && IsCurrentPrepareTaskLocked(generation) &&
                               g_state.status == wqn::AiSessionStatus::kPreparingCapture && g_submit_task == nullptr;
        if (should_start_capture) {
            g_state.pending_text = "正在启动录音...";
            g_state.status_since_ms = esp_timer_get_time() / 1000;
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
                SetErrorLocked("WiFi 未连接或未配置");
                ESP_LOGW(kTag, "AI recording blocked: WiFi unavailable: %s", esp_err_to_name(result));
            }
        } else if (current && !g_recording_requested && g_state.status == wqn::AiSessionStatus::kPreparingCapture) {
            SetCancelledBeforeRecordingLocked();
        }
        if (current) {
            g_recording_requested = false;
        }
        FinishPrepareTaskLocked(generation);
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    result = wqn::StartAudioCapture();

    bool stop_started_capture = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool still_requested = g_recording_requested && IsCurrentPrepareTaskLocked(generation) &&
                                 g_state.status == wqn::AiSessionStatus::kPreparingCapture && g_submit_task == nullptr;
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
        ESP_LOGI(kTag, "AI recording started after WiFi ready; toast=录音中");
        vTaskDelete(nullptr);
        return;
    }

    const bool current = IsCurrentPrepareTaskLocked(generation);
    if (result != ESP_OK && still_requested) {
        SetErrorLocked(std::string("录音启动失败: ") + esp_err_to_name(result));
    } else if (current && !still_requested && g_state.status == wqn::AiSessionStatus::kPreparingCapture) {
        SetCancelledBeforeRecordingLocked();
    }
    stop_started_capture = result == ESP_OK;
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
    vTaskDelete(nullptr);
}

}  // namespace

namespace wqn {

esp_err_t InitAiSession()
{
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
    xSemaphoreTake(g_lock, portMAX_DELAY);
    LoadTodaySessionLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t StartAiRecordingSession()
{
    ESP_RETURN_ON_ERROR(InitAiSession(), kTag, "init AI session");

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.status == AiSessionStatus::kPreparingCapture ||
        g_state.status == AiSessionStatus::kListening || g_state.status == AiSessionStatus::kWaitingReply ||
        g_submit_task != nullptr || g_prepare_task != nullptr || g_prepare_active) {
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
    g_recording_requested = true;
    const uint32_t prepare_generation = ++g_prepare_generation;
    g_prepare_task = nullptr;
    MarkChanged();
    xSemaphoreGive(g_lock);

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreate(PrepareRecordingTask,
                                           "wqn_ai_prepare",
                                           6144,
                                           reinterpret_cast<void*>(static_cast<uintptr_t>(prepare_generation)),
                                           5,
                                           &task);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (created != pdPASS) {
        g_recording_requested = false;
        FinishPrepareTaskLocked(prepare_generation);
        SetErrorLocked("AI 任务创建失败");
        xSemaphoreGive(g_lock);
        return ESP_ERR_NO_MEM;
    }
    if (IsCurrentPrepareTaskLocked(prepare_generation)) {
        g_prepare_task = task;
    }
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t StopAiRecordingAndSubmit()
{
    ESP_RETURN_ON_ERROR(InitAiSession(), kTag, "init AI session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_submit_task != nullptr) {
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.status == AiSessionStatus::kPreparingCapture && g_prepare_active) {
        g_recording_requested = false;
        ++g_prepare_generation;
        g_prepare_active = false;
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
    SetStateLocked(AiSessionStatus::kWaitingReply, "正在停止录音...", "", "");
    xSemaphoreGive(g_lock);

    LogAiMemory("before-submit-task-create");
    const BaseType_t created = xTaskCreate(SubmitTask, "wqn_ai_submit", 12288, nullptr, 5, &g_submit_task);
    if (created != pdPASS) {
        AudioCaptureChunk discarded;
        StopAudioCapture(&discarded);
        ReleaseAudioCapturePower();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        SetErrorLocked("AI 任务创建失败");
        xSemaphoreGive(g_lock);
        return ESP_ERR_NO_MEM;
    }
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
        g_prepare_active || g_prepare_task != nullptr || g_submit_task != nullptr ||
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
