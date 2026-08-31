#include "opencode_session.h"

#include <algorithm>
#include <utility>

#include "ai_session.h"
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
    kPrepareCapture,
    kTranscribe,
    kRunPrompt,
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
std::string g_run_session_id;
std::string g_run_prompt;
wqn::runtime::SleepLease g_agent_sleep_lease;
wqn::services::ConnectivityDemand g_connectivity_demand;

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
            g_state.ui.context_label.clear();
            g_state.ui.prompt_text.clear();
            g_state.ui.response_text.clear();
            g_state.ui.scroll_offset_lines = 0;
            g_state.ui.requires_confirmation = false;
            g_state.confirmation_armed_at_ms = 0;
            g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
            g_state.ui.status_label = "选择 Session";
            g_state.ui.activity_text = "上下选择，确认锁定";
            g_state.ui.action_hint = "↑/↓ 选择 · 确认锁定";
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
    xSemaphoreTake(g_lock, portMAX_DELAY);
    should_capture = result == ESP_OK && g_recording_requested;
    xSemaphoreGive(g_lock);
    if (should_capture) {
        // AudioCapture is shared with the legacy AI page, whose tap forwards
        // PCM to its Std/Pro WebSocket. Agent capture is ASR-only until the
        // user confirms, so isolate the microphone before starting it.
        if (wqn::IsAudioCaptureRunning()) {
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
        SetErrorLocked(result == ESP_ERR_INVALID_STATE
            ? "设备未配对或网络不可用"
            : "录音启动失败");
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

void OnOpenCodeEvent(const wqn::OpenCodeEvent& event, void*)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    switch (event.kind) {
        case wqn::OpenCodeEventKind::kAccepted:
            SetPhaseLocked(wqn::AiFeaturePhase::kRunning, "Agent 执行中");
            g_state.ui.activity_text = "OpenCode 已接收任务";
            break;
        case wqn::OpenCodeEventKind::kStatus:
            if (event.status == "idle") {
                g_state.stream_active = false;
                if (!g_run_failed) {
                    SetPhaseLocked(wqn::AiFeaturePhase::kComplete, "执行完成");
                    g_state.ui.action_hint = "长按确认发起新任务";
                }
            } else if (event.status == "retry") {
                SetPhaseLocked(wqn::AiFeaturePhase::kRunning, "Agent 重试中");
                g_state.ui.activity_text = event.text;
            } else {
                SetPhaseLocked(wqn::AiFeaturePhase::kRunning, "Agent 执行中");
            }
            break;
        case wqn::OpenCodeEventKind::kTextDelta:
            if (g_state.ui.response_text.size() < kMaxAgentTextBytes) {
                const size_t remaining = kMaxAgentTextBytes - g_state.ui.response_text.size();
                g_state.ui.response_text.append(event.text.data(), std::min(remaining, event.text.size()));
            }
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kText:
            g_state.ui.response_text = event.text.substr(0, kMaxAgentTextBytes);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kTool:
            g_state.ui.activity_text = event.tool;
            if (!event.status.empty()) {
                g_state.ui.activity_text += " · " + event.status;
            }
            if (!event.preview.empty()) {
                g_state.ui.activity_text += " · " + event.preview;
            }
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kPermission:
            g_state.ui.status_label = "等待权限";
            g_state.ui.activity_text = event.text;
            if (!event.preview.empty()) {
                g_state.ui.activity_text += " · " + event.preview;
            }
            g_state.ui.action_hint = "请在 OpenCode 端审批";
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kError:
            g_run_failed = true;
            g_state.ui.phase = wqn::AiFeaturePhase::kError;
            g_state.ui.status_label = "执行失败";
            g_state.ui.activity_text = event.text;
            g_state.ui.action_hint = "长按确认重试新任务";
            MarkChangedLocked();
            break;
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
            token, g_run_session_id, g_run_prompt, OnOpenCodeEvent, nullptr, &api_result);
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
            case WorkerCommand::kPrepareCapture:
                PrepareCapture();
                break;
            case WorkerCommand::kTranscribe:
                Transcribe();
                break;
            case WorkerCommand::kRunPrompt:
                RunPrompt();
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
    g_state.ui.context_label = selected.title;
    g_state.ui.phase = AiFeaturePhase::kIdle;
    g_state.ui.status_label = "就绪";
    g_state.ui.activity_text = "长按确认键语音输入";
    g_state.ui.action_hint = "长按确认录音 · ↑/↓ 滚动";
    g_state.confirmation_armed_at_ms = 0;
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
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

void ScrollOpenCodeResponse(int direction)
{
    if (g_lock == nullptr || direction == 0) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const int32_t next = g_state.ui.scroll_offset_lines + direction * 4;
    g_state.ui.scroll_offset_lines = std::max<int32_t>(0, next);
    MarkChangedLocked();
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
