#include "services/connectivity_service.h"

#include <algorithm>
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
#include "services/sync_service.h"
#include "provision_manager.h"
#include "runtime/sleep_coordinator.h"
#include "storage.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "connectivity";
constexpr UBaseType_t kCommandQueueDepth = 12;
constexpr UBaseType_t kReplyQueueDepth = 8;
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 5;
constexpr int64_t kAssociationTimeoutUs = 15LL * 1000 * 1000;
constexpr int64_t kDhcpTimeoutUs = 15LL * 1000 * 1000;
constexpr int64_t kRetryBaseDelayUs = 5LL * 1000 * 1000;
constexpr int64_t kBackoffDelayUs = 60LL * 1000 * 1000;
// [wifi-redundancy] Count complete failed connection attempts, not timer ticks.
// Credential failures pivot immediately; transient failures get bounded 5 s,
// 10 s retry delays before the third failure exhausts the slot.
constexpr uint8_t kPerSlotAttemptLimit = 3;
constexpr size_t kMaxSsidBytes = 32;
// [hang-fix] Bound for callers that submit commands from the UI or sync
// tasks. If ConnectivityTask wedges inside the Wi-Fi driver or LwIP, the
// submitting thread must get ESP_ERR_TIMEOUT back instead of hanging
// forever on g_call_mutex / the reply queue. 30 s matches the STA connect
// budget (kWifiConnectTimeout in wqn_api.cpp).
constexpr TickType_t kSubmitCommandTimeout = pdMS_TO_TICKS(30000);
constexpr size_t kMaxPasswordBytes = 64;
constexpr EventBits_t kOnlineBit = BIT0;

enum class CommandType : uint8_t {
    kStart,
    kStartWithCredentials,
    kBeginProvisioning,
    kWifiStarted,
    kWifiAssociated,
    kWifiGotIp,
    kWifiDisconnected,
    kProvisioned,
    kPrepareSleep,
    kRollbackSleep,
};

enum class ScheduledAction : uint8_t {
    kNone,
    kAssociationTimeout,
    kDhcpTimeout,
    kRetrySlot,
    kBackoffExpired,
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

int64_t g_next_action_us = 0;
ScheduledAction g_scheduled_action = ScheduledAction::kNone;
bool g_attempt_active = false;
bool g_resume_provisioning = false;
wqn::runtime::SleepLease g_connectivity_lease;

// [wifi-redundancy] Dual-slot credential state, owned by ConnectivityTask.
wqn::WifiCredentialStore g_cred_store{};
uint8_t g_active_slot = 0;
uint8_t g_slot_fail_count[2] = {0, 0};
// [wifi-redundancy] Snapshot identity read by GetConnectivitySnapshot from any
// task. Written by ConnectivityTask under g_snapshot_lock; copies are short and
// under the spinlock so a cross-core reader never sees a torn string.
portMUX_TYPE g_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
char g_snapshot_active_ssid[33] = {};
char g_snapshot_backup_ssid[33] = {};
bool g_snapshot_has_backup = false;

const char* StateName(wqn::services::ConnectivityState state)
{
    switch (state) {
        case wqn::services::ConnectivityState::kOff:
            return "off";
        case wqn::services::ConnectivityState::kProvisioning:
            return "provisioning";
        case wqn::services::ConnectivityState::kConnecting:
            return "connecting";
        case wqn::services::ConnectivityState::kWaitingIp:
            return "waiting-ip";
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
        state != wqn::services::ConnectivityState::kWaitingIp &&
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

void ClearScheduledAction();
void ScheduleAction(ScheduledAction action, int64_t delay_us);
esp_err_t BeginConnectionAttempt();
void ScheduleBackoff();

// [wifi-redundancy] Publishes the active/backup SSID identity for snapshots.
// Runs on ConnectivityTask; the copy is short and under a spinlock so a reader
// on the other core (UI snapshot) never sees a torn string.
void PublishSnapshotIdentity()
{
    taskENTER_CRITICAL(&g_snapshot_lock);
    if (g_cred_store.count > 0 && g_active_slot < g_cred_store.count) {
        CopyCredential(
            g_snapshot_active_ssid,
            sizeof(g_snapshot_active_ssid),
            g_cred_store.slots[g_active_slot].ssid);
    } else {
        g_snapshot_active_ssid[0] = '\0';
    }
    if (g_cred_store.count >= 2) {
        CopyCredential(
            g_snapshot_backup_ssid,
            sizeof(g_snapshot_backup_ssid),
            g_cred_store.slots[1 - g_active_slot].ssid);
        g_snapshot_has_backup = true;
    } else {
        g_snapshot_backup_ssid[0] = '\0';
        g_snapshot_has_backup = false;
    }
    taskEXIT_CRITICAL(&g_snapshot_lock);
}

// [wifi-redundancy] Reloads the credential store from NVS into task state.
void ReloadCredStoreFromNvs()
{
    const esp_err_t result = wqn::LoadWifiCredentialStore(&g_cred_store);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "load wifi credential store failed: %s", esp_err_to_name(result));
        g_cred_store = wqn::WifiCredentialStore{};
    }
}

// [wifi-redundancy] Starts a fresh connect cycle on the preferred slot and
// resets the per-slot failure budget. The store must already be loaded via
// ReloadCredStoreFromNvs. Returns ESP_ERR_NOT_FOUND when nothing is stored.
esp_err_t KickConnectFromStore()
{
    if (g_cred_store.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    g_active_slot = g_cred_store.preferred;
    g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
    PublishSnapshotIdentity();
    const wqn::WifiCredentialSlot& slot = g_cred_store.slots[g_active_slot];
    return wqn::StartWifiWithCredentials(slot.ssid, slot.password);
}

// [wifi-redundancy] Switches to the backup credential after the active slot
// exhausted its budget or hit a credential-level failure. Returns true when a
// usable backup slot was armed and its connect kicked off.
bool TryPivotToBackupSlot()
{
    if (g_cred_store.count < 2) {
        return false;
    }
    const uint8_t backup = 1 - g_active_slot;
    const wqn::WifiCredentialSlot& slot = g_cred_store.slots[backup];
    if (slot.ssid[0] == '\0' || g_slot_fail_count[backup] >= kPerSlotAttemptLimit) {
        return false;
    }
    // A slot pivot is a connectivity transition even when invoked outside the
    // normal disconnect-event path. Publish offline before changing identity
    // so readers never treat the old link as usable with the new slot.
    SetOnline(false);
    g_active_slot = backup;
    ESP_LOGI(kTag, "pivot to backup WiFi slot %u (SSID=%s)", static_cast<unsigned>(backup), slot.ssid);
    PublishSnapshotIdentity();
    const esp_err_t result = wqn::StartWifiWithCredentials(slot.ssid, slot.password);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pivot connect failed: %s", esp_err_to_name(result));
        g_slot_fail_count[backup] = kPerSlotAttemptLimit;
        return false;
    }
    SetState(wqn::services::ConnectivityState::kConnecting);
    const esp_err_t connect_result = BeginConnectionAttempt();
    if (connect_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "backup WiFi connect request failed: %s",
            esp_err_to_name(connect_result));
        g_slot_fail_count[backup] = kPerSlotAttemptLimit;
        return false;
    }
    return true;
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
        case wqn::WifiStationEvent::kAssociated:
            command.type = CommandType::kWifiAssociated;
            break;
        case wqn::WifiStationEvent::kGotIp:
            command.type = CommandType::kWifiGotIp;
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

void ClearScheduledAction()
{
    g_scheduled_action = ScheduledAction::kNone;
    g_next_action_us = 0;
}

void ScheduleAction(ScheduledAction action, int64_t delay_us)
{
    g_scheduled_action = action;
    g_next_action_us = esp_timer_get_time() + delay_us;
}

int64_t RetryDelayUs(uint8_t failed_attempts)
{
    if (failed_attempts <= 1) {
        return kRetryBaseDelayUs;
    }
    const uint8_t shift = std::min<uint8_t>(failed_attempts - 1, 3);
    return std::min<int64_t>(kRetryBaseDelayUs << shift, kBackoffDelayUs);
}

esp_err_t BeginConnectionAttempt()
{
    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }
    SetOnline(false);
    SetState(wqn::services::ConnectivityState::kConnecting);
    ClearScheduledAction();
    const esp_err_t result = wqn::ConnectWifiStationNow();
    if (result != ESP_OK) {
        g_attempt_active = false;
        return result;
    }
    g_attempt_active = true;
    ScheduleAction(ScheduledAction::kAssociationTimeout, kAssociationTimeoutUs);
    ESP_LOGI(
        kTag,
        "WiFi connection attempt started: slot=%u attempt=%u/%u",
        static_cast<unsigned>(g_active_slot),
        static_cast<unsigned>(g_slot_fail_count[g_active_slot] + 1),
        static_cast<unsigned>(kPerSlotAttemptLimit));
    return ESP_OK;
}

void ScheduleBackoff()
{
    SetOnline(false);
    g_attempt_active = false;
    SetState(wqn::services::ConnectivityState::kBackoff);
    // [wifi-redundancy] Fresh per-slot budget after the radio-off pause; the
    // next cycle starts clean from the preferred slot.
    g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
    ScheduleAction(ScheduledAction::kBackoffExpired, kBackoffDelayUs);
    const esp_err_t stop_result = wqn::StopWifiStationRadio();
    if (stop_result != ESP_OK) {
        ESP_LOGW(kTag, "stop WiFi for backoff failed: %s", esp_err_to_name(stop_result));
    }
    ESP_LOGW(kTag, "WiFi retries exhausted; radio off for 60 seconds");
}

void RecordAttemptFailure(const char* cause, bool credential_failure)
{
    g_attempt_active = false;
    SetOnline(false);
    uint8_t& failures = g_slot_fail_count[g_active_slot];
    if (credential_failure) {
        failures = kPerSlotAttemptLimit;
    } else if (failures < kPerSlotAttemptLimit) {
        ++failures;
    }
    ESP_LOGW(
        kTag,
        "WiFi attempt failed: cause=%s slot=%u failures=%u/%u",
        cause,
        static_cast<unsigned>(g_active_slot),
        static_cast<unsigned>(failures),
        static_cast<unsigned>(kPerSlotAttemptLimit));
    if (failures >= kPerSlotAttemptLimit) {
        if (TryPivotToBackupSlot()) {
            return;
        }
        ScheduleBackoff();
        return;
    }
    SetState(wqn::services::ConnectivityState::kConnecting);
    const int64_t retry_delay_us = RetryDelayUs(failures);
    ScheduleAction(ScheduledAction::kRetrySlot, retry_delay_us);
    ESP_LOGI(
        kTag,
        "WiFi retry scheduled: slot=%u delay_ms=%lld",
        static_cast<unsigned>(g_active_slot),
        static_cast<long long>(retry_delay_us / 1000));
}

esp_err_t BeginProvisioning()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }
    SetOnline(false);
    g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
    g_attempt_active = false;
    ClearScheduledAction();
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
    const wqn::services::ConnectivityState state =
        g_state.load(std::memory_order_acquire);
    switch (state) {
        case wqn::services::ConnectivityState::kQuiescing:
            return ESP_ERR_INVALID_STATE;
        case wqn::services::ConnectivityState::kProvisioning:
        case wqn::services::ConnectivityState::kConnecting:
        case wqn::services::ConnectivityState::kWaitingIp:
        case wqn::services::ConnectivityState::kOnline:
        case wqn::services::ConnectivityState::kBackoff:
            // StartConnectivity is an idempotent readiness request. In
            // particular, it must not tear down an in-flight association or
            // bypass the radio-off backoff selected by this owner task.
            return ESP_OK;
        case wqn::services::ConnectivityState::kOff:
            break;
    }
    if (wqn::IsProvisioningActive()) {
        SetState(wqn::services::ConnectivityState::kProvisioning);
        return ESP_OK;
    }

    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }

    // [wifi-redundancy] Stored credentials first (preferred slot), then the
    // compile-time developer fallback, then provisioning.
    ReloadCredStoreFromNvs();
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (g_cred_store.count > 0) {
        result = KickConnectFromStore();
    } else if (wqn::IsWifiStationInitialized()) {
        result = wqn::StartWifiStationRadio();
    } else {
        result = wqn::StartWifiStationIfEnabled();
    }
    if (result == ESP_ERR_NOT_FOUND) {
        return BeginProvisioning();
    }
    if (result != ESP_OK) {
        ScheduleBackoff();
        return result;
    }
    result = BeginConnectionAttempt();
    if (result != ESP_OK) {
        RecordAttemptFailure("connect-request", false);
    }
    return result;
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
    g_attempt_active = false;
    ClearScheduledAction();

    // [wifi-redundancy] Persist first (dedup by SSID, the new credential becomes
    // preferred), then connect from the refreshed store. If the upsert fails we
    // still connect the raw credentials so the user gets online.
    const esp_err_t upsert_result = wqn::UpsertWifiCredential(command.ssid, command.password);
    if (upsert_result != ESP_OK) {
        ESP_LOGW(kTag, "upsert wifi credential failed: %s", esp_err_to_name(upsert_result));
    }
    ReloadCredStoreFromNvs();
    esp_err_t result = ESP_OK;
    const bool stored_new_credential =
        upsert_result == ESP_OK && g_cred_store.count > 0 &&
        g_cred_store.preferred < g_cred_store.count &&
        std::strcmp(
            g_cred_store.slots[g_cred_store.preferred].ssid,
            command.ssid) == 0;
    if (stored_new_credential) {
        result = KickConnectFromStore();
    } else {
        g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
        result = wqn::StartWifiWithCredentials(command.ssid, command.password);
        if (result == ESP_OK) {
            taskENTER_CRITICAL(&g_snapshot_lock);
            CopyCredential(g_snapshot_active_ssid, sizeof(g_snapshot_active_ssid), command.ssid);
            g_snapshot_backup_ssid[0] = '\0';
            g_snapshot_has_backup = false;
            taskEXIT_CRITICAL(&g_snapshot_lock);
        }
    }
    if (result != ESP_OK) {
        ScheduleBackoff();
        return result;
    }
    result = BeginConnectionAttempt();
    if (result != ESP_OK) {
        RecordAttemptFailure("credential-connect-request", false);
    }
    return result;
#endif
}

esp_err_t PrepareForSleep(const wqn::power::PrepareSleepCommand& command)
{
    if (command.deadline_us > 0 && esp_timer_get_time() >= command.deadline_us) {
        return ESP_ERR_TIMEOUT;
    }
    g_resume_provisioning = wqn::IsProvisioningActive();
    g_attempt_active = false;
    ClearScheduledAction();
    SetState(wqn::services::ConnectivityState::kQuiescing);

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
        g_attempt_active = false;
        ClearScheduledAction();
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
        g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
        const esp_err_t result = BeginConnectionAttempt();
        if (result != ESP_OK) {
            RecordAttemptFailure("sleep-rollback-connect", false);
        }
        return result;
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
                state == wqn::services::ConnectivityState::kProvisioning ||
                state == wqn::services::ConnectivityState::kWaitingIp ||
                state == wqn::services::ConnectivityState::kOnline ||
                g_attempt_active) {
                return ESP_OK;
            }
            const esp_err_t result = BeginConnectionAttempt();
            if (result != ESP_OK) {
                RecordAttemptFailure("station-start-connect", false);
            }
            return result;
        }
        case CommandType::kWifiAssociated:
            if (state == wqn::services::ConnectivityState::kQuiescing ||
                state == wqn::services::ConnectivityState::kBackoff ||
                state == wqn::services::ConnectivityState::kProvisioning ||
                state == wqn::services::ConnectivityState::kOnline) {
                return ESP_OK;
            }
            if (!HoldConnectivityLease()) {
                return ESP_ERR_INVALID_STATE;
            }
            g_attempt_active = true;
            SetState(wqn::services::ConnectivityState::kWaitingIp);
            ScheduleAction(ScheduledAction::kDhcpTimeout, kDhcpTimeoutUs);
            ESP_LOGI(
                kTag,
                "WiFi associated; waiting up to %lld ms for DHCP",
                static_cast<long long>(kDhcpTimeoutUs / 1000));
            return ESP_OK;
        case CommandType::kWifiGotIp:
            if (state == wqn::services::ConnectivityState::kQuiescing ||
                state == wqn::services::ConnectivityState::kBackoff) {
                return ESP_OK;
            }
            g_attempt_active = false;
            g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
            ClearScheduledAction();
            SetOnline(true, wqn::GetWifiRssi());
            SetState(wqn::services::ConnectivityState::kOnline);
            // [wifi-redundancy] The slot that just connected becomes the next
            // cycle's first try; the NVS write is skipped when unchanged.
            if (g_cred_store.count > 0 && g_active_slot < g_cred_store.count) {
                const esp_err_t preferred_result =
                    wqn::MarkWifiSlotPreferred(g_active_slot);
                if (preferred_result != ESP_OK) {
                    ESP_LOGW(
                        kTag,
                        "mark WiFi slot preferred failed: %s",
                        esp_err_to_name(preferred_result));
                }
            }
            // Connectivity is readiness, not a synchronization intent. The
            // sync scheduler wakes and runs only if a manual/periodic/retry/
            // outbox reason is already pending; reconnecting by itself must
            // not turn every 60-second RTC wake into a full sync.
            wqn::services::NotifySyncConnectivityAvailable();
            return ESP_OK;
        case CommandType::kWifiDisconnected:
            SetOnline(false);
            if (state == wqn::services::ConnectivityState::kQuiescing ||
                state == wqn::services::ConnectivityState::kBackoff ||
                state == wqn::services::ConnectivityState::kProvisioning ||
                state == wqn::services::ConnectivityState::kOff) {
                return ESP_OK;
            }
            ESP_LOGW(
                kTag,
                "WiFi disconnected: reason=%d rssi=%d slot=%u",
                command.reason,
                command.rssi,
                static_cast<unsigned>(g_active_slot));
            if (!HoldConnectivityLease()) {
                return ESP_ERR_INVALID_STATE;
            }
            if (!g_attempt_active &&
                g_scheduled_action == ScheduledAction::kRetrySlot) {
                ESP_LOGI(kTag, "ignore disconnect from completed/aborted attempt");
                return ESP_OK;
            }
            RecordAttemptFailure(
                "disconnect",
                wqn::IsWifiCredentialFailureReason(command.reason));
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
    const ScheduledAction action = g_scheduled_action;
    if (action == ScheduledAction::kNone) {
        ClearScheduledAction();
        return;
    }
    const bool action_matches_state =
        (action == ScheduledAction::kAssociationTimeout &&
         state == wqn::services::ConnectivityState::kConnecting &&
         g_attempt_active) ||
        (action == ScheduledAction::kDhcpTimeout &&
         state == wqn::services::ConnectivityState::kWaitingIp &&
         g_attempt_active) ||
        (action == ScheduledAction::kRetrySlot &&
         state == wqn::services::ConnectivityState::kConnecting &&
         !g_attempt_active) ||
        (action == ScheduledAction::kBackoffExpired &&
         state == wqn::services::ConnectivityState::kBackoff);
    if (!action_matches_state) {
        ClearScheduledAction();
        return;
    }
    if (!HoldConnectivityLease()) {
        ScheduleAction(action, 1000 * 1000);
        return;
    }

    switch (action) {
        case ScheduledAction::kAssociationTimeout:
            if (state != wqn::services::ConnectivityState::kConnecting ||
                !g_attempt_active) {
                ClearScheduledAction();
                return;
            }
            ClearScheduledAction();
            g_attempt_active = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::DisconnectWifiStationNow());
            RecordAttemptFailure("association-timeout", false);
            return;
        case ScheduledAction::kDhcpTimeout:
            if (state != wqn::services::ConnectivityState::kWaitingIp ||
                !g_attempt_active) {
                ClearScheduledAction();
                return;
            }
            ClearScheduledAction();
            g_attempt_active = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::DisconnectWifiStationNow());
            RecordAttemptFailure("dhcp-timeout", false);
            return;
        case ScheduledAction::kRetrySlot: {
            if (state != wqn::services::ConnectivityState::kConnecting ||
                g_attempt_active) {
                ClearScheduledAction();
                return;
            }
            ClearScheduledAction();
            const esp_err_t result = BeginConnectionAttempt();
            if (result != ESP_OK) {
                RecordAttemptFailure("retry-connect-request", false);
            }
            return;
        }
        case ScheduledAction::kBackoffExpired: {
            if (state != wqn::services::ConnectivityState::kBackoff) {
                ClearScheduledAction();
                return;
            }
            ClearScheduledAction();
            ESP_LOGI(kTag, "WiFi backoff complete; restarting radio");
            // Reload the store because provisioning may have written new
            // credentials while the radio was stopped.
            ReloadCredStoreFromNvs();
            esp_err_t result = ESP_ERR_NOT_FOUND;
            if (g_cred_store.count > 0) {
                result = KickConnectFromStore();
            } else if (wqn::IsWifiStationInitialized()) {
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
                ScheduleBackoff();
                return;
            }
            result = BeginConnectionAttempt();
            if (result != ESP_OK) {
                RecordAttemptFailure("backoff-connect-request", false);
            }
            return;
        }
        case ScheduledAction::kNone:
            return;
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
    return SubmitCommand(command, kSubmitCommandTimeout);
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
    return SubmitCommand(command, kSubmitCommandTimeout);
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(SubmitCommand(command, kSubmitCommandTimeout));
}

ConnectivitySnapshot GetConnectivitySnapshot()
{
    ConnectivitySnapshot snapshot;
    snapshot.state = g_state.load(std::memory_order_acquire);
    snapshot.online = g_online.load(std::memory_order_acquire);
    snapshot.rssi = snapshot.online ? GetConnectivityRssi() : 0;
    // [wifi-redundancy] Copy the identity under the same spinlock the writer
    // (ConnectivityTask) uses, so the SSID strings are never torn.
    taskENTER_CRITICAL(&g_snapshot_lock);
    CopyCredential(snapshot.active_ssid, sizeof(snapshot.active_ssid), g_snapshot_active_ssid);
    CopyCredential(snapshot.backup_ssid, sizeof(snapshot.backup_ssid), g_snapshot_backup_ssid);
    snapshot.has_backup = g_snapshot_has_backup;
    taskEXIT_CRITICAL(&g_snapshot_lock);
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
