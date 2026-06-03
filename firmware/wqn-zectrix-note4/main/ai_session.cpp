#include "ai_session.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdio>
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
constexpr TickType_t kWifiStartWait = pdMS_TO_TICKS(1500);

SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_submit_task = nullptr;
wqn::AiSessionState g_state;
std::string g_conversation_id;
bool g_changed = false;
bool g_loaded_today = false;

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
    if (response.error_code == "model_failed") {
        return "模型回复失败";
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
        if (action.type == "todo_status_updated" && action.status == "completed") {
            const std::string title = action.title.empty() ? "Todo" : action.title;
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += "已完成 Todo：" + title;
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

    std::string token;
    esp_err_t result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK || !wqn::IsValidAccessToken(token)) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        SetErrorLocked("设备未配对，请先在 Web 端创建配对");
        xSemaphoreGive(g_lock);
        ESP_LOGW(kTag, "AI recording blocked: no valid token");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(wqn::StartWifiStationIfEnabled(), kTag, "start WiFi for AI");
    if (!wqn::IsWifiStationConnected()) {
        result = wqn::WaitForWifiStationConnected(kWifiStartWait);
        if (result != ESP_OK) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            SetErrorLocked("WiFi 未连接");
            xSemaphoreGive(g_lock);
            ESP_LOGW(kTag, "AI recording blocked: WiFi offline");
            return result;
        }
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.status == AiSessionStatus::kListening || g_state.status == AiSessionStatus::kWaitingReply ||
        g_submit_task != nullptr) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_state.status = AiSessionStatus::kListening;
    g_state.user_text.clear();
    g_state.assistant_text.clear();
    g_state.pending_text = "正在录音...";
    g_state.status_detail.clear();
    g_state.function_call_summaries.clear();
    g_state.conversation_id = g_conversation_id;
    g_state.page = 0;
    g_state.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
    xSemaphoreGive(g_lock);

    result = StartAudioCapture();
    if (result != ESP_OK) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        SetErrorLocked(std::string("录音启动失败: ") + esp_err_to_name(result));
        xSemaphoreGive(g_lock);
        return result;
    }
    ESP_LOGI(kTag, "AI recording started");
    return ESP_OK;
}

esp_err_t StopAiRecordingAndSubmit()
{
    ESP_RETURN_ON_ERROR(InitAiSession(), kTag, "init AI session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_submit_task != nullptr) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
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

}  // namespace wqn

#endif
