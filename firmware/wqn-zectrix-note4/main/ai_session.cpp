#include "ai_session.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>

#include "audio_capture.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage.h"
#include "wifi_manager.h"
#include "wqn_api.h"

namespace {

constexpr char kTag[] = "wqn_ai";
constexpr int kMinAudioDurationMs = 300;
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

void MarkChanged()
{
    g_changed = true;
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
    MarkChanged();
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
    MarkChanged();
}

bool IsCurrentPrepareTaskLocked(uint32_t generation)
{
    return g_prepare_active && generation == g_prepare_generation;
}

void FinishPrepareTaskLocked(uint32_t generation)
{
    if (generation == g_prepare_generation) {
        g_prepare_active = false;
        g_prepare_task = nullptr;
    }
}

bool HasEffectiveSpeech(const wqn::AudioCaptureChunk& audio)
{
    return audio.duration_ms >= kMinAudioDurationMs && !audio.samples.empty() && audio.peak >= kMinAudioPeak &&
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

std::string CurrentLocalDay()
{
    std::time_t now = 0;
    std::time(&now);
    if (now < 1704067200) {
        return "";
    }
    std::tm local = {};
    localtime_r(&now, &local);
    const int year = std::clamp(local.tm_year + 1900, 0, 9999);
    const int month = std::clamp(local.tm_mon + 1, 1, 12);
    const int day = std::clamp(local.tm_mday, 1, 31);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return std::string(buffer);
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

void SaveTodaySessionLocked(const wqn::WqnAiChatResponse& response)
{
    const std::string day = CurrentLocalDay();
    if (day.empty()) {
        return;
    }

    wqn::CachedAiSession session;
    session.day = day;
    session.conversation_id = g_conversation_id;
    session.transcript = g_state.user_text;
    session.reply_text = response.reply_text.empty() ? g_state.assistant_text : response.reply_text;
    session.status_detail = g_state.status_detail;
    session.latency_ms = response.latency_ms;
    for (const auto& call : response.function_calls) {
        if (!call.display.empty()) {
            session.function_call_summaries.push_back(call.display);
        }
    }
    if (session.function_call_summaries.empty()) {
        const std::string action_summary = BuildAiActionSummary(response.actions);
        if (!action_summary.empty()) {
            session.function_call_summaries.push_back(action_summary);
        }
    }

    const esp_err_t saved = wqn::SaveAiSessionForDay(session);
    if (saved != ESP_OK) {
        ESP_LOGW(kTag, "save AI session failed: %s", esp_err_to_name(saved));
    }
}

void LoadTodaySessionLocked()
{
    if (g_loaded_today) {
        return;
    }
    g_loaded_today = true;

    const std::string day = CurrentLocalDay();
    if (day.empty()) {
        return;
    }

    wqn::CachedAiSession session;
    const esp_err_t loaded = wqn::LoadAiSessionForDay(day, &session);
    if (loaded != ESP_OK || session.day.empty()) {
        return;
    }

    g_conversation_id = session.conversation_id;
    g_state.status = wqn::AiSessionStatus::kReplyReady;
    g_state.user_text = session.transcript;
    g_state.assistant_text = session.reply_text;
    g_state.pending_text.clear();
    g_state.status_detail = session.status_detail;
    g_state.function_call_summaries = session.function_call_summaries;
    g_state.conversation_id = g_conversation_id;
    g_state.page = 0;
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
}

void SubmitTask(void*)
{
    wqn::AudioCaptureChunk audio;
    esp_err_t result = wqn::StopAudioCapture(&audio);

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK) {
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("录音停止失败");
        g_submit_task = nullptr;
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag,
             "audio captured: duration_ms=%d mono_samples=%u peak=%d rms=%d",
             audio.duration_ms,
             static_cast<unsigned>(audio.samples.size()),
             static_cast<int>(audio.peak),
             audio.rms);

    if (audio.duration_ms < kMinAudioDurationMs || audio.samples.empty()) {
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("录音太短");
        g_submit_task = nullptr;
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    if (!HasEffectiveSpeech(audio)) {
        wqn::ReleaseAudioCapturePower();
        SetErrorLocked("未检测到有效语音");
        g_submit_task = nullptr;
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    if (audio.duration_ms > kMaxAudioDurationMs) {
        audio.duration_ms = kMaxAudioDurationMs;
    }
    SetStateLocked(wqn::AiSessionStatus::kWaitingReply, "正在上传语音...", "", "");
    xSemaphoreGive(g_lock);

    std::string token;
    result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK || !wqn::IsValidAccessToken(token)) {
        wqn::ReleaseAudioCapturePower();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        SetErrorLocked("设备未配对");
        g_submit_task = nullptr;
        xSemaphoreGive(g_lock);
        vTaskDelete(nullptr);
        return;
    }

    wqn::WqnAiChatResponse response;
    result = wqn::UploadAiAudioChat(
        token,
        reinterpret_cast<const uint8_t*>(audio.samples.data()),
        audio.samples.size() * sizeof(int16_t),
        audio.duration_ms,
        g_conversation_id,
        &response);
    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result == ESP_OK) {
        g_conversation_id = response.conversation_id;
        g_state.status = wqn::AiSessionStatus::kReplyReady;
        g_state.pending_text.clear();
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
        SaveTodaySessionLocked(response);
        MarkChanged();
        ESP_LOGI(kTag, "AI response ready: latency_ms=%d transcript_bytes=%u reply_bytes=%u actions=%u",
                 response.latency_ms,
                 static_cast<unsigned>(response.transcript.size()),
                 static_cast<unsigned>(response.reply_text.size()),
                 static_cast<unsigned>(response.actions.size()));
    } else {
        SetErrorLocked(MessageForAiRequestFailure(response, result));
    }
    g_submit_task = nullptr;
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
        result = wqn::StartWifiStationIfEnabled();
        if (result == ESP_OK && !wqn::IsWifiStationConnected()) {
            result = wqn::WaitForWifiStationConnected(kWifiReadyWait);
        }
        if (result != ESP_OK) {
            failure = PrepareFailure::kWifi;
        }
    }

    bool should_start_capture = false;
    if (result == ESP_OK) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        should_start_capture = g_recording_requested && IsCurrentPrepareTaskLocked(generation) &&
                               g_state.status == wqn::AiSessionStatus::kWaitingReply && g_submit_task == nullptr;
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
            g_state.status == wqn::AiSessionStatus::kWaitingReply) {
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
        } else if (current && !g_recording_requested && g_state.status == wqn::AiSessionStatus::kWaitingReply) {
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
                                 g_state.status == wqn::AiSessionStatus::kWaitingReply && g_submit_task == nullptr;
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
        g_state.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        FinishPrepareTaskLocked(generation);
        xSemaphoreGive(g_lock);
        ESP_LOGI(kTag, "AI recording started after WiFi ready");
        vTaskDelete(nullptr);
        return;
    }

    const bool current = IsCurrentPrepareTaskLocked(generation);
    if (result != ESP_OK && still_requested) {
        SetErrorLocked(std::string("录音启动失败: ") + esp_err_to_name(result));
    } else if (current && !still_requested && g_state.status == wqn::AiSessionStatus::kWaitingReply) {
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
    if (g_state.status == AiSessionStatus::kListening || g_state.status == AiSessionStatus::kWaitingReply ||
        g_submit_task != nullptr || g_prepare_active) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(g_lock);

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.status = AiSessionStatus::kWaitingReply;
    g_state.user_text.clear();
    g_state.assistant_text.clear();
    g_state.pending_text = "正在连接 WiFi...";
    g_state.status_detail.clear();
    g_state.function_call_summaries.clear();
    g_state.conversation_id = g_conversation_id;
    g_state.page = 0;
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
    if (g_state.status == AiSessionStatus::kWaitingReply && g_prepare_active) {
        g_recording_requested = false;
        ++g_prepare_generation;
        g_prepare_active = false;
        g_prepare_task = nullptr;
        SetCancelledBeforeRecordingLocked();
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

void SetAiTier(AiTier tier)
{
    (void)tier;
}

AiTier GetAiTier()
{
    return AiTier::kStd;
}

}  // namespace wqn

#endif
