#include "services/connectivity_service.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "online_sync.h"
#include "provision_manager.h"
#include "runtime/sleep_coordinator.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "connectivity";
constexpr UBaseType_t kCommandQueueDepth = 12;
constexpr UBaseType_t kReplyQueueDepth = 8;
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 5;
constexpr int kFastRetryLimit = 5;
constexpr int64_t kFastRetryDelayUs = 5LL * 1000 * 1000;
constexpr int64_t kBackoffDelayUs = 60LL * 1000 * 1000;
constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMaxPasswordBytes = 64;
constexpr EventBits_t kOnlineBit = BIT0;

enum class CommandType : uint8_t {
    kStart,
    kStartWithCredentials,
    kBeginProvisioning,
    kWifiStarted,
    kWifiConnected,
    kWifiDisconnected,
    kProvisioned,
    kPrepareSleep,
    kRollbackSleep,
};

struct ConnectivityCommand {
    CommandType type = CommandType::kStart;
    uint32_t request_id = 0;
    char ssid[kMaxSsidBytes + 1] = {};
    char password[kMaxPasswordBytes + 1] = {};
    wqn::power::PrepareSleepCommand sleep = {};
    int reason = 0;
    int rssi = 0;
};

struct ConnectivityReply {
    uint32_t request_id = 0;
    esp_err_t result = ESP_FAIL;
};

StaticQueue_t g_command_queue_storage;
uint8_t g_command_queue_buffer[
    kCommandQueueDepth * sizeof(ConnectivityCommand)] = {};
QueueHandle_t g_command_queue = nullptr;

StaticQueue_t g_reply_queue_storage;
uint8_t g_reply_queue_buffer[kReplyQueueDepth * sizeof(ConnectivityReply)] = {};
QueueHandle_t g_reply_queue = nullptr;

StaticEventGroup_t g_event_group_storage;
EventGroupHandle_t g_event_group = nullptr;
StaticSemaphore_t g_call_mutex_storage;
SemaphoreHandle_t g_call_mutex = nullptr;

TaskHandle_t g_task = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;
uint32_t g_next_request_id = 1;

std::atomic<wqn::services::ConnectivityState> g_state{
    wqn::services::ConnectivityState::kOff};
std::atomic<bool> g_online{false};
std::atomic<int> g_rssi{0};

int g_fast_retry_count = 0;
int64_t g_next_action_us = 0;
bool g_resume_provisioning = false;
wqn::runtime::SleepLease g_connectivity_lease;

const char* StateName(wqn::services::ConnectivityState state)
{
    switch (state) {
        case wqn::services::ConnectivityState::kOff:
            return "off";
        case wqn::services::ConnectivityState::kProvisioning:
            return "provisioning";
        case wqn::services::ConnectivityState::kConnecting:
            return "connecting";
        case wqn::services::ConnectivityState::kOnline:
            return "online";
        case wqn::services::ConnectivityState::kBackoff:
            return "backoff";
        case wqn::services::ConnectivityState::kQuiescing:
            return "quiescing";
        default:
            return "unknown";
    }
}

void SetState(wqn::services::ConnectivityState state)
{
    if (state != wqn::services::ConnectivityState::kConnecting &&
        state != wqn::services::ConnectivityState::kProvisioning) {
        g_connectivity_lease.Reset();
    }
    const wqn::services::ConnectivityState previous =
        g_state.exchange(state, std::memory_order_acq_rel);
    if (previous != state) {
        ESP_LOGI(kTag, "state: %s -> %s", StateName(previous), StateName(state));
    }
}

bool HoldConnectivityLease()
{
    if (g_connectivity_lease) {
        return true;
    }
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kConnectivity,
        "connectivity",
        __FILE__,
        __LINE__);
    if (!lease) {
        return false;
    }
    g_connectivity_lease = std::move(lease);
    return true;
}

void SetOnline(bool online, int rssi = 0)
{
    g_online.store(online, std::memory_order_release);
    g_rssi.store(online ? rssi : 0, std::memory_order_release);
    if (g_event_group != nullptr) {
        if (online) {
            xEventGroupSetBits(g_event_group, kOnlineBit);
        } else {
            xEventGroupClearBits(g_event_group, kOnlineBit);
        }
    }
}

void CopyCredential(char* destination, size_t destination_size, const char* value)
{
    if (destination == nullptr || destination_size == 0) {
        return;
    }
    std::snprintf(destination, destination_size, "%s", value == nullptr ? "" : value);
}

bool PostAsyncCommand(const ConnectivityCommand& command)
{
    if (g_command_queue == nullptr ||
        xQueueSend(g_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "drop async command: type=%u", static_cast<unsigned>(command.type));
        return false;
    }
    return true;
}

void WifiEventSink(wqn::WifiStationEvent event, int reason, int rssi)
{
    ConnectivityCommand command;
    command.reason = reason;
    command.rssi = rssi;
    switch (event) {
        case wqn::WifiStationEvent::kStarted:
            command.type = CommandType::kWifiStarted;
            break;
        case wqn::WifiStationEvent::kConnected:
            command.type = CommandType::kWifiConnected;
            break;
        case wqn::WifiStationEvent::kDisconnected:
            command.type = CommandType::kWifiDisconnected;
            break;
    }
    PostAsyncCommand(command);
}

#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
void ProvisionDone(const std::string& ssid, const std::string& password)
{
    ConnectivityCommand command;
    command.type = CommandType::kProvisioned;
    CopyCredential(command.ssid, sizeof(command.ssid), ssid.c_str());
    CopyCredential(command.password, sizeof(command.password), password.c_str());
    PostAsyncCommand(command);
}
#endif

void ScheduleConnectRetry()
{
    g_next_action_us = esp_timer_get_time() + kFastRetryDelayUs;
}

void ScheduleBackoff()
{
    SetOnline(false);
    SetState(wqn::services::ConnectivityState::kBackoff);
    g_fast_retry_count = 0;
    g_next_action_us = esp_timer_get_time() + kBackoffDelayUs;
    const esp_err_t stop_result = wqn::StopWifiStationRadio();
    if (stop_result != ESP_OK) {
        ESP_LOGW(kTag, "stop WiFi for backoff failed: %s", esp_err_to_name(stop_result));
    }
    ESP_LOGW(kTag, "WiFi fast retries exhausted; radio off for 60 seconds");
}

esp_err_t BeginProvisioning()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }
    SetOnline(false);
    g_fast_retry_count = 0;
    g_next_action_us = 0;
    SetState(wqn::services::ConnectivityState::kProvisioning);
    const esp_err_t result = wqn::StartProvisioningMode();
    if (result != ESP_OK) {
        SetState(wqn::services::ConnectivityState::kOff);
    }
    return result;
#else
    SetState(wqn::services::ConnectivityState::kOff);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t StartConfiguredConnectivity()
{
#if !CONFIG_WQN_WIFI_STA_ENABLE
    SetState(wqn::services::ConnectivityState::kOff);
    return ESP_OK;
#else
    if (g_state.load(std::memory_order_acquire) ==
        wqn::services::ConnectivityState::kQuiescing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_online.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    if (wqn::IsProvisioningActive()) {
        SetState(wqn::services::ConnectivityState::kProvisioning);
        return ESP_OK;
    }

    if (g_state.load(std::memory_order_acquire) ==
            wqn::services::ConnectivityState::kBackoff &&
        wqn::IsWifiStationInitialized()) {
        if (!HoldConnectivityLease()) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t restart_result = wqn::StartWifiStationRadio();
        if (restart_result != ESP_OK) {
            g_connectivity_lease.Reset();
            return restart_result;
        }
        SetState(wqn::services::ConnectivityState::kConnecting);
        ScheduleConnectRetry();
        return ESP_OK;
    }

    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = wqn::StartWifiStationIfEnabled();
    if (result == ESP_ERR_NOT_FOUND) {
        return BeginProvisioning();
    }
    if (result != ESP_OK) {
        g_connectivity_lease.Reset();
        SetState(wqn::services::ConnectivityState::kBackoff);
        g_next_action_us = esp_timer_get_time() + kBackoffDelayUs;
        return result;
    }
    SetState(wqn::services::ConnectivityState::kConnecting);
    ScheduleConnectRetry();
    return ESP_OK;
#endif
}

esp_err_t StartWithCredentials(const ConnectivityCommand& command)
{
#if !CONFIG_WQN_WIFI_STA_ENABLE
    (void)command;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (g_state.load(std::memory_order_acquire) ==
        wqn::services::ConnectivityState::kQuiescing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (command.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result =
        wqn::StartWifiWithCredentials(command.ssid, command.password);
    if (result != ESP_OK) {
        g_connectivity_lease.Reset();
        SetState(wqn::services::ConnectivityState::kBackoff);
        g_next_action_us = esp_timer_get_time() + kBackoffDelayUs;
        return result;
    }
    SetOnline(false);
    SetState(wqn::services::ConnectivityState::kConnecting);
    g_fast_retry_count = 0;
    ScheduleConnectRetry();
    return ESP_OK;
#endif
}

esp_err_t PrepareForSleep(const wqn::power::PrepareSleepCommand& command)
{
    if (command.deadline_us > 0 && esp_timer_get_time() >= command.deadline_us) {
        return ESP_ERR_TIMEOUT;
    }
    g_resume_provisioning = wqn::IsProvisioningActive();
    SetState(wqn::services::ConnectivityState::kQuiescing);
    g_next_action_us = 0;

    if (g_resume_provisioning) {
        const esp_err_t provision_result = wqn::StopProvisioningMode();
        if (provision_result != ESP_OK) {
            return provision_result;
        }
    }
    const esp_err_t result = wqn::PrepareConnectivityForSleep(command);
    if (result == ESP_OK) {
        SetOnline(false);
    }
    return result;
}

esp_err_t RollbackAfterSleepAbort()
{
    wqn::RollbackConnectivityAfterSleepAbort();
    if (g_resume_provisioning) {
        g_resume_provisioning = false;
        return BeginProvisioning();
    }
    if (wqn::IsWifiStationConnected()) {
        SetOnline(true, wqn::GetWifiRssi());
        SetState(wqn::services::ConnectivityState::kOnline);
        return ESP_OK;
    }
    if (wqn::IsWifiStationInitialized()) {
        if (!HoldConnectivityLease()) {
            return ESP_ERR_INVALID_STATE;
        }
        SetOnline(false);
        SetState(wqn::services::ConnectivityState::kConnecting);
        g_fast_retry_count = 0;
        ScheduleConnectRetry();
        return ESP_OK;
    }
    SetState(wqn::services::ConnectivityState::kOff);
    return ESP_OK;
}

esp_err_t HandleCommand(const ConnectivityCommand& command)
{
    const wqn::services::ConnectivityState state =
        g_state.load(std::memory_order_acquire);
    switch (command.type) {
        case CommandType::kStart:
            return StartConfiguredConnectivity();
        case CommandType::kStartWithCredentials:
        case CommandType::kProvisioned:
            return StartWithCredentials(command);
        case CommandType::kBeginProvisioning:
            if (state == wqn::services::ConnectivityState::kQuiescing) {
                return ESP_ERR_INVALID_STATE;
            }
            return BeginProvisioning();
        case CommandType::kWifiStarted: {
            if (state == wqn::services::ConnectivityState::kQuiescing ||
                state == wqn::services::ConnectivityState::kBackoff ||
                state == wqn::services::ConnectivityState::kProvisioning) {
                return ESP_OK;
            }
            if (!HoldConnectivityLease()) {
                return ESP_ERR_INVALID_STATE;
            }
            SetState(wqn::services::ConnectivityState::kConnecting);
            const esp_err_t result = wqn::ConnectWifiStationNow();
            ScheduleConnectRetry();
            return result;
        }
        case CommandType::kWifiConnected:
            if (state == wqn::services::ConnectivityState::kQuiescing) {
                return ESP_OK;
            }
            g_fast_retry_count = 0;
            g_next_action_us = 0;
            SetOnline(true, wqn::GetWifiRssi());
            SetState(wqn::services::ConnectivityState::kOnline);
            wqn::RequestOnlineSyncNow();
            return ESP_OK;
        case CommandType::kWifiDisconnected:
            SetOnline(false);
            if (state == wqn::services::ConnectivityState::kQuiescing ||
                state == wqn::services::ConnectivityState::kBackoff ||
                state == wqn::services::ConnectivityState::kProvisioning) {
                return ESP_OK;
            }
            ESP_LOGW(
                kTag,
                "WiFi disconnected: reason=%d rssi=%d",
                command.reason,
                command.rssi);
            if (!HoldConnectivityLease()) {
                return ESP_ERR_INVALID_STATE;
            }
            SetState(wqn::services::ConnectivityState::kConnecting);
            ScheduleConnectRetry();
            return ESP_OK;
        case CommandType::kPrepareSleep:
            return PrepareForSleep(command.sleep);
        case CommandType::kRollbackSleep:
            return RollbackAfterSleepAbort();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

void HandleScheduledAction()
{
    const wqn::services::ConnectivityState state =
        g_state.load(std::memory_order_acquire);
    if (state == wqn::services::ConnectivityState::kConnecting) {
        if (!HoldConnectivityLease()) {
            g_next_action_us = esp_timer_get_time() + 1000 * 1000;
            return;
        }
        if (++g_fast_retry_count >= kFastRetryLimit) {
            ScheduleBackoff();
            return;
        }
        ESP_LOGI(
            kTag,
            "retry WiFi connection: attempt=%d/%d",
            g_fast_retry_count,
            kFastRetryLimit);
        const esp_err_t result = wqn::ConnectWifiStationNow();
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "WiFi connect request failed: %s", esp_err_to_name(result));
        }
        ScheduleConnectRetry();
        return;
    }
    if (state == wqn::services::ConnectivityState::kBackoff) {
        if (!HoldConnectivityLease()) {
            g_next_action_us = esp_timer_get_time() + 1000 * 1000;
            return;
        }
        ESP_LOGI(kTag, "WiFi backoff complete; restarting radio");
        esp_err_t result = ESP_OK;
        if (wqn::IsWifiStationInitialized()) {
            result = wqn::StartWifiStationRadio();
        } else {
            result = wqn::StartWifiStationIfEnabled();
        }
        if (result == ESP_ERR_NOT_FOUND) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(BeginProvisioning());
            return;
        }
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "WiFi restart failed: %s", esp_err_to_name(result));
            g_next_action_us = esp_timer_get_time() + kBackoffDelayUs;
            return;
        }
        SetState(wqn::services::ConnectivityState::kConnecting);
        g_fast_retry_count = 0;
        ScheduleConnectRetry();
    }
}

TickType_t NextCommandWait()
{
    if (g_next_action_us <= 0) {
        return portMAX_DELAY;
    }
    const int64_t remaining_us = g_next_action_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    const uint64_t remaining_ms =
        static_cast<uint64_t>((remaining_us + 999) / 1000);
    const uint64_t ticks = pdMS_TO_TICKS(remaining_ms);
    return ticks == 0 ? 1 : static_cast<TickType_t>(ticks);
}

void ConnectivityTask(void*)
{
    wqn::SetWifiStationEventSink(WifiEventSink);
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    wqn::SetProvisionDoneCallback(ProvisionDone);
#endif
    ESP_LOGI(
        kTag,
        "connectivity service started: command_depth=%u",
        static_cast<unsigned>(kCommandQueueDepth));

    while (true) {
        ConnectivityCommand command;
        if (xQueueReceive(g_command_queue, &command, NextCommandWait()) == pdTRUE) {
            const esp_err_t result = HandleCommand(command);
            if (command.request_id != 0) {
                const ConnectivityReply reply = {command.request_id, result};
                if (xQueueSend(g_reply_queue, &reply, 0) != pdTRUE) {
                    ESP_LOGE(
                        kTag,
                        "reply queue full: request=%u",
                        static_cast<unsigned>(command.request_id));
                }
            }
            continue;
        }
        HandleScheduledAction();
    }
}

esp_err_t EnsureServiceStarted()
{
    taskENTER_CRITICAL(&g_start_lock);
    if (g_task != nullptr) {
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_OK;
    }
    if (g_starting) {
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_starting = true;
    if (g_command_queue == nullptr) {
        g_command_queue = xQueueCreateStatic(
            kCommandQueueDepth,
            sizeof(ConnectivityCommand),
            g_command_queue_buffer,
            &g_command_queue_storage);
    }
    if (g_reply_queue == nullptr) {
        g_reply_queue = xQueueCreateStatic(
            kReplyQueueDepth,
            sizeof(ConnectivityReply),
            g_reply_queue_buffer,
            &g_reply_queue_storage);
    }
    if (g_event_group == nullptr) {
        g_event_group = xEventGroupCreateStatic(&g_event_group_storage);
    }
    if (g_call_mutex == nullptr) {
        g_call_mutex = xSemaphoreCreateMutexStatic(&g_call_mutex_storage);
    }
    taskEXIT_CRITICAL(&g_start_lock);
    if (g_command_queue == nullptr || g_reply_queue == nullptr ||
        g_event_group == nullptr || g_call_mutex == nullptr) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t created_task = nullptr;
    if (xTaskCreate(
            ConnectivityTask,
            "connectivity",
            kTaskStackBytes,
            nullptr,
            kTaskPriority,
            &created_task) != pdPASS) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&g_start_lock);
    g_task = created_task;
    g_starting = false;
    taskEXIT_CRITICAL(&g_start_lock);
    return ESP_OK;
}

esp_err_t SubmitCommand(ConnectivityCommand command, TickType_t timeout)
{
    ESP_RETURN_ON_ERROR(EnsureServiceStarted(), kTag, "start service");
    const TickType_t started_at = xTaskGetTickCount();
    const auto remaining_timeout = [started_at, timeout]() -> TickType_t {
        if (timeout == portMAX_DELAY) {
            return portMAX_DELAY;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started_at;
        return elapsed >= timeout ? 0 : timeout - elapsed;
    };

    if (xSemaphoreTake(g_call_mutex, remaining_timeout()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ConnectivityReply stale;
    while (xQueueReceive(g_reply_queue, &stale, 0) == pdTRUE) {
        ESP_LOGW(
            kTag,
            "discard stale reply: request=%u",
            static_cast<unsigned>(stale.request_id));
    }

    command.request_id = g_next_request_id++;
    if (command.request_id == 0) {
        command.request_id = g_next_request_id++;
    }
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xQueueSend(g_command_queue, &command, remaining_timeout()) == pdTRUE) {
        ConnectivityReply reply;
        while (xQueueReceive(g_reply_queue, &reply, remaining_timeout()) == pdTRUE) {
            if (reply.request_id == command.request_id) {
                result = reply.result;
                break;
            }
        }
    }
    xSemaphoreGive(g_call_mutex);
    return result;
}

}  // namespace

namespace wqn::services {

esp_err_t StartConnectivity()
{
    ConnectivityCommand command;
    command.type = CommandType::kStart;
    return SubmitCommand(command, portMAX_DELAY);
}

esp_err_t StartConnectivityWithCredentials(const char* ssid, const char* password)
{
    if (ssid == nullptr || ssid[0] == '\0' || std::strlen(ssid) > kMaxSsidBytes ||
        (password != nullptr && std::strlen(password) > kMaxPasswordBytes)) {
        return ESP_ERR_INVALID_ARG;
    }
    ConnectivityCommand command;
    command.type = CommandType::kStartWithCredentials;
    CopyCredential(command.ssid, sizeof(command.ssid), ssid);
    CopyCredential(command.password, sizeof(command.password), password);
    return SubmitCommand(command, portMAX_DELAY);
}

esp_err_t WaitForConnectivity(TickType_t timeout)
{
    ESP_RETURN_ON_ERROR(StartConnectivity(), kTag, "request connectivity");
    const EventBits_t bits = xEventGroupWaitBits(
        g_event_group,
        kOnlineBit,
        pdFALSE,
        pdFALSE,
        timeout);
    return (bits & kOnlineBit) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool IsConnectivityOnline()
{
    return g_online.load(std::memory_order_acquire);
}

int GetConnectivityRssi()
{
    if (!g_online.load(std::memory_order_acquire)) {
        return 0;
    }
    const int current = wqn::GetWifiRssi();
    if (current != 0) {
        g_rssi.store(current, std::memory_order_release);
    }
    return g_rssi.load(std::memory_order_acquire);
}

void SetConnectivityProvisioning()
{
    ConnectivityCommand command;
    command.type = CommandType::kBeginProvisioning;
    ESP_ERROR_CHECK_WITHOUT_ABORT(SubmitCommand(command, portMAX_DELAY));
}

ConnectivitySnapshot GetConnectivitySnapshot()
{
    ConnectivitySnapshot snapshot;
    snapshot.state = g_state.load(std::memory_order_acquire);
    snapshot.online = g_online.load(std::memory_order_acquire);
    snapshot.rssi = snapshot.online ? GetConnectivityRssi() : 0;
    return snapshot;
}

esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand& command)
{
    ConnectivityCommand queued;
    queued.type = CommandType::kPrepareSleep;
    queued.sleep = command;
    TickType_t timeout = portMAX_DELAY;
    if (command.deadline_us > 0) {
        const int64_t remaining_us = command.deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        timeout = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (timeout == 0) {
            timeout = 1;
        }
    }
    return SubmitCommand(queued, timeout);
}

void RollbackConnectivityAfterSleepAbort()
{
    ConnectivityCommand command;
    command.type = CommandType::kRollbackSleep;
    const esp_err_t result = SubmitCommand(command, pdMS_TO_TICKS(5000));
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "connectivity rollback failed: %s", esp_err_to_name(result));
    }
}

}  // namespace wqn::services
