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
constexpr int64_t kOnlineIdleTailUs = 15LL * 1000 * 1000;
// [power-fix] Radio-off backoff ladder for consecutive EXHAUSTED retry
// rounds: 60s -> 5min -> 15min cap. RTC retention keeps the round count
// alive across deep-sleep cycles, otherwise every boot would restart the
// storm from rung 0 against a dead AP. After kBackoffSuspendAfterRounds
// failed rounds the automatic retry is suspended entirely: the radio stays
// off until an explicit trigger (boot admission with a due sync deadline,
// a user action, or new credentials) resumes it.
constexpr int64_t kBackoffLadderUs[] = {
    60LL * 1000 * 1000,
    300LL * 1000 * 1000,
    900LL * 1000 * 1000};
constexpr size_t kBackoffLadderSize =
    sizeof(kBackoffLadderUs) / sizeof(kBackoffLadderUs[0]);
constexpr uint8_t kBackoffSuspendAfterRounds = 6;
RTC_DATA_ATTR uint8_t g_backoff_rounds = 0;
RTC_DATA_ATTR bool g_backoff_suspended = false;
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
constexpr EventBits_t kStateChangedBit = BIT1;
constexpr size_t kDemandSlotCount = 16;
constexpr TickType_t kServiceStartWait = pdMS_TO_TICKS(2000);

enum class CommandType : uint8_t {
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
    kOnlineIdleTailExpired,
};

struct ConnectivityCommand {
    CommandType type = CommandType::kStartWithCredentials;
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
StaticSemaphore_t g_demand_changed_storage;
SemaphoreHandle_t g_demand_changed = nullptr;
QueueSetHandle_t g_work_set = nullptr;

TaskHandle_t g_task = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;
uint32_t g_next_request_id = 1;

std::atomic<wqn::services::ConnectivityState> g_state{
    wqn::services::ConnectivityState::kOff};
std::atomic<bool> g_online{false};
std::atomic<int> g_rssi{0};
std::atomic<uint8_t> g_demand_count{0};
std::atomic<uint32_t> g_demand_mask{0};
std::atomic<wqn::services::ConnectivityDemandPriority> g_demand_priority{
    wqn::services::ConnectivityDemandPriority::kNone};
std::atomic<uint32_t> g_latest_demand_generation{0};
std::atomic<uint8_t> g_backoff_rounds_view{0};
std::atomic<bool> g_backoff_suspended_view{false};
std::atomic<wqn::services::ConnectivityWaitResult> g_last_failure{
    wqn::services::ConnectivityWaitResult::kCancelled};
std::atomic<uint32_t> g_last_failure_generation{0};

struct DemandSlot {
    bool active = false;
    uint32_t id = 0;
    uint32_t activation_generation = 0;
    wqn::services::ConnectivityDemandReason reason =
        wqn::services::ConnectivityDemandReason::kSyncBackground;
    const char* owner = nullptr;
    const char* file = nullptr;
    int line = 0;
};

struct DemandPolicySnapshot {
    uint8_t count = 0;
    uint32_t mask = 0;
    wqn::services::ConnectivityDemandPriority priority =
        wqn::services::ConnectivityDemandPriority::kNone;
    uint32_t latest_generation = 0;
    uint32_t latest_interactive_generation = 0;
};

portMUX_TYPE g_demand_lock = portMUX_INITIALIZER_UNLOCKED;
DemandSlot g_demand_slots[kDemandSlotCount] = {};
uint32_t g_next_demand_id = 1;
uint32_t g_policy_generation = 0;

int64_t g_next_action_us = 0;
ScheduledAction g_scheduled_action = ScheduledAction::kNone;
bool g_attempt_active = false;
bool g_resume_provisioning = false;
uint32_t g_last_interactive_bypass_generation = 0;
DemandPolicySnapshot g_observed_demand_policy{};
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
        case wqn::services::ConnectivityState::kOnlineIdleTail:
            return "online-idle-tail";
        case wqn::services::ConnectivityState::kBackoff:
            return "backoff";
        case wqn::services::ConnectivityState::kQuiescing:
            return "quiescing";
        default:
            return "unknown";
    }
}

const char* DemandReasonName(wqn::services::ConnectivityDemandReason reason)
{
    switch (reason) {
        case wqn::services::ConnectivityDemandReason::kAiInteractive:
            return "ai-interactive";
        case wqn::services::ConnectivityDemandReason::kCloudInteractive:
            return "cloud-interactive";
        case wqn::services::ConnectivityDemandReason::kSyncInteractive:
            return "sync-interactive";
        case wqn::services::ConnectivityDemandReason::kSyncBackground:
            return "sync-background";
        case wqn::services::ConnectivityDemandReason::kBulkBackground:
            return "bulk-background";
        default:
            return "unknown";
    }
}

wqn::services::ConnectivityDemandPriority PriorityForReason(
    wqn::services::ConnectivityDemandReason reason)
{
    switch (reason) {
        case wqn::services::ConnectivityDemandReason::kAiInteractive:
        case wqn::services::ConnectivityDemandReason::kCloudInteractive:
        case wqn::services::ConnectivityDemandReason::kSyncInteractive:
            return wqn::services::ConnectivityDemandPriority::kInteractive;
        case wqn::services::ConnectivityDemandReason::kSyncBackground:
        case wqn::services::ConnectivityDemandReason::kBulkBackground:
            return wqn::services::ConnectivityDemandPriority::kBackground;
        default:
            return wqn::services::ConnectivityDemandPriority::kNone;
    }
}

bool GenerationAfter(uint32_t candidate, uint32_t baseline)
{
    return static_cast<int32_t>(candidate - baseline) > 0;
}

uint32_t NextPolicyGenerationLocked()
{
    ++g_policy_generation;
    if (g_policy_generation == 0) {
        ++g_policy_generation;
    }
    return g_policy_generation;
}

DemandPolicySnapshot SnapshotDemandPolicyLocked()
{
    DemandPolicySnapshot snapshot;
    for (const DemandSlot& slot : g_demand_slots) {
        if (!slot.active) {
            continue;
        }
        if (snapshot.count < UINT8_MAX) {
            ++snapshot.count;
        }
        snapshot.mask |= 1UL << static_cast<uint8_t>(slot.reason);
        snapshot.priority = std::max(
            snapshot.priority,
            PriorityForReason(slot.reason));
        if (snapshot.latest_generation == 0 ||
            GenerationAfter(slot.activation_generation, snapshot.latest_generation)) {
            snapshot.latest_generation = slot.activation_generation;
        }
        if (PriorityForReason(slot.reason) ==
                wqn::services::ConnectivityDemandPriority::kInteractive &&
            (snapshot.latest_interactive_generation == 0 ||
             GenerationAfter(
                 slot.activation_generation,
                 snapshot.latest_interactive_generation))) {
            snapshot.latest_interactive_generation = slot.activation_generation;
        }
    }
    return snapshot;
}

DemandPolicySnapshot SnapshotDemandPolicy()
{
    taskENTER_CRITICAL(&g_demand_lock);
    const DemandPolicySnapshot snapshot = SnapshotDemandPolicyLocked();
    taskEXIT_CRITICAL(&g_demand_lock);
    return snapshot;
}

void PublishDemandPolicyLocked(const DemandPolicySnapshot& snapshot)
{
    // Publish payload before count: a reader that observes the new count sees
    // the matching mask, priority and generation (§4.7).
    g_demand_mask.store(snapshot.mask, std::memory_order_relaxed);
    g_demand_priority.store(snapshot.priority, std::memory_order_relaxed);
    g_latest_demand_generation.store(
        snapshot.latest_generation, std::memory_order_relaxed);
    g_demand_count.store(snapshot.count, std::memory_order_release);
}

void NotifyDemandChanged()
{
    if (g_demand_changed != nullptr) {
        xSemaphoreGive(g_demand_changed);
    }
}

bool IsDemandActive(uint32_t id)
{
    if (id == 0) {
        return false;
    }
    bool active = false;
    taskENTER_CRITICAL(&g_demand_lock);
    for (const DemandSlot& slot : g_demand_slots) {
        if (slot.active && slot.id == id) {
            active = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&g_demand_lock);
    return active;
}

void PublishStateChanged()
{
    if (g_event_group != nullptr) {
        xEventGroupSetBits(g_event_group, kStateChangedBit);
    }
}

void ClearFailure()
{
    g_last_failure.store(
        wqn::services::ConnectivityWaitResult::kCancelled,
        std::memory_order_relaxed);
    g_last_failure_generation.store(0, std::memory_order_release);
}

void PublishFailure(wqn::services::ConnectivityWaitResult result)
{
    uint32_t generation = 0;
    taskENTER_CRITICAL(&g_demand_lock);
    generation = NextPolicyGenerationLocked();
    taskEXIT_CRITICAL(&g_demand_lock);
    g_last_failure.store(result, std::memory_order_relaxed);
    g_last_failure_generation.store(generation, std::memory_order_release);
    PublishStateChanged();
    ESP_LOGW(
        kTag,
        "connectivity failure: result=%s generation=%u",
        wqn::services::ConnectivityWaitResultName(result),
        static_cast<unsigned>(generation));
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
        PublishStateChanged();
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
    const bool previous = g_online.exchange(online, std::memory_order_acq_rel);
    g_rssi.store(online ? rssi : 0, std::memory_order_release);
    if (g_event_group != nullptr) {
        if (online) {
            xEventGroupSetBits(g_event_group, kOnlineBit);
        } else {
            xEventGroupClearBits(g_event_group, kOnlineBit);
        }
    }
    if (previous != online) {
        PublishStateChanged();
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
void ScheduleBackoff(
    wqn::services::ConnectivityWaitResult failure =
        wqn::services::ConnectivityWaitResult::kUnavailable);
esp_err_t EnsureServiceStarted();
void ReleaseDemand(uint32_t id);

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
    ClearFailure();
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

void ScheduleBackoff(wqn::services::ConnectivityWaitResult failure)
{
    PublishFailure(failure);
    SetOnline(false);
    g_attempt_active = false;
    SetState(wqn::services::ConnectivityState::kBackoff);
    // [wifi-redundancy] Fresh per-slot budget after the radio-off pause; the
    // next cycle starts clean from the preferred slot.
    g_slot_fail_count[0] = g_slot_fail_count[1] = 0;
    if (g_backoff_rounds < UINT8_MAX) {
        ++g_backoff_rounds;
    }
    if (!g_backoff_suspended &&
        g_backoff_rounds > kBackoffSuspendAfterRounds) {
        g_backoff_suspended = true;
    }
    g_backoff_rounds_view.store(g_backoff_rounds, std::memory_order_release);
    g_backoff_suspended_view.store(g_backoff_suspended, std::memory_order_release);
    const esp_err_t stop_result = wqn::StopWifiStationRadio();
    if (stop_result != ESP_OK) {
        ESP_LOGW(kTag, "stop WiFi for backoff failed: %s", esp_err_to_name(stop_result));
    }
    if (g_backoff_suspended) {
        // No timer: only an explicit trigger (due sync deadline at boot
        // admission, user action, new credentials) restarts the radio. This
        // is what keeps a dead AP from lighting the radio once per interval
        // forever on a battery-powered device.
        ESP_LOGW(
            kTag,
            "WiFi auto-retry suspended after %u exhausted rounds; radio off until explicit trigger",
            static_cast<unsigned>(g_backoff_rounds));
        return;
    }
    const uint8_t ladder_index = static_cast<uint8_t>(
        std::min<uint8_t>(static_cast<uint8_t>(g_backoff_rounds - 1),
                          static_cast<uint8_t>(kBackoffLadderSize - 1)));
    const int64_t backoff_us = kBackoffLadderUs[ladder_index];
    ScheduleAction(ScheduledAction::kBackoffExpired, backoff_us);
    ESP_LOGW(
        kTag,
        "WiFi retries exhausted; radio off for %lld s (round %u)",
        static_cast<long long>(backoff_us / (1000 * 1000)),
        static_cast<unsigned>(g_backoff_rounds));
}

void ResetBackoffEscalation()
{
    g_backoff_rounds = 0;
    g_backoff_suspended = false;
    g_backoff_rounds_view.store(0, std::memory_order_release);
    g_backoff_suspended_view.store(false, std::memory_order_release);
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
        ScheduleBackoff(
            credential_failure
                ? wqn::services::ConnectivityWaitResult::kAuthFailed
                : wqn::services::ConnectivityWaitResult::kUnavailable);
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

esp_err_t StopRadioAndSetOff(const char* reason)
{
    g_attempt_active = false;
    ClearScheduledAction();
    SetOnline(false);
    const esp_err_t result = wqn::StopWifiStationRadio();
    if (result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "stop WiFi failed: reason=%s error=%s",
            reason,
            esp_err_to_name(result));
        return result;
    }
    SetState(wqn::services::ConnectivityState::kOff);
    ESP_LOGI(
        kTag,
        "WiFi radio off: reason=%s cumulative_radio_ms=%lu",
        reason,
        static_cast<unsigned long>(wqn::GetWifiRadioOnTotalMs()));
    return ESP_OK;
}

esp_err_t StartConfiguredConnectivity(bool force_backoff = false)
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
            return ESP_OK;
        case wqn::services::ConnectivityState::kOnlineIdleTail:
            ClearScheduledAction();
            SetState(wqn::services::ConnectivityState::kOnline);
            return ESP_OK;
        case wqn::services::ConnectivityState::kBackoff:
            if (!force_backoff) {
                return ESP_OK;
            }
            g_backoff_suspended = false;
            g_backoff_suspended_view.store(false, std::memory_order_release);
            ClearScheduledAction();
            break;
        case wqn::services::ConnectivityState::kOff:
            break;
    }
    if (wqn::IsProvisioningActive()) {
        SetState(wqn::services::ConnectivityState::kProvisioning);
        return ESP_OK;
    }

    if (g_demand_count.load(std::memory_order_acquire) == 0) {
        ESP_LOGE(kTag, "connectivity start rejected without a demand owner");
        return ESP_ERR_INVALID_STATE;
    }

    if (!HoldConnectivityLease()) {
        return ESP_ERR_INVALID_STATE;
    }

    // [wifi-demand] A normal request never silently opens the provisioning
    // portal. Missing credentials are a typed terminal result; only the
    // explicit settings/provisioning flow may start SoftAP mode.
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
        PublishFailure(wqn::services::ConnectivityWaitResult::kNeedsProvisioning);
        SetState(wqn::services::ConnectivityState::kOff);
        return result;
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

void HandleDemandPolicyChanged()
{
    const DemandPolicySnapshot current = SnapshotDemandPolicy();
    const bool changed =
        current.count != g_observed_demand_policy.count ||
        current.mask != g_observed_demand_policy.mask ||
        current.priority != g_observed_demand_policy.priority ||
        current.latest_generation != g_observed_demand_policy.latest_generation ||
        current.latest_interactive_generation !=
            g_observed_demand_policy.latest_interactive_generation;
    if (!changed) {
        return;
    }
    g_observed_demand_policy = current;
    ESP_LOGI(
        kTag,
        "wifi-demand policy: count=%u mask=0x%08lx priority=%u generation=%u state=%s",
        static_cast<unsigned>(current.count),
        static_cast<unsigned long>(current.mask),
        static_cast<unsigned>(current.priority),
        static_cast<unsigned>(current.latest_generation),
        StateName(g_state.load(std::memory_order_acquire)));

    const wqn::services::ConnectivityState state =
        g_state.load(std::memory_order_acquire);
    if (current.count == 0) {
        if (state == wqn::services::ConnectivityState::kOnline) {
            SetState(wqn::services::ConnectivityState::kOnlineIdleTail);
            ScheduleAction(
                ScheduledAction::kOnlineIdleTailExpired,
                kOnlineIdleTailUs);
            ESP_LOGI(
                kTag,
                "WiFi idle tail armed: delay_ms=%lld",
                static_cast<long long>(kOnlineIdleTailUs / 1000));
        } else if (state == wqn::services::ConnectivityState::kConnecting ||
                   state == wqn::services::ConnectivityState::kWaitingIp ||
                   state == wqn::services::ConnectivityState::kBackoff) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                StopRadioAndSetOff("demand-empty"));
        }
        return;
    }

    const bool interactive =
        current.priority ==
        wqn::services::ConnectivityDemandPriority::kInteractive;
    const bool fresh_interactive = interactive &&
        GenerationAfter(
            current.latest_interactive_generation,
            g_last_interactive_bypass_generation);
    if (fresh_interactive) {
        // Consume this admission generation regardless of the current state.
        // A later unrelated demand release must not turn the same user action
        // into a second backoff bypass.
        g_last_interactive_bypass_generation =
            current.latest_interactive_generation;
    }

    if (state == wqn::services::ConnectivityState::kOnlineIdleTail) {
        ClearScheduledAction();
        SetState(wqn::services::ConnectivityState::kOnline);
        ESP_LOGI(kTag, "WiFi idle tail cancelled by new demand");
        return;
    }
    if (state == wqn::services::ConnectivityState::kOff) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(StartConfiguredConnectivity());
        return;
    }

    if (fresh_interactive &&
        state == wqn::services::ConnectivityState::kBackoff) {
        ESP_LOGI(
            kTag,
            "WiFi foreground demand bypasses backoff: generation=%u rounds=%u",
            static_cast<unsigned>(current.latest_interactive_generation),
            static_cast<unsigned>(g_backoff_rounds));
        ESP_ERROR_CHECK_WITHOUT_ABORT(StartConfiguredConnectivity(true));
        return;
    }
    if (fresh_interactive &&
        state == wqn::services::ConnectivityState::kConnecting &&
        !g_attempt_active &&
        g_scheduled_action == ScheduledAction::kRetrySlot) {
        ClearScheduledAction();
        ESP_ERROR_CHECK_WITHOUT_ABORT(BeginConnectionAttempt());
    }
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
    // A credential change is an explicit user action: restart the backoff
    // ladder from rung 0 and lift any auto-retry suspension.
    ResetBackoffEscalation();

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
    if (g_demand_count.load(std::memory_order_acquire) != 0) {
        ESP_LOGE(kTag, "sleep prepare reached connectivity with active demands");
        return ESP_ERR_INVALID_STATE;
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
        ResetBackoffEscalation();
        SetOnline(true, wqn::GetWifiRssi());
        if (g_demand_count.load(std::memory_order_acquire) == 0) {
            SetState(wqn::services::ConnectivityState::kOnlineIdleTail);
            ScheduleAction(
                ScheduledAction::kOnlineIdleTailExpired,
                kOnlineIdleTailUs);
        } else {
            SetState(wqn::services::ConnectivityState::kOnline);
        }
        return ESP_OK;
    }
    if (g_demand_count.load(std::memory_order_acquire) == 0) {
        return StopRadioAndSetOff("sleep-rollback-no-demand");
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
                state == wqn::services::ConnectivityState::kOnlineIdleTail ||
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
                state == wqn::services::ConnectivityState::kOnline ||
                state == wqn::services::ConnectivityState::kOnlineIdleTail) {
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
            ResetBackoffEscalation();
            SetOnline(true, wqn::GetWifiRssi());
            if (g_demand_count.load(std::memory_order_acquire) == 0) {
                SetState(wqn::services::ConnectivityState::kOnlineIdleTail);
                ScheduleAction(
                    ScheduledAction::kOnlineIdleTailExpired,
                    kOnlineIdleTailUs);
            } else {
                SetState(wqn::services::ConnectivityState::kOnline);
            }
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
            if (g_demand_count.load(std::memory_order_acquire) == 0) {
                return StopRadioAndSetOff("disconnect-no-demand");
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
         state == wqn::services::ConnectivityState::kBackoff) ||
        (action == ScheduledAction::kOnlineIdleTailExpired &&
         state == wqn::services::ConnectivityState::kOnlineIdleTail);
    if (!action_matches_state) {
        ClearScheduledAction();
        return;
    }
    if (action == ScheduledAction::kOnlineIdleTailExpired) {
        ClearScheduledAction();
        if (g_demand_count.load(std::memory_order_acquire) == 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                StopRadioAndSetOff("idle-tail-expired"));
        } else {
            SetState(wqn::services::ConnectivityState::kOnline);
        }
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
            if (g_demand_count.load(std::memory_order_acquire) == 0) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    StopRadioAndSetOff("backoff-expired-no-demand"));
                return;
            }
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
                PublishFailure(
                    wqn::services::ConnectivityWaitResult::kNeedsProvisioning);
                SetState(wqn::services::ConnectivityState::kOff);
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
        case ScheduledAction::kOnlineIdleTailExpired:
            return;
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
    g_backoff_rounds_view.store(g_backoff_rounds, std::memory_order_release);
    g_backoff_suspended_view.store(g_backoff_suspended, std::memory_order_release);
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    wqn::SetProvisionDoneCallback(ProvisionDone);
#endif
    ESP_LOGI(
        kTag,
        "connectivity service started: command_depth=%u",
        static_cast<unsigned>(kCommandQueueDepth));

    while (true) {
        const QueueSetMemberHandle_t ready =
            xQueueSelectFromSet(g_work_set, NextCommandWait());
        if (ready == g_demand_changed) {
            xSemaphoreTake(g_demand_changed, 0);
            HandleDemandPolicyChanged();
            continue;
        }
        ConnectivityCommand command;
        if (ready == g_command_queue &&
            xQueueReceive(g_command_queue, &command, 0) == pdTRUE) {
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
    // Cold boot can dispatch sync, cloud and AI work nearly together. Exactly
    // one caller creates the owner task; concurrent callers wait for that
    // bounded operation instead of reporting a spurious network failure.
    const TickType_t wait_started_at = xTaskGetTickCount();
    while (true) {
        bool create_service = false;
        taskENTER_CRITICAL(&g_start_lock);
        if (g_task != nullptr) {
            taskEXIT_CRITICAL(&g_start_lock);
            return ESP_OK;
        }
        if (!g_starting) {
            g_starting = true;
            create_service = true;
        }
        taskEXIT_CRITICAL(&g_start_lock);
        if (create_service) {
            break;
        }
        if (xTaskGetTickCount() - wait_started_at >= kServiceStartWait) {
            ESP_LOGE(kTag, "timed out waiting for connectivity service start");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    taskENTER_CRITICAL(&g_start_lock);
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
    if (g_demand_changed == nullptr) {
        g_demand_changed =
            xSemaphoreCreateBinaryStatic(&g_demand_changed_storage);
    }
    taskEXIT_CRITICAL(&g_start_lock);
    if (g_command_queue == nullptr || g_reply_queue == nullptr ||
        g_event_group == nullptr || g_call_mutex == nullptr ||
        g_demand_changed == nullptr) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }
    if (g_work_set == nullptr) {
        g_work_set = xQueueCreateSet(kCommandQueueDepth + 1);
        if (g_work_set == nullptr ||
            xQueueAddToSet(g_command_queue, g_work_set) != pdPASS ||
            xQueueAddToSet(g_demand_changed, g_work_set) != pdPASS) {
            taskENTER_CRITICAL(&g_start_lock);
            g_starting = false;
            taskEXIT_CRITICAL(&g_start_lock);
            return ESP_ERR_NO_MEM;
        }
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

bool RegisterDemand(
    wqn::services::ConnectivityDemandReason reason,
    const char* owner,
    const char* file,
    int line,
    uint32_t* id,
    uint32_t* activation_generation)
{
    if (id == nullptr || activation_generation == nullptr) {
        return false;
    }
    *id = 0;
    *activation_generation = 0;
    DemandSlot* allocated = nullptr;
    DemandPolicySnapshot snapshot;
    taskENTER_CRITICAL(&g_demand_lock);
    for (DemandSlot& slot : g_demand_slots) {
        if (!slot.active) {
            allocated = &slot;
            break;
        }
    }
    if (allocated != nullptr) {
        uint32_t next_id = g_next_demand_id++;
        if (next_id == 0) {
            next_id = g_next_demand_id++;
        }
        const uint32_t generation = NextPolicyGenerationLocked();
        allocated->id = next_id;
        allocated->activation_generation = generation;
        allocated->reason = reason;
        allocated->owner = owner;
        allocated->file = file;
        allocated->line = line;
        allocated->active = true;
        snapshot = SnapshotDemandPolicyLocked();
        PublishDemandPolicyLocked(snapshot);
        *id = next_id;
        *activation_generation = generation;
    }
    taskEXIT_CRITICAL(&g_demand_lock);
    if (allocated == nullptr) {
        ESP_LOGE(kTag, "wifi-demand table full: owner=%s", owner);
        return false;
    }
    ESP_LOGI(
        kTag,
        "wifi-demand acquire: id=%u reason=%s owner=%s count=%u mask=0x%08lx at=%s:%d",
        static_cast<unsigned>(*id),
        DemandReasonName(reason),
        owner,
        static_cast<unsigned>(snapshot.count),
        static_cast<unsigned long>(snapshot.mask),
        file,
        line);
    NotifyDemandChanged();
    return true;
}

void ReleaseDemand(uint32_t id)
{
    if (id == 0) {
        return;
    }
    const char* owner = "unknown";
    wqn::services::ConnectivityDemandReason reason =
        wqn::services::ConnectivityDemandReason::kSyncBackground;
    bool released = false;
    DemandPolicySnapshot snapshot;
    taskENTER_CRITICAL(&g_demand_lock);
    for (DemandSlot& slot : g_demand_slots) {
        if (!slot.active || slot.id != id) {
            continue;
        }
        owner = slot.owner == nullptr ? "unknown" : slot.owner;
        reason = slot.reason;
        slot = DemandSlot{};
        released = true;
        snapshot = SnapshotDemandPolicyLocked();
        PublishDemandPolicyLocked(snapshot);
        break;
    }
    taskEXIT_CRITICAL(&g_demand_lock);
    if (!released) {
        ESP_LOGE(kTag, "wifi-demand release missing id=%u", static_cast<unsigned>(id));
        return;
    }
    ESP_LOGI(
        kTag,
        "wifi-demand release: id=%u reason=%s owner=%s count=%u mask=0x%08lx",
        static_cast<unsigned>(id),
        DemandReasonName(reason),
        owner,
        static_cast<unsigned>(snapshot.count),
        static_cast<unsigned long>(snapshot.mask));
    NotifyDemandChanged();
}

wqn::services::ConnectivityWaitResult WaitForDemandGeneration(
    uint32_t demand_id,
    uint32_t activation_generation,
    wqn::services::ConnectivityDemandPriority priority,
    TickType_t timeout)
{
    const TickType_t started_at = xTaskGetTickCount();
    while (true) {
        // Demand lifetime wins over the online idle tail. A cancellation must
        // never be observed as success merely because the radio remains online
        // for a few seconds after the last owner released it.
        if (demand_id != 0 && !IsDemandActive(demand_id)) {
            return wqn::services::ConnectivityWaitResult::kCancelled;
        }
        if (demand_id == 0 &&
            g_demand_count.load(std::memory_order_acquire) == 0) {
            return wqn::services::ConnectivityWaitResult::kCancelled;
        }
        if (g_state.load(std::memory_order_acquire) ==
            wqn::services::ConnectivityState::kQuiescing) {
            return wqn::services::ConnectivityWaitResult::kQuiescing;
        }
        if (g_online.load(std::memory_order_acquire)) {
            return wqn::services::ConnectivityWaitResult::kOnline;
        }
        const uint32_t failure_generation =
            g_last_failure_generation.load(std::memory_order_acquire);
        const bool background_held_in_backoff =
            priority ==
                wqn::services::ConnectivityDemandPriority::kBackground &&
            g_state.load(std::memory_order_acquire) ==
                wqn::services::ConnectivityState::kBackoff;
        if (failure_generation != 0 &&
            (GenerationAfter(failure_generation, activation_generation) ||
             background_held_in_backoff)) {
            return g_last_failure.load(std::memory_order_acquire);
        }

        TickType_t remaining = portMAX_DELAY;
        if (timeout != portMAX_DELAY) {
            const TickType_t elapsed = xTaskGetTickCount() - started_at;
            if (elapsed >= timeout) {
                return wqn::services::ConnectivityWaitResult::kTimedOut;
            }
            remaining = timeout - elapsed;
        }
        const TickType_t poll_bound = pdMS_TO_TICKS(200);
        const TickType_t wait = remaining == portMAX_DELAY
            ? poll_bound
            : std::min(remaining, poll_bound);
        xEventGroupWaitBits(
            g_event_group,
            kStateChangedBit,
            pdTRUE,
            pdFALSE,
            wait == 0 ? 1 : wait);
    }
}

}  // namespace

namespace wqn::services {

ConnectivityDemand::ConnectivityDemand(
    uint32_t id,
    uint32_t activation_generation,
    ConnectivityDemandReason reason,
    runtime::SleepLease&& sleep_lease)
    : id_(id),
      activation_generation_(activation_generation),
      reason_(reason),
      sleep_lease_(std::move(sleep_lease))
{
}

ConnectivityDemand::~ConnectivityDemand()
{
    Reset();
}

ConnectivityDemand::ConnectivityDemand(ConnectivityDemand&& other) noexcept
    : id_(other.id_),
      activation_generation_(other.activation_generation_),
      reason_(other.reason_),
      sleep_lease_(std::move(other.sleep_lease_))
{
    other.id_ = 0;
    other.activation_generation_ = 0;
}

ConnectivityDemand& ConnectivityDemand::operator=(
    ConnectivityDemand&& other) noexcept
{
    if (this != &other) {
        Reset();
        id_ = other.id_;
        activation_generation_ = other.activation_generation_;
        reason_ = other.reason_;
        sleep_lease_ = std::move(other.sleep_lease_);
        other.id_ = 0;
        other.activation_generation_ = 0;
    }
    return *this;
}

void ConnectivityDemand::Reset()
{
    if (id_ == 0) {
        return;
    }
    const uint32_t released_id = id_;
    id_ = 0;
    activation_generation_ = 0;
    ReleaseDemand(released_id);
    sleep_lease_.Reset();
}

ConnectivityDemand AcquireConnectivityDemand(
    ConnectivityDemandReason reason,
    const char* owner,
    const char* file,
    int line)
{
    if (owner == nullptr || file == nullptr) {
        return {};
    }
    if (EnsureServiceStarted() != ESP_OK) {
        return {};
    }
    runtime::SleepLease sleep_lease = runtime::SleepLease::TryAcquire(
        runtime::SleepBlocker::kConnectivity,
        owner,
        file,
        line);
    if (!sleep_lease) {
        ESP_LOGW(kTag, "wifi-demand rejected during sleep quiesce: owner=%s", owner);
        return {};
    }
    uint32_t id = 0;
    uint32_t generation = 0;
    if (!RegisterDemand(reason, owner, file, line, &id, &generation)) {
        return {};
    }
    return ConnectivityDemand(id, generation, reason, std::move(sleep_lease));
}

const char* ConnectivityWaitResultName(ConnectivityWaitResult result)
{
    switch (result) {
        case ConnectivityWaitResult::kOnline:
            return "online";
        case ConnectivityWaitResult::kNeedsProvisioning:
            return "needs-provisioning";
        case ConnectivityWaitResult::kAuthFailed:
            return "auth-failed";
        case ConnectivityWaitResult::kUnavailable:
            return "unavailable";
        case ConnectivityWaitResult::kTimedOut:
            return "timed-out";
        case ConnectivityWaitResult::kQuiescing:
            return "quiescing";
        case ConnectivityWaitResult::kCancelled:
            return "cancelled";
        default:
            return "unknown";
    }
}

esp_err_t ConnectivityWaitResultToEspErr(ConnectivityWaitResult result)
{
    switch (result) {
        case ConnectivityWaitResult::kOnline:
            return ESP_OK;
        case ConnectivityWaitResult::kNeedsProvisioning:
            return ESP_ERR_NOT_FOUND;
        case ConnectivityWaitResult::kTimedOut:
            return ESP_ERR_TIMEOUT;
        case ConnectivityWaitResult::kAuthFailed:
        case ConnectivityWaitResult::kQuiescing:
        case ConnectivityWaitResult::kCancelled:
            return ESP_ERR_INVALID_STATE;
        case ConnectivityWaitResult::kUnavailable:
        default:
            return ESP_FAIL;
    }
}

ConnectivityWaitResult WaitForConnectivity(
    const ConnectivityDemand& demand,
    TickType_t timeout)
{
    return WaitForConnectivity(demand.ticket(), timeout);
}

ConnectivityWaitResult WaitForConnectivity(
    ConnectivityDemandTicket ticket,
    TickType_t timeout)
{
    if (ticket.id == 0) {
        return ConnectivityWaitResult::kCancelled;
    }
    return WaitForDemandGeneration(
        ticket.id,
        ticket.activation_generation,
        PriorityForReason(ticket.reason),
        timeout);
}

esp_err_t WaitForConnectivity(TickType_t timeout)
{
    if (g_demand_count.load(std::memory_order_acquire) == 0) {
        ESP_LOGE(kTag, "readiness wait rejected without a ConnectivityDemand");
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t generation =
        g_latest_demand_generation.load(std::memory_order_acquire);
    return ConnectivityWaitResultToEspErr(
        WaitForDemandGeneration(
            0,
            generation,
            g_demand_priority.load(std::memory_order_acquire),
            timeout));
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
    const esp_err_t start_result = EnsureServiceStarted();
    if (start_result != ESP_OK) {
        ESP_LOGE(
            kTag,
            "start connectivity for provisioning failed: %s",
            esp_err_to_name(start_result));
        return;
    }
    if (!PostAsyncCommand(command)) {
        ESP_LOGE(kTag, "queue provisioning request failed");
    }
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
    snapshot.demand_count = g_demand_count.load(std::memory_order_acquire);
    snapshot.demand_mask = g_demand_mask.load(std::memory_order_acquire);
    snapshot.demand_priority =
        g_demand_priority.load(std::memory_order_acquire);
    snapshot.backoff_rounds =
        g_backoff_rounds_view.load(std::memory_order_acquire);
    snapshot.backoff_suspended =
        g_backoff_suspended_view.load(std::memory_order_acquire);
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
