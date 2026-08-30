#include "services/sync_service.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "device_protocol/claim_crypto.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/wake_context.h"
#include "storage.h"
#include "note_store.h"
#include "outbox_suspend_reason.h"
#include "problem_store.h"
#include "services/connectivity_service.h"
#include "services/server_error_codes.h"
#include "word_study_store.h"
#include "wqn_api.h"

namespace {

constexpr char kTag[] = "sync_service";

#if CONFIG_WQN_WIFI_STA_ENABLE
wqn::services::SyncSnapshot g_sync_snapshot = {};
portMUX_TYPE g_sync_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_sync_event_sequence = 1;
wqn::services::SyncEvent g_latest_sync_event = {};
std::atomic<wqn::services::SyncEventSink> g_sync_event_sink{nullptr};
// [gap-1] Nominal full-sync retry ladder: a persistent failure used to pin the
// retry cadence at 10s forever, keeping the radio hot across deep-sleep cycles
// (2026-08-19 sync liveness audit). Consecutive nominal-path failures escalate
// through this ladder and cap at 15 minutes; ClearFullSyncRetry() resets the
// attempt count on success or when an explicit manual/boot reason runs.
constexpr uint32_t kFullSyncRetryLadderMs[] = {
    10000, 30000, 60000, 300000, 900000};
constexpr size_t kFullSyncRetryLadderSize =
    sizeof(kFullSyncRetryLadderMs) / sizeof(kFullSyncRetryLadderMs[0]);
// RTC retention keeps the escalation alive across deep-sleep wake cycles;
// only the sync task touches it (schedule/retry decisions are single-tasked).
RTC_DATA_ATTR uint8_t g_full_sync_retry_attempts = 0;
bool LoadUsableToken(std::string* token);
TaskHandle_t g_sync_service_task = nullptr;
enum FullSyncReason : uint32_t {
    kFullSyncManual = 1u << 0,
    kFullSyncBoot = 1u << 1,
    kFullSyncCredentials = 1u << 2,
    kFullSyncContentRefresh = 1u << 3,
};
std::atomic<uint32_t> g_full_sync_reasons{0};
std::atomic<bool> g_word_outbox_sync_requested{false};
std::atomic<bool> g_outbox_immediate_requested{false};
std::atomic<bool> g_outbox_transport_resume_requested{false};
std::atomic<uint32_t> g_auto_sync_interval_minutes{0};
std::atomic<int64_t> g_next_periodic_sync_not_before_ms{0};
std::atomic<bool> g_boot_outbox_pending{false};
std::atomic<bool> g_boot_policy_evaluated{false};
constexpr uint32_t kPeriodicScheduleMagic = 0x57514E53;  // "WQNS"
constexpr uint32_t kFullRetryMagic = 0x57514E52;  // "WQNR"
constexpr std::time_t kMinScheduleUnixTime = 1704067200;  // 2024-01-01 UTC
RTC_DATA_ATTR uint32_t g_periodic_schedule_magic = 0;
RTC_DATA_ATTR uint32_t g_periodic_schedule_interval_minutes = 0;
RTC_DATA_ATTR int64_t g_next_periodic_sync_unix_seconds = 0;
RTC_DATA_ATTR uint32_t g_full_sync_retry_magic = 0;
RTC_DATA_ATTR int64_t g_full_sync_retry_unix_seconds = 0;
std::atomic<int64_t> g_full_sync_retry_not_before_ms{0};
portMUX_TYPE g_periodic_schedule_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> g_content_refresh_requested{0};
constexpr uint32_t kWordPacksRefreshBit = 1u << 0;
constexpr uint32_t kNotePacksRefreshBit = 1u << 1;
constexpr uint32_t kProblemPacksRefreshBit = 1u << 2;
std::atomic<uint32_t> g_word_interaction_generation{0};
constexpr uint32_t kWordOutboxQuietPeriodMs = 5000;
// A steady stream of study input may keep resetting the 5-second debounce.
// Bound the pre-upload sleep lease so active use still makes durable progress
// without turning a non-empty outbox into a permanent sleep blocker.
constexpr uint32_t kOutboxFlushLeaseMaxMs = 15000;
StaticTimer_t g_word_outbox_timer_storage;
TimerHandle_t g_word_outbox_timer = nullptr;
portMUX_TYPE g_outbox_quiet_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_outbox_quiet_active = false;
int64_t g_outbox_quiet_due_ms = 0;
int64_t g_outbox_quiet_deadline_ms = 0;
uint32_t g_outbox_quiet_generation = 0;
uint32_t g_outbox_ready_generation = 0;
bool g_outbox_lease_cycle_expired = false;
wqn::SyncJournal g_sync_journal = {};
bool g_sync_journal_loaded = false;
StaticSemaphore_t g_sync_journal_mutex_storage;
SemaphoreHandle_t g_sync_journal_mutex = nullptr;
uint32_t g_content_claim_generation[3] = {};
uint32_t g_content_active_generation[3] = {};

size_t ContentDomainIndex(wqn::services::SyncContentDomain domain)
{
    return static_cast<size_t>(domain);
}

uint32_t ContentRefreshBit(wqn::services::SyncContentDomain domain)
{
    switch (domain) {
        case wqn::services::SyncContentDomain::kWordPacks:
            return kWordPacksRefreshBit;
        case wqn::services::SyncContentDomain::kNotePacks:
            return kNotePacksRefreshBit;
        case wqn::services::SyncContentDomain::kProblemPacks:
            return kProblemPacksRefreshBit;
        default:
            return 0;
    }
}

std::time_t CurrentUnixSeconds()
{
    std::time_t now = 0;
    std::time(&now);
    return now;
}

bool PeriodicScheduleDue(uint32_t interval_minutes)
{
    if (interval_minutes == 0) {
        return false;
    }
    uint32_t magic = 0;
    uint32_t scheduled_interval = 0;
    int64_t due_seconds = 0;
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    magic = g_periodic_schedule_magic;
    scheduled_interval = g_periodic_schedule_interval_minutes;
    due_seconds = g_next_periodic_sync_unix_seconds;
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
    const std::time_t now = CurrentUnixSeconds();
    // Missing/old schedule state is due once. A successful round writes the
    // first valid absolute deadline, which then survives deep sleep.
    if (magic != kPeriodicScheduleMagic ||
        scheduled_interval != interval_minutes) {
        return true;
    }
    if (due_seconds < static_cast<int64_t>(kMinScheduleUnixTime) ||
        now < kMinScheduleUnixTime) {
        return g_next_periodic_sync_not_before_ms.load(
                   std::memory_order_acquire) <=
            esp_timer_get_time() / 1000;
    }
    return static_cast<int64_t>(now) >= due_seconds;
}

void ScheduleNextPeriodicSync(uint32_t interval_minutes)
{
    const std::time_t now = CurrentUnixSeconds();
    g_next_periodic_sync_not_before_ms.store(
        interval_minutes == 0
            ? 0
            : esp_timer_get_time() / 1000 +
                static_cast<int64_t>(interval_minutes) * 60 * 1000,
        std::memory_order_release);
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    if (interval_minutes == 0) {
        g_periodic_schedule_magic = 0;
        g_periodic_schedule_interval_minutes = interval_minutes;
        g_next_periodic_sync_unix_seconds = 0;
    } else {
        g_periodic_schedule_interval_minutes = interval_minutes;
        // The UI seeds wall time after app_main starts the services. If a
        // successful cold-boot sync wins that race, keep a valid relative
        // schedule marker instead of clearing the schedule and immediately
        // spinning another full round. Once wall time is valid, later rounds
        // replace this zero sentinel with the absolute deadline.
        g_next_periodic_sync_unix_seconds = now >= kMinScheduleUnixTime
            ? static_cast<int64_t>(now) +
                static_cast<int64_t>(interval_minutes) * 60
            : 0;
        g_periodic_schedule_magic = kPeriodicScheduleMagic;
    }
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
}

TickType_t PeriodicSyncWaitDelay(uint32_t interval_minutes)
{
    if (interval_minutes == 0) {
        return portMAX_DELAY;
    }
    uint32_t magic = 0;
    uint32_t scheduled_interval = 0;
    int64_t due_seconds = 0;
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    magic = g_periodic_schedule_magic;
    scheduled_interval = g_periodic_schedule_interval_minutes;
    due_seconds = g_next_periodic_sync_unix_seconds;
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
    const std::time_t now = CurrentUnixSeconds();
    if (magic != kPeriodicScheduleMagic ||
        scheduled_interval != interval_minutes) {
        return 0;
    }
    if (due_seconds < static_cast<int64_t>(kMinScheduleUnixTime) ||
        now < kMinScheduleUnixTime) {
        const int64_t remaining_ms =
            g_next_periodic_sync_not_before_ms.load(
                std::memory_order_acquire) -
            esp_timer_get_time() / 1000;
        if (remaining_ms <= 0) {
            return 0;
        }
        const uint64_t fallback_ticks =
            static_cast<uint64_t>(remaining_ms) / portTICK_PERIOD_MS;
        return fallback_ticks >= static_cast<uint64_t>(portMAX_DELAY)
            ? portMAX_DELAY - 1
            : std::max<TickType_t>(1, static_cast<TickType_t>(fallback_ticks));
    }
    if (static_cast<int64_t>(now) >= due_seconds) {
        return 0;
    }
    const uint64_t remaining_ms =
        static_cast<uint64_t>(due_seconds - static_cast<int64_t>(now)) * 1000ULL;
    const uint64_t ticks = remaining_ms / portTICK_PERIOD_MS;
    return ticks >= static_cast<uint64_t>(portMAX_DELAY)
        ? portMAX_DELAY - 1
        : std::max<TickType_t>(1, static_cast<TickType_t>(ticks));
}

void ClearFullSyncRetry()
{
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    g_full_sync_retry_magic = 0;
    g_full_sync_retry_unix_seconds = 0;
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
    g_full_sync_retry_not_before_ms.store(0, std::memory_order_release);
    g_full_sync_retry_attempts = 0;
}

void ScheduleFullSyncRetry(uint32_t delay_ms)
{
    const std::time_t now = CurrentUnixSeconds();
    const int64_t delay_seconds = std::max<int64_t>(1, (delay_ms + 999) / 1000);
    g_full_sync_retry_not_before_ms.store(
        esp_timer_get_time() / 1000 + delay_seconds * 1000,
        std::memory_order_release);
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    g_full_sync_retry_unix_seconds = now >= kMinScheduleUnixTime
        ? static_cast<int64_t>(now) + delay_seconds
        : 0;
    // Keep the retry marker even before wall time is seeded. The in-boot
    // monotonic deadline drives the wait; if deep sleep/restart resets that
    // scalar, the retained zero wall deadline is conservatively due once.
    g_full_sync_retry_magic = kFullRetryMagic;
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
}

TickType_t FullSyncRetryWaitDelay()
{
    uint32_t retry_magic = 0;
    int64_t due_seconds = 0;
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    retry_magic = g_full_sync_retry_magic;
    due_seconds = g_full_sync_retry_unix_seconds;
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
    if (retry_magic != kFullRetryMagic) {
        return portMAX_DELAY;
    }
    const std::time_t now = CurrentUnixSeconds();
    if (due_seconds == 0 || now < kMinScheduleUnixTime) {
        const int64_t remaining_ms =
            g_full_sync_retry_not_before_ms.load(std::memory_order_acquire) -
            esp_timer_get_time() / 1000;
        if (remaining_ms <= 0) {
            return 0;
        }
        const uint64_t ticks =
            static_cast<uint64_t>(remaining_ms) / portTICK_PERIOD_MS;
        return ticks >= static_cast<uint64_t>(portMAX_DELAY)
            ? portMAX_DELAY - 1
            : std::max<TickType_t>(1, static_cast<TickType_t>(ticks));
    }
    if (static_cast<int64_t>(now) >= due_seconds) {
        return 0;
    }
    const uint64_t remaining_ms =
        static_cast<uint64_t>(due_seconds - static_cast<int64_t>(now)) * 1000ULL;
    const uint64_t ticks = remaining_ms / portTICK_PERIOD_MS;
    return ticks >= static_cast<uint64_t>(portMAX_DELAY)
        ? portMAX_DELAY - 1
        : std::max<TickType_t>(1, static_cast<TickType_t>(ticks));
}

bool FullSyncRetryDue()
{
    return FullSyncRetryWaitDelay() == 0;
}

bool JournalHasPendingContent(const wqn::SyncJournal& journal)
{
    const auto pending = [](const wqn::SyncJournalContentState& state) {
        return state.desired_revision > state.applied_revision &&
            state.phase != wqn::SyncJournalPhase::kBlocked;
    };
    return pending(journal.word_packs) || pending(journal.note_packs) ||
        pending(journal.problem_packs);
}

bool ProbeDurableOutboxWork()
{
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    wqn::DurableWordObservation word;
    esp_err_t result = wqn::PeekPendingWordObservation(&word);
    if (result == ESP_OK) {
        return true;
    }
    if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "word outbox boot probe failed: %s",
                 esp_err_to_name(result));
        return true;
    }
    wqn::DurableNoteObservation note;
    result = wqn::PeekPendingNoteObservation(&note);
    if (result == ESP_OK) {
        return true;
    }
    if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "note outbox boot probe failed: %s",
                 esp_err_to_name(result));
        return true;
    }
    wqn::DurableProblemObservation problem;
    result = wqn::PeekPendingProblemObservation(&problem);
    if (result == ESP_OK) {
        return true;
    }
    if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "problem outbox boot probe failed: %s",
                 esp_err_to_name(result));
        return true;
    }
#endif
    return false;
}

uint32_t ArmOutboxQuietWindow()
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t generation = 0;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    generation = ++g_outbox_quiet_generation;
    if (generation == 0) {
        generation = ++g_outbox_quiet_generation;
    }
    if (!g_outbox_quiet_active) {
        g_outbox_quiet_deadline_ms =
            now_ms + static_cast<int64_t>(kOutboxFlushLeaseMaxMs);
    }
    g_outbox_quiet_due_ms =
        now_ms + static_cast<int64_t>(kWordOutboxQuietPeriodMs);
    g_outbox_quiet_active = true;
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    return generation;
}

void PublishOutboxReadyGeneration(uint32_t generation)
{
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    g_outbox_ready_generation = generation;
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    // Publish urgency before the ready flag. A consumer that observes the flag
    // must also observe that this round is due now rather than retry-deferred.
    g_outbox_immediate_requested.store(true, std::memory_order_relaxed);
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
}

void PublishCurrentOutboxQuietWindowReady()
{
    uint32_t generation = 0;
    bool ready = false;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    if (g_outbox_quiet_active) {
        generation = g_outbox_quiet_generation;
        g_outbox_ready_generation = generation;
        ready = true;
    }
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    if (!ready) {
        return;
    }
    g_outbox_immediate_requested.store(true, std::memory_order_relaxed);
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
}

bool OutboxQuietWindowActive()
{
    bool active = false;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    active = g_outbox_quiet_active;
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    return active;
}

TickType_t OutboxQuietLeaseWaitDelay()
{
    bool active = false;
    int64_t deadline_ms = 0;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    active = g_outbox_quiet_active;
    deadline_ms = g_outbox_quiet_deadline_ms;
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    if (!active) {
        return portMAX_DELAY;
    }
    const int64_t remaining_ms = deadline_ms - esp_timer_get_time() / 1000;
    if (remaining_ms <= 0) {
        return 0;
    }
    return pdMS_TO_TICKS(static_cast<uint32_t>(std::min<int64_t>(
        remaining_ms, kOutboxFlushLeaseMaxMs)));
}

uint32_t SecondsUntilOutboxQuietWake()
{
    bool active = false;
    int64_t due_ms = 0;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    active = g_outbox_quiet_active;
    due_ms = g_outbox_quiet_due_ms;
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    if (!active) {
        return UINT32_MAX;
    }
    const int64_t remaining_ms = due_ms - esp_timer_get_time() / 1000;
    return remaining_ms <= 0
        ? 1
        : static_cast<uint32_t>(std::max<int64_t>(
              1, (remaining_ms + 999) / 1000));
}

void ForceExpiredOutboxQuietWindow()
{
    bool expired = false;
    uint32_t generation = 0;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    if (g_outbox_quiet_active && g_outbox_quiet_deadline_ms <= now_ms) {
        generation = g_outbox_quiet_generation;
        g_outbox_ready_generation = generation;
        g_outbox_quiet_active = false;
        g_outbox_quiet_due_ms = 0;
        g_outbox_quiet_deadline_ms = 0;
        g_outbox_lease_cycle_expired = true;
        expired = true;
    }
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    if (!expired) {
        return;
    }
    ESP_LOGI(
        kTag,
        "outbox quiet window capped; forcing bounded progress: generation=%lu",
        static_cast<unsigned long>(generation));
    g_outbox_immediate_requested.store(true, std::memory_order_relaxed);
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
}

void ClaimOutboxReadyGeneration()
{
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    const uint32_t ready_generation = g_outbox_ready_generation;
    g_outbox_ready_generation = 0;
    // A newer durable observation may have reset the timer after an older
    // callback fired. Keep that newer quiet window (and its lease) armed.
    if (ready_generation != 0 &&
        ready_generation == g_outbox_quiet_generation) {
        g_outbox_quiet_active = false;
        g_outbox_quiet_due_ms = 0;
        g_outbox_quiet_deadline_ms = 0;
    }
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
}

bool TakeOutboxLeaseCycleExpired()
{
    bool expired = false;
    taskENTER_CRITICAL(&g_outbox_quiet_lock);
    expired = g_outbox_lease_cycle_expired;
    g_outbox_lease_cycle_expired = false;
    taskEXIT_CRITICAL(&g_outbox_quiet_lock);
    return expired;
}

wqn::services::SyncContentSnapshot* ContentSnapshotForKind(
    wqn::protocol::v3::SyncContentKind kind)
{
    switch (kind) {
        case wqn::protocol::v3::SyncContentKind::kWordPacks:
            return &g_sync_snapshot.word_packs;
        case wqn::protocol::v3::SyncContentKind::kNotePacks:
            return &g_sync_snapshot.note_packs;
        case wqn::protocol::v3::SyncContentKind::kProblemPacks:
            return &g_sync_snapshot.problem_packs;
        default:
            return nullptr;
    }
}

wqn::SyncJournalContentState* JournalStateForKind(
    wqn::protocol::v3::SyncContentKind kind)
{
    switch (kind) {
        case wqn::protocol::v3::SyncContentKind::kWordPacks:
            return &g_sync_journal.word_packs;
        case wqn::protocol::v3::SyncContentKind::kNotePacks:
            return &g_sync_journal.note_packs;
        case wqn::protocol::v3::SyncContentKind::kProblemPacks:
            return &g_sync_journal.problem_packs;
        default:
            return nullptr;
    }
}

wqn::services::SyncContentSnapshot* ContentSnapshotForDomain(
    wqn::services::SyncContentDomain domain)
{
    switch (domain) {
        case wqn::services::SyncContentDomain::kWordPacks:
            return &g_sync_snapshot.word_packs;
        case wqn::services::SyncContentDomain::kNotePacks:
            return &g_sync_snapshot.note_packs;
        case wqn::services::SyncContentDomain::kProblemPacks:
            return &g_sync_snapshot.problem_packs;
        default:
            return nullptr;
    }
}

wqn::SyncJournalContentState* JournalStateForDomain(
    wqn::services::SyncContentDomain domain)
{
    switch (domain) {
        case wqn::services::SyncContentDomain::kWordPacks:
            return &g_sync_journal.word_packs;
        case wqn::services::SyncContentDomain::kNotePacks:
            return &g_sync_journal.note_packs;
        case wqn::services::SyncContentDomain::kProblemPacks:
            return &g_sync_journal.problem_packs;
        default:
            return nullptr;
    }
}

esp_err_t PersistLatestSyncJournal()
{
    if (g_sync_journal_mutex == nullptr ||
        xSemaphoreTake(g_sync_journal_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    wqn::SyncJournal snapshot;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    snapshot = g_sync_journal;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    const esp_err_t result = wqn::SaveSyncJournal(snapshot);
    xSemaphoreGive(g_sync_journal_mutex);
    return result;
}

void PublishContentTargets(
    const std::vector<wqn::protocol::v3::SyncContentTarget>& targets)
{
    bool changed = false;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    for (const auto& target : targets) {
        if (target.kind == wqn::protocol::v3::SyncContentKind::kTodos) {
            if (target.revision > g_sync_snapshot.todo_revision) {
                g_sync_snapshot.todo_revision = target.revision;
                ++g_sync_snapshot.state_sequence;
            }
            continue;
        }
        wqn::services::SyncContentSnapshot* snapshot =
            ContentSnapshotForKind(target.kind);
        if (snapshot == nullptr || target.revision == 0) {
            continue;
        }
        if (target.revision > snapshot->desired_revision) {
            snapshot->desired_revision = target.revision;
            snapshot->phase = wqn::services::SyncContentPhase::kPending;
            snapshot->retry_attempt = 0;
            snapshot->next_retry_ms = 0;
            snapshot->last_error[0] = '\0';
            wqn::SyncJournalContentState* journal_state =
                JournalStateForKind(target.kind);
            if (journal_state != nullptr) {
                journal_state->desired_revision = target.revision;
                journal_state->phase = wqn::SyncJournalPhase::kPending;
                journal_state->retry_attempt = 0;
                journal_state->desired_snapshot_id[0] = '\0';
            }
            ++g_sync_snapshot.state_sequence;
            changed = true;
        }
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    if (changed && PersistLatestSyncJournal() != ESP_OK) {
        ESP_LOGW(kTag, "content target journal save failed");
    }
}

void WordOutboxTimerCallback(TimerHandle_t)
{
    PublishCurrentOutboxQuietWindowReady();
}
#endif

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
uint64_t g_config_revision = 0;
uint64_t g_sync_cursor = 0;
bool g_bootstrap_complete = false;
bool g_control_state_loaded = false;
// Per-dispatch signal from claim/bootstrap/sync. The scheduler converts it to
// the same sticky protocol-blocked latch used by observation uploads.
bool g_control_protocol_blocked_this_round = false;
uint32_t g_control_retry_after_ms = 0;
constexpr uint32_t kClaimPollFloorMs = 10000;
constexpr uint32_t kClaimPollJitterMaxMs = 2000;
constexpr uint32_t kClaimRetryBaseMs = 15000;
constexpr uint32_t kClaimRetryMaxMs = 5 * 60 * 1000;
constexpr uint8_t kClaimRetryMaxShift = 4;
constexpr uint32_t kWordOutboxRetryBaseMs = 30000;
constexpr uint32_t kWordOutboxRetryMaxMs = 5 * 60 * 1000;
constexpr uint32_t kWordOutboxRetryJitterMaxMs = 2000;
constexpr uint8_t kWordOutboxRetryMaxShift = 4;
std::string g_bootstrap_request_id;
std::string g_sync_request_id;
uint32_t g_sync_request_auto_interval_minutes = 0;
wqn::protocol::v3::ClaimKeyPair g_claim_key_pair;
std::string g_claim_start_request_id;
std::string g_claim_id;
uint32_t g_claim_poll_interval_ms = kClaimPollFloorMs;
uint8_t g_claim_retry_attempts = 0;
bool g_claim_active = false;

enum class WordOutboxUploadState : uint8_t {
    kDrained,
    kPending,
    kYielded,
    kAuthenticationRequired,
    // The server rejected the request at the protocol level (426
    // UPGRADE_REQUIRED). Outbound polling suspends until OTA; records stay.
    kProtocolBlocked,
    kFailed,
};

enum class OutboxRetryCause : uint8_t {
    kNone,
    kTransport,
    kServer,
    kLocalStorage,
};

enum class OutboxFailureDisposition : uint8_t {
    kAuthenticationRequired,
    kTransientTransport,
    kTransientServer,
    // Terminal classes (audit §16.B). The first three can still make queue
    // progress; the last two must not delete or wedge the head.
    kTombstoneRecoverable,
    kSequenceResolved,
    kSessionTerminal,
    kProtocolBlocked,
    kProtocolIntegrity,
};

// Maps a raw failure onto the seven-class taxonomy. Transport/retryable
// ambiguity is resolved before the code table: a damaged envelope retries
// because the server may have consumed the idempotency key already. An
// unrecognized non-retryable code is conservatively treated as protocol
// integrity (park, never delete) until a firmware update teaches us better.
constexpr OutboxFailureDisposition ClassifyOutboxFailure(
    bool authentication_required,
    bool transport_failure,
    bool server_retryable,
    wqn::services::ServerErrorClass code_class)
{
    using wqn::services::ServerErrorClass;
    if (authentication_required ||
        code_class == ServerErrorClass::kAuthRequired) {
        return OutboxFailureDisposition::kAuthenticationRequired;
    }
    if (transport_failure) {
        return OutboxFailureDisposition::kTransientTransport;
    }
    if (server_retryable) {
        return OutboxFailureDisposition::kTransientServer;
    }
    switch (code_class) {
        case ServerErrorClass::kSequenceResolved:
            return OutboxFailureDisposition::kSequenceResolved;
        case ServerErrorClass::kSessionTerminal:
            return OutboxFailureDisposition::kSessionTerminal;
        case ServerErrorClass::kTombstoneRecoverable:
            return OutboxFailureDisposition::kTombstoneRecoverable;
        case ServerErrorClass::kProtocolBlocked:
            return OutboxFailureDisposition::kProtocolBlocked;
        case ServerErrorClass::kProtocolIntegrity:
        case ServerErrorClass::kUnknownCode:
            // Unrecognized non-empty terminal codes park conservatively
            // until a firmware update teaches us the semantics.
            return OutboxFailureDisposition::kProtocolIntegrity;
        case ServerErrorClass::kAuthRequired:
            return OutboxFailureDisposition::kAuthenticationRequired;
        case ServerErrorClass::kUnrecognized:
        case ServerErrorClass::kTransientRetry:
            break;
    }
    return OutboxFailureDisposition::kTransientServer;
}

static_assert(
    ClassifyOutboxFailure(
        false,
        false,
        false,
        wqn::services::ClassifyServerErrorCode("UNAUTHORIZED")) ==
    OutboxFailureDisposition::kAuthenticationRequired);
static_assert(
    ClassifyOutboxFailure(false, true, false,
                          wqn::services::ServerErrorClass::kUnrecognized) ==
    OutboxFailureDisposition::kTransientTransport);
static_assert(
    ClassifyOutboxFailure(false, false, true,
                          wqn::services::ServerErrorClass::kUnrecognized) ==
    OutboxFailureDisposition::kTransientServer);
static_assert(
    ClassifyOutboxFailure(
        false,
        false,
        false,
        wqn::services::ClassifyServerErrorCode("SEQUENCE_ALREADY_APPLIED")) ==
    OutboxFailureDisposition::kSequenceResolved);
static_assert(
    ClassifyOutboxFailure(
        false, false, false,
        wqn::services::ClassifyServerErrorCode("SESSION_NOT_ACTIVE")) ==
    OutboxFailureDisposition::kSessionTerminal);
static_assert(
    ClassifyOutboxFailure(
        false, false, false,
        wqn::services::ClassifyServerErrorCode("ITEM_NOT_VISIBLE")) ==
    OutboxFailureDisposition::kTombstoneRecoverable);
static_assert(
    ClassifyOutboxFailure(
        false, false, false,
        wqn::services::ClassifyServerErrorCode("UPGRADE_REQUIRED")) ==
    OutboxFailureDisposition::kProtocolBlocked);
static_assert(
    ClassifyOutboxFailure(
        false, false, false,
        wqn::services::ClassifyServerErrorCode("REQUEST_ID_REUSED")) ==
    OutboxFailureDisposition::kProtocolIntegrity);
static_assert(
    ClassifyOutboxFailure(
        false,
        false,
        false,
        wqn::services::ClassifyServerErrorCode("SOME_FUTURE_CODE")) ==
    OutboxFailureDisposition::kProtocolIntegrity);

OutboxFailureDisposition ClassifyOutboxFailure(
    const wqn::protocol::v3::Error& error,
    bool transport_failure,
    bool force_retryable = false)
{
    return ClassifyOutboxFailure(
        error.code == "UNAUTHORIZED",
        transport_failure,
        error.retryable || force_retryable,
        wqn::services::ClassifyServerErrorCode(error.code));
}

OutboxRetryCause RetryCauseFor(OutboxFailureDisposition disposition)
{
    return disposition == OutboxFailureDisposition::kTransientTransport
        ? OutboxRetryCause::kTransport
        : OutboxRetryCause::kServer;
}

// Picks the durable park reason recorded next to a suspended observation.
wqn::OutboxSuspendReason SuspendReasonFor(const std::string& code)
{
    if (code == "REQUEST_ID_REUSED") {
        return wqn::OutboxSuspendReason::kIdempotencyConflict;
    }
    if (code == "SESSION_ACTOR_MISMATCH") {
        return wqn::OutboxSuspendReason::kActorOwnership;
    }
    if (code == "INVALID_STUDY_OBSERVATION" ||
        code == "INVALID_REQUEST") {
        return wqn::OutboxSuspendReason::kInvalidIdentity;
    }
    if (code == "UPGRADE_REQUIRED") {
        return wqn::OutboxSuspendReason::kProtocolBlocked;
    }
    return wqn::OutboxSuspendReason::kUnknownTerminal;
}

const char* OutboxRetryCauseName(OutboxRetryCause cause)
{
    switch (cause) {
        case OutboxRetryCause::kTransport:
            return "transport";
        case OutboxRetryCause::kServer:
            return "server";
        case OutboxRetryCause::kLocalStorage:
            return "local-storage";
        case OutboxRetryCause::kNone:
        default:
            return "none";
    }
}

std::string g_word_outbox_retry_request_id;
int64_t g_word_outbox_retry_not_before_ms = 0;
uint8_t g_word_outbox_retry_attempts = 0;
OutboxRetryCause g_word_outbox_retry_cause = OutboxRetryCause::kNone;

enum class SyncRoundOutcome : uint8_t {
    kSucceeded,
    kPartial,
    kPartialNeedsFullRetry,
    // The server rejected outbound traffic at the protocol level (426
    // UPGRADE_REQUIRED): suspend retry polling until an OTA lands instead
    // of burning the retry ladder against a contract we cannot satisfy.
    kProtocolBlocked,
    kFailed,
};

// Sticky suspension latch for CLIENT_PROTOCOL_BLOCKED. RAM-resident by
// design: an OTA reboot re-probes the server once and either clears it or
// re-arms it with fresh information.
bool g_outbox_protocol_suspended = false;

WordOutboxUploadState g_last_word_outbox_upload_state =
    WordOutboxUploadState::kDrained;

// Note observations ride the same quiet-window timer and interaction-yield
// signal as word, but keep an independent retry cursor so a deferred note head
// never blocks word progress (and vice versa). WordOutboxUploadState is reused
// verbatim; the semantics are identical.
std::string g_note_outbox_retry_request_id;
int64_t g_note_outbox_retry_not_before_ms = 0;
uint8_t g_note_outbox_retry_attempts = 0;
OutboxRetryCause g_note_outbox_retry_cause = OutboxRetryCause::kNone;
WordOutboxUploadState g_last_note_outbox_upload_state =
    WordOutboxUploadState::kDrained;

// Problem verdicts: same quiet-window trigger, independent retry cursor.
// There is no per-session sequence, so terminal rejections quarantine
// directly without a skip tombstone.
std::string g_problem_outbox_retry_request_id;
int64_t g_problem_outbox_retry_not_before_ms = 0;
uint8_t g_problem_outbox_retry_attempts = 0;
OutboxRetryCause g_problem_outbox_retry_cause = OutboxRetryCause::kNone;
WordOutboxUploadState g_last_problem_outbox_upload_state =
    WordOutboxUploadState::kDrained;

void ResetWordOutboxRetryBackoff()
{
    g_word_outbox_retry_request_id.clear();
    g_word_outbox_retry_not_before_ms = 0;
    g_word_outbox_retry_attempts = 0;
    g_word_outbox_retry_cause = OutboxRetryCause::kNone;
}

bool WordOutboxRetryDeferred(
    const std::string& request_id,
    int64_t now_ms)
{
    if (g_word_outbox_retry_request_id != request_id) {
        ResetWordOutboxRetryBackoff();
        return false;
    }
    return g_word_outbox_retry_not_before_ms > now_ms;
}

void ScheduleWordOutboxRetry(
    const std::string& request_id,
    uint32_t server_retry_after_ms,
    OutboxRetryCause cause)
{
    if (g_word_outbox_retry_request_id != request_id) {
        ResetWordOutboxRetryBackoff();
        g_word_outbox_retry_request_id = request_id;
    }
    const uint8_t shift =
        std::min(g_word_outbox_retry_attempts, kWordOutboxRetryMaxShift);
    const uint32_t local_delay_ms = std::min(
        kWordOutboxRetryBaseMs << shift,
        kWordOutboxRetryMaxMs);
    const uint32_t requested_delay_ms =
        std::min(server_retry_after_ms, kWordOutboxRetryMaxMs);
    const uint32_t base_delay_ms =
        std::max(local_delay_ms, requested_delay_ms);
    const uint32_t available_jitter_ms =
        kWordOutboxRetryMaxMs - base_delay_ms;
    const uint32_t jitter_ms = esp_random() %
        (std::min(available_jitter_ms, kWordOutboxRetryJitterMaxMs) + 1);
    const uint32_t delay_ms = base_delay_ms + jitter_ms;
    if (g_word_outbox_retry_attempts < std::numeric_limits<uint8_t>::max()) {
        ++g_word_outbox_retry_attempts;
    }
    g_word_outbox_retry_not_before_ms =
        esp_timer_get_time() / 1000 + static_cast<int64_t>(delay_ms);
    g_word_outbox_retry_cause = cause;
    ESP_LOGW(
        kTag,
        "word outbox retry scheduled: request=%s cause=%s attempt=%u retry_after_ms=%lu",
        request_id.c_str(),
        OutboxRetryCauseName(cause),
        static_cast<unsigned>(g_word_outbox_retry_attempts),
        static_cast<unsigned long>(delay_ms));
}

TickType_t WordOutboxRetryWaitDelay()
{
    if (g_word_outbox_retry_request_id.empty()) {
        return portMAX_DELAY;
    }
    const int64_t remaining_ms =
        g_word_outbox_retry_not_before_ms - esp_timer_get_time() / 1000;
    if (remaining_ms <= 0) {
        return 0;
    }
    return pdMS_TO_TICKS(
        static_cast<uint32_t>(
            std::min<int64_t>(remaining_ms, kWordOutboxRetryMaxMs)));
}

void ResetNoteOutboxRetryBackoff()
{
    g_note_outbox_retry_request_id.clear();
    g_note_outbox_retry_not_before_ms = 0;
    g_note_outbox_retry_attempts = 0;
    g_note_outbox_retry_cause = OutboxRetryCause::kNone;
}

bool NoteOutboxRetryDeferred(
    const std::string& request_id,
    int64_t now_ms)
{
    if (g_note_outbox_retry_request_id != request_id) {
        ResetNoteOutboxRetryBackoff();
        return false;
    }
    return g_note_outbox_retry_not_before_ms > now_ms;
}

void ScheduleNoteOutboxRetry(
    const std::string& request_id,
    uint32_t server_retry_after_ms,
    OutboxRetryCause cause)
{
    if (g_note_outbox_retry_request_id != request_id) {
        ResetNoteOutboxRetryBackoff();
        g_note_outbox_retry_request_id = request_id;
    }
    const uint8_t shift =
        std::min(g_note_outbox_retry_attempts, kWordOutboxRetryMaxShift);
    const uint32_t local_delay_ms = std::min(
        kWordOutboxRetryBaseMs << shift,
        kWordOutboxRetryMaxMs);
    const uint32_t requested_delay_ms =
        std::min(server_retry_after_ms, kWordOutboxRetryMaxMs);
    const uint32_t base_delay_ms =
        std::max(local_delay_ms, requested_delay_ms);
    const uint32_t available_jitter_ms =
        kWordOutboxRetryMaxMs - base_delay_ms;
    const uint32_t jitter_ms = esp_random() %
        (std::min(available_jitter_ms, kWordOutboxRetryJitterMaxMs) + 1);
    const uint32_t delay_ms = base_delay_ms + jitter_ms;
    if (g_note_outbox_retry_attempts < std::numeric_limits<uint8_t>::max()) {
        ++g_note_outbox_retry_attempts;
    }
    g_note_outbox_retry_not_before_ms =
        esp_timer_get_time() / 1000 + static_cast<int64_t>(delay_ms);
    g_note_outbox_retry_cause = cause;
    ESP_LOGW(
        kTag,
        "note outbox retry scheduled: request=%s cause=%s attempt=%u retry_after_ms=%lu",
        request_id.c_str(),
        OutboxRetryCauseName(cause),
        static_cast<unsigned>(g_note_outbox_retry_attempts),
        static_cast<unsigned long>(delay_ms));
}

TickType_t NoteOutboxRetryWaitDelay()
{
    if (g_note_outbox_retry_request_id.empty()) {
        return portMAX_DELAY;
    }
    const int64_t remaining_ms =
        g_note_outbox_retry_not_before_ms - esp_timer_get_time() / 1000;
    if (remaining_ms <= 0) {
        return 0;
    }
    return pdMS_TO_TICKS(
        static_cast<uint32_t>(
            std::min<int64_t>(remaining_ms, kWordOutboxRetryMaxMs)));
}

void ResetProblemOutboxRetryBackoff()
{
    g_problem_outbox_retry_request_id.clear();
    g_problem_outbox_retry_not_before_ms = 0;
    g_problem_outbox_retry_attempts = 0;
    g_problem_outbox_retry_cause = OutboxRetryCause::kNone;
}

bool ProblemOutboxRetryDeferred(
    const std::string& request_id,
    int64_t now_ms)
{
    if (g_problem_outbox_retry_request_id != request_id) {
        ResetProblemOutboxRetryBackoff();
        return false;
    }
    return g_problem_outbox_retry_not_before_ms > now_ms;
}

void ScheduleProblemOutboxRetry(
    const std::string& request_id,
    uint32_t server_retry_after_ms,
    OutboxRetryCause cause)
{
    if (g_problem_outbox_retry_request_id != request_id) {
        ResetProblemOutboxRetryBackoff();
        g_problem_outbox_retry_request_id = request_id;
    }
    const uint8_t shift =
        std::min(g_problem_outbox_retry_attempts, kWordOutboxRetryMaxShift);
    const uint32_t local_delay_ms = std::min(
        kWordOutboxRetryBaseMs << shift,
        kWordOutboxRetryMaxMs);
    const uint32_t requested_delay_ms =
        std::min(server_retry_after_ms, kWordOutboxRetryMaxMs);
    const uint32_t base_delay_ms =
        std::max(local_delay_ms, requested_delay_ms);
    const uint32_t available_jitter_ms =
        kWordOutboxRetryMaxMs - base_delay_ms;
    const uint32_t jitter_ms = esp_random() %
        (std::min(available_jitter_ms, kWordOutboxRetryJitterMaxMs) + 1);
    const uint32_t delay_ms = base_delay_ms + jitter_ms;
    if (g_problem_outbox_retry_attempts < std::numeric_limits<uint8_t>::max()) {
        ++g_problem_outbox_retry_attempts;
    }
    g_problem_outbox_retry_not_before_ms =
        esp_timer_get_time() / 1000 + static_cast<int64_t>(delay_ms);
    g_problem_outbox_retry_cause = cause;
    ESP_LOGW(
        kTag,
        "problem outbox retry scheduled: request=%s cause=%s attempt=%u retry_after_ms=%lu",
        request_id.c_str(),
        OutboxRetryCauseName(cause),
        static_cast<unsigned>(g_problem_outbox_retry_attempts),
        static_cast<unsigned long>(delay_ms));
}

TickType_t ProblemOutboxRetryWaitDelay()
{
    if (g_problem_outbox_retry_request_id.empty()) {
        return portMAX_DELAY;
    }
    const int64_t remaining_ms =
        g_problem_outbox_retry_not_before_ms - esp_timer_get_time() / 1000;
    if (remaining_ms <= 0) {
        return 0;
    }
    return pdMS_TO_TICKS(
        static_cast<uint32_t>(
            std::min<int64_t>(remaining_ms, kWordOutboxRetryMaxMs)));
}

void ResumeTransportDeferredOutboxes()
{
    bool resumed = false;
    if (!g_word_outbox_retry_request_id.empty() &&
        g_word_outbox_retry_cause == OutboxRetryCause::kTransport) {
        g_word_outbox_retry_not_before_ms = 0;
        resumed = true;
    }
    if (!g_note_outbox_retry_request_id.empty() &&
        g_note_outbox_retry_cause == OutboxRetryCause::kTransport) {
        g_note_outbox_retry_not_before_ms = 0;
        resumed = true;
    }
    if (!g_problem_outbox_retry_request_id.empty() &&
        g_problem_outbox_retry_cause == OutboxRetryCause::kTransport) {
        g_problem_outbox_retry_not_before_ms = 0;
        resumed = true;
    }
    if (!resumed) {
        return;
    }
    ESP_LOGI(kTag, "connectivity restored; resuming transport-deferred outboxes");
    g_outbox_immediate_requested.store(true, std::memory_order_relaxed);
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
}

uint32_t AddClaimJitter(uint32_t base_ms)
{
    const uint32_t jitter = esp_random() % (kClaimPollJitterMaxMs + 1);
    return base_ms > std::numeric_limits<uint32_t>::max() - jitter
        ? std::numeric_limits<uint32_t>::max()
        : base_ms + jitter;
}

uint32_t ClaimPollDelayMs()
{
    return AddClaimJitter(
        std::max(g_claim_poll_interval_ms, kClaimPollFloorMs));
}

uint32_t NextClaimRetryDelayMs(uint32_t server_retry_after_ms)
{
    const uint8_t shift =
        std::min(g_claim_retry_attempts, kClaimRetryMaxShift);
    const uint32_t local_backoff_ms = std::min(
        kClaimRetryBaseMs << shift,
        kClaimRetryMaxMs);
    if (g_claim_retry_attempts < std::numeric_limits<uint8_t>::max()) {
        ++g_claim_retry_attempts;
    }
    return AddClaimJitter(
        std::max(server_retry_after_ms, local_backoff_ms));
}

void ResetClaimRetryBackoff()
{
    g_claim_retry_attempts = 0;
}

esp_err_t EnsureControlStateLoaded()
{
    if (g_control_state_loaded) {
        return ESP_OK;
    }
    wqn::DeviceControlState state;
    ESP_RETURN_ON_ERROR(
        wqn::LoadDeviceControlState(&state),
        kTag,
        "load v3 control checkpoint");
    g_config_revision = state.config_revision;
    g_sync_cursor = state.sync_cursor;
    g_control_state_loaded = true;
    return ESP_OK;
}

esp_err_t EnsureSyncJournalLoaded()
{
    if (g_sync_journal_loaded) {
        return ESP_OK;
    }
    esp_err_t result = wqn::LoadSyncJournal(&g_sync_journal);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "sync journal invalid: %s", esp_err_to_name(result));
        return result;
    }
    bool recovered_interrupted_install = false;
    const auto recover = [&](wqn::SyncJournalContentState* state) {
        if (state->phase == wqn::SyncJournalPhase::kFetching ||
            state->phase == wqn::SyncJournalPhase::kInstalling) {
            state->phase = wqn::SyncJournalPhase::kPending;
            recovered_interrupted_install = true;
        }
    };
    recover(&g_sync_journal.word_packs);
    recover(&g_sync_journal.note_packs);
    recover(&g_sync_journal.problem_packs);
    const auto publish = [](const wqn::SyncJournalContentState& source,
                            wqn::services::SyncContentSnapshot* target) {
        target->desired_revision = source.desired_revision;
        target->applied_revision = source.applied_revision;
        target->phase = static_cast<wqn::services::SyncContentPhase>(source.phase);
        target->retry_attempt = source.retry_attempt;
        std::snprintf(target->snapshot_id, sizeof(target->snapshot_id), "%s",
                      source.desired_snapshot_id);
    };
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    publish(g_sync_journal.word_packs, &g_sync_snapshot.word_packs);
    publish(g_sync_journal.note_packs, &g_sync_snapshot.note_packs);
    publish(g_sync_journal.problem_packs, &g_sync_snapshot.problem_packs);
    if (g_sync_snapshot.word_packs.desired_revision >
            g_sync_snapshot.word_packs.applied_revision ||
        g_sync_snapshot.note_packs.desired_revision >
            g_sync_snapshot.note_packs.applied_revision ||
        g_sync_snapshot.problem_packs.desired_revision >
            g_sync_snapshot.problem_packs.applied_revision) {
        g_sync_snapshot.state_sequence = 1;
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    g_sync_journal_loaded = true;
    if (recovered_interrupted_install) {
        ESP_LOGW(kTag, "interrupted content install recovered as pending");
        ESP_RETURN_ON_ERROR(
            PersistLatestSyncJournal(), kTag, "persist recovered sync journal");
    }
    return ESP_OK;
}

std::string RandomControlId(const char* prefix)
{
    char value[48] = {};
    std::snprintf(
        value,
        sizeof(value),
        "%s%08lx%08lx%08lx%08lx",
        prefix,
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()));
    return value;
}

const std::string& ControlBootId()
{
    static const std::string boot_id = RandomControlId("boot_");
    return boot_id;
}

wqn::protocol::v3::RequestMetadata MakeControlMetadata()
{
    wqn::protocol::v3::RequestMetadata metadata;
    metadata.request_id = RandomControlId("req_");
    metadata.boot_id = ControlBootId();
    metadata.firmware_version = WQN_FIRMWARE_VERSION;
    metadata.config_revision = g_config_revision;
    metadata.sync_cursor = g_sync_cursor;
    metadata.limit = WQN_SYNC_LIMIT;
    return metadata;
}

std::string DeviceHardwareId()
{
    std::array<uint8_t, 6> mac = {};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
        return {};
    }
    char hardware_id[18] = {};
    std::snprintf(
        hardware_id,
        sizeof(hardware_id),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);
    return hardware_id;
}

void PublishClaimCode(const wqn::protocol::v3::ClaimStartData& claim)
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    std::snprintf(
        g_sync_snapshot.claim_code,
        sizeof(g_sync_snapshot.claim_code),
        "%s",
        claim.display_code.c_str());
    g_sync_snapshot.claim_expires_at_ms = claim.expires_at_ms;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

void ResetClaimSession()
{
    g_claim_key_pair.Clear();
    g_claim_start_request_id.clear();
    g_claim_id.clear();
    g_claim_poll_interval_ms = kClaimPollFloorMs;
    ResetClaimRetryBackoff();
    g_claim_active = false;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_snapshot.claim_code[0] = '\0';
    g_sync_snapshot.claim_expires_at_ms = 0;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

esp_err_t StartClaimSession()
{
    if (!g_claim_key_pair.valid()) {
        ESP_RETURN_ON_ERROR(
            wqn::protocol::v3::GenerateClaimKeyPair(&g_claim_key_pair),
            kTag,
            "generate claim key");
        g_claim_start_request_id = RandomControlId("req_claim_start_");
    }

    const std::string hardware_id = DeviceHardwareId();
    if (hardware_id.empty()) {
        return ESP_FAIL;
    }
    wqn::protocol::v3::RequestMetadata metadata = MakeControlMetadata();
    metadata.request_id = g_claim_start_request_id;
    wqn::protocol::v3::ClaimStartData claim;
    wqn::protocol::v3::Error error;
    const esp_err_t result = wqn::StartDeviceClaimV3(
        metadata,
        hardware_id,
        g_claim_key_pair.public_key(),
        &claim,
        &error);
    if (result != ESP_OK) {
        if (error.code == "UPGRADE_REQUIRED") {
            g_control_protocol_blocked_this_round = true;
        }
        g_control_retry_after_ms = NextClaimRetryDelayMs(
            error.retryable ? error.retry_after_ms : 0);
        ESP_LOGW(
            kTag,
            "v3 claim start retry scheduled: delay_ms=%u code=%s",
            static_cast<unsigned>(g_control_retry_after_ms),
            error.code.empty() ? "TRANSPORT" : error.code.c_str());
        if (!error.retryable && error.code == "REQUEST_ID_REUSED") {
            ESP_LOGW(kTag, "expired claim/start id rejected; rotating ephemeral claim session");
            ResetClaimSession();
        }
        return result;
    }
    g_control_retry_after_ms = 0;
    ResetClaimRetryBackoff();

    g_claim_id = claim.claim_id;
    g_claim_poll_interval_ms =
        std::max(claim.poll_interval_ms, kClaimPollFloorMs);
    g_claim_active = true;
    PublishClaimCode(claim);
    ESP_LOGI(
        kTag,
        "v3 claim started: expires_at_ms=%llu server_poll_ms=%u effective_poll_ms=%u",
        static_cast<unsigned long long>(claim.expires_at_ms),
        static_cast<unsigned>(claim.poll_interval_ms),
        static_cast<unsigned>(g_claim_poll_interval_ms));
    return ESP_ERR_NOT_FINISHED;
}

esp_err_t PollClaimSession()
{
    wqn::protocol::v3::RequestMetadata metadata = MakeControlMetadata();
    wqn::protocol::v3::ClaimPollData poll;
    wqn::protocol::v3::Error error;
    const esp_err_t result =
        wqn::PollDeviceClaimV3(metadata, g_claim_id, &poll, &error);
    if (result != ESP_OK) {
        if (error.code == "UPGRADE_REQUIRED") {
            g_control_protocol_blocked_this_round = true;
        }
        g_control_retry_after_ms = NextClaimRetryDelayMs(
            error.retryable ? error.retry_after_ms : 0);
        ESP_LOGW(
            kTag,
            "v3 claim poll retry scheduled: delay_ms=%u code=%s",
            static_cast<unsigned>(g_control_retry_after_ms),
            error.code.empty() ? "TRANSPORT" : error.code.c_str());
        if (!error.retryable &&
            (error.code == "CLAIM_NOT_FOUND" || error.code == "CLAIM_CONSUMED")) {
            ResetClaimSession();
        }
        return result;
    }
    g_control_retry_after_ms = 0;
    ResetClaimRetryBackoff();
    if (poll.status == wqn::protocol::v3::ClaimStatus::kPending) {
        if (poll.poll_interval_ms >= 1000 && poll.poll_interval_ms <= 30000) {
            g_claim_poll_interval_ms =
                std::max(poll.poll_interval_ms, kClaimPollFloorMs);
        }
        return ESP_ERR_NOT_FINISHED;
    }
    if (poll.status == wqn::protocol::v3::ClaimStatus::kExpired) {
        ESP_LOGI(kTag, "v3 claim expired; starting a new physical approval session");
        ResetClaimSession();
        return ESP_ERR_NOT_FINISHED;
    }

    std::string device_id;
    std::string access_token;
    ESP_RETURN_ON_ERROR(
        wqn::protocol::v3::OpenSealedCredential(
            g_claim_key_pair,
            g_claim_id,
            poll.sealed_credential,
            &device_id,
            &access_token),
        kTag,
        "open sealed claim credential");
    if (!wqn::IsValidAccessToken(access_token)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_RETURN_ON_ERROR(wqn::SaveAccessToken(access_token), kTag, "save v3 access token");
    ESP_LOGI(kTag, "v3 claim approved: device_id=%s", device_id.c_str());
    access_token.assign(access_token.size(), '\0');
    ResetClaimSession();
    return ESP_OK;
}

esp_err_t RunDeviceClaimRoundV3()
{
    std::string token;
    if (LoadUsableToken(&token)) {
        ResetClaimSession();
        return ESP_OK;
    }
    g_bootstrap_complete = false;
    g_bootstrap_request_id.clear();
    g_sync_request_id.clear();
    g_sync_request_auto_interval_minutes = 0;
    g_config_revision = 0;
    g_sync_cursor = 0;
    return g_claim_active ? PollClaimSession() : StartClaimSession();
}
#endif

#if CONFIG_WQN_WIFI_STA_ENABLE

void SetSyncStatus(const char* status)
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    std::snprintf(g_sync_snapshot.status, sizeof(g_sync_snapshot.status), "%s", status == nullptr ? "" : status);
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

void SetSyncTaskRunning()
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_snapshot.task_running = true;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

void SetSyncRoundStarted(int64_t started_ms)
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_snapshot.last_started_ms = started_ms;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

void CompleteSyncRound(int64_t finished_ms, SyncRoundOutcome outcome)
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_snapshot.last_finished_ms = finished_ms;
    g_sync_snapshot.last_round_success = outcome == SyncRoundOutcome::kSucceeded;
    if (outcome == SyncRoundOutcome::kSucceeded) {
        ++g_sync_snapshot.success_count;
    } else if (outcome == SyncRoundOutcome::kPartial ||
               outcome == SyncRoundOutcome::kPartialNeedsFullRetry) {
        ++g_sync_snapshot.partial_count;
    } else {
        ++g_sync_snapshot.failure_count;
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
wqn::services::SyncOutboxPhase PublicOutboxPhase(WordOutboxUploadState state)
{
    switch (state) {
        case WordOutboxUploadState::kDrained:
            return wqn::services::SyncOutboxPhase::kDrained;
        case WordOutboxUploadState::kPending:
        case WordOutboxUploadState::kAuthenticationRequired:
            return wqn::services::SyncOutboxPhase::kPending;
        case WordOutboxUploadState::kYielded:
            return wqn::services::SyncOutboxPhase::kYielded;
        case WordOutboxUploadState::kProtocolBlocked:
        case WordOutboxUploadState::kFailed:
        default:
            return wqn::services::SyncOutboxPhase::kBlocked;
    }
}

void FillOutboxSnapshot(
    wqn::services::SyncOutboxSnapshot* snapshot,
    WordOutboxUploadState state,
    uint8_t retry_attempt,
    int64_t next_retry_ms)
{
    if (snapshot == nullptr) {
        return;
    }
    snapshot->phase = PublicOutboxPhase(state);
    snapshot->retry_attempt = retry_attempt;
    snapshot->next_retry_ms = next_retry_ms;
    const char* detail = "";
    switch (state) {
        case WordOutboxUploadState::kPending:
            detail = "retry pending";
            break;
        case WordOutboxUploadState::kYielded:
            detail = "yielded for interaction";
            break;
        case WordOutboxUploadState::kAuthenticationRequired:
            detail = "authentication recovery";
            break;
        case WordOutboxUploadState::kProtocolBlocked:
            detail = "firmware update required";
            break;
        case WordOutboxUploadState::kFailed:
            detail = "queue blocked";
            break;
        case WordOutboxUploadState::kDrained:
        default:
            break;
    }
    std::snprintf(snapshot->last_error, sizeof(snapshot->last_error), "%s", detail);
}

void PublishOutboxSnapshots()
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    FillOutboxSnapshot(
        &g_sync_snapshot.word_outbox,
        g_last_word_outbox_upload_state,
        g_word_outbox_retry_attempts,
        g_word_outbox_retry_not_before_ms);
    FillOutboxSnapshot(
        &g_sync_snapshot.note_outbox,
        g_last_note_outbox_upload_state,
        g_note_outbox_retry_attempts,
        g_note_outbox_retry_not_before_ms);
    FillOutboxSnapshot(
        &g_sync_snapshot.problem_outbox,
        g_last_problem_outbox_upload_state,
        g_problem_outbox_retry_attempts,
        g_problem_outbox_retry_not_before_ms);
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

bool AllOutboxesDrained()
{
    return g_last_word_outbox_upload_state == WordOutboxUploadState::kDrained &&
        g_last_note_outbox_upload_state == WordOutboxUploadState::kDrained &&
        g_last_problem_outbox_upload_state == WordOutboxUploadState::kDrained;
}

SyncRoundOutcome CurrentOutboxOutcome()
{
    if (g_last_word_outbox_upload_state ==
            WordOutboxUploadState::kProtocolBlocked ||
        g_last_note_outbox_upload_state ==
            WordOutboxUploadState::kProtocolBlocked ||
        g_last_problem_outbox_upload_state ==
            WordOutboxUploadState::kProtocolBlocked) {
        return SyncRoundOutcome::kProtocolBlocked;
    }
    if (AllOutboxesDrained()) {
        return SyncRoundOutcome::kSucceeded;
    }
    if (g_last_word_outbox_upload_state == WordOutboxUploadState::kFailed ||
        g_last_note_outbox_upload_state == WordOutboxUploadState::kFailed ||
        g_last_problem_outbox_upload_state == WordOutboxUploadState::kFailed ||
        g_last_word_outbox_upload_state ==
            WordOutboxUploadState::kAuthenticationRequired ||
        g_last_note_outbox_upload_state ==
            WordOutboxUploadState::kAuthenticationRequired ||
        g_last_problem_outbox_upload_state ==
            WordOutboxUploadState::kAuthenticationRequired) {
        return SyncRoundOutcome::kPartialNeedsFullRetry;
    }
    return SyncRoundOutcome::kPartial;
}
#endif

void PublishSyncEvent(
    wqn::services::SyncEventStatus status,
    int64_t finished_ms,
    wqn::services::SyncEventScope scope)
{
    wqn::services::SyncEvent event;
    event.status = status;
    event.scope = scope;
    event.finished_ms = finished_ms;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    // SyncService and the independent bulk content lane can both publish.
    // Assign the sequence under the same lock as the mailbox write so a later
    // sequence can never be overwritten by an earlier publisher.
    event.sequence = g_sync_event_sequence++;
    if (event.sequence == 0) {
        event.sequence = g_sync_event_sequence++;
    }
    std::snprintf(
        event.claim_code,
        sizeof(event.claim_code),
        "%s",
        g_sync_snapshot.claim_code);
    event.claim_expires_at_ms = g_sync_snapshot.claim_expires_at_ms;
    event.todo_revision = g_sync_snapshot.todo_revision;
    g_latest_sync_event = event;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    // The mailbox payload is complete before the sink is observed/called. A
    // task notification may coalesce, which is safe because consumers compare
    // the overwrite-safe sequence rather than expecting one wake per event.
    const wqn::services::SyncEventSink sink =
        g_sync_event_sink.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink();
    }
}

bool LoadUsableToken(std::string* token)
{
    if (token == nullptr) {
        return false;
    }

    token->clear();
    const esp_err_t result = wqn::LoadAccessToken(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "access token reload failed: %s", esp_err_to_name(result));
        return false;
    }
    if (token->empty()) {
        return false;
    }
    if (!wqn::IsValidAccessToken(*token)) {
        ESP_LOGW(kTag, "stored token invalid during sync setup; clearing");
        ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::ClearAccessToken());
        token->clear();
        return false;
    }
    return true;
}

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
esp_err_t BootstrapControlV3(const std::string& token)
{
    if (g_bootstrap_complete) {
        return ESP_OK;
    }
    if (g_bootstrap_request_id.empty()) {
        g_bootstrap_request_id = RandomControlId("req_bootstrap_");
    }
    wqn::protocol::v3::RequestMetadata metadata = MakeControlMetadata();
    metadata.request_id = g_bootstrap_request_id;
    wqn::protocol::v3::BootstrapData bootstrap;
    wqn::protocol::v3::Error error;
    const esp_err_t result = wqn::BootstrapDeviceControlV3(
        token, metadata, &bootstrap, &error);
    if (result != ESP_OK) {
        if (error.code == "UPGRADE_REQUIRED") {
            g_control_protocol_blocked_this_round = true;
        }
        if (error.retryable) {
            g_control_retry_after_ms = error.retry_after_ms;
        }
        return result;
    }
    const wqn::DeviceControlState checkpoint = {
        bootstrap.config_revision,
        bootstrap.sync_cursor,
    };
    ESP_RETURN_ON_ERROR(
        wqn::SaveDeviceControlState(checkpoint),
        kTag,
        "save v3 bootstrap checkpoint");
    g_control_retry_after_ms = 0;
    g_config_revision = checkpoint.config_revision;
    g_sync_cursor = checkpoint.sync_cursor;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_journal.config_revision = checkpoint.config_revision;
    g_sync_journal.sync_cursor = checkpoint.sync_cursor;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    ESP_RETURN_ON_ERROR(
        PersistLatestSyncJournal(),
        kTag,
        "save bootstrap sync journal");
    g_bootstrap_complete = true;
    g_bootstrap_request_id.clear();
    ESP_LOGI(
        kTag,
        "v3 bootstrap complete: config_revision=%llu sync_cursor=%llu",
        static_cast<unsigned long long>(g_config_revision),
        static_cast<unsigned long long>(g_sync_cursor));
    return ESP_OK;
}

esp_err_t SyncControlPlaneV3(const std::string& token)
{
    if (g_sync_request_id.empty()) {
        g_sync_request_id = RandomControlId("req_sync_");
        // The request id and its fingerprint are an immutable retry unit.
        // Freeze the locally-authoritative setting with the id: changing the
        // setting while a transport retry is pending must not reuse the same
        // id with a different JSON body and trigger REQUEST_ID_REUSED.
        g_sync_request_auto_interval_minutes =
            g_auto_sync_interval_minutes.load(std::memory_order_acquire);
    }
    wqn::protocol::v3::RequestMetadata metadata = MakeControlMetadata();
    metadata.request_id = g_sync_request_id;
    wqn::protocol::v3::SyncData sync;
    wqn::protocol::v3::Error error;
    const esp_err_t sync_result =
        wqn::SyncDeviceControlV3(
            token, metadata, g_sync_request_auto_interval_minutes, &sync, &error);
    if (sync_result != ESP_OK) {
        if (error.code == "UPGRADE_REQUIRED") {
            g_control_protocol_blocked_this_round = true;
        }
        if (error.retryable) {
            g_control_retry_after_ms = error.retry_after_ms;
        }
        ESP_LOGW(kTag, "v3 sync failed: %s", esp_err_to_name(sync_result));
        return sync_result;
    }
    g_control_retry_after_ms = 0;
    PublishContentTargets(sync.content_targets);
    // Device settings are local-authoritative. The server echoes the reported
    // value for protocol observability but must never overwrite the NVS value
    // selected on the device.
    if (sync.auto_sync_interval_minutes !=
        g_sync_request_auto_interval_minutes) {
        ESP_LOGW(
            kTag,
            "server auto-sync echo mismatch: reported=%u echoed=%u (ignored)",
            static_cast<unsigned>(g_sync_request_auto_interval_minutes),
            static_cast<unsigned>(sync.auto_sync_interval_minutes));
    }
    ESP_LOGI(
        kTag,
        "v3 sync summary: due=%u todos=%d words=%d cursor=%llu",
        static_cast<unsigned>(sync.due_problem_ids.size()),
        sync.todo_count,
        sync.word_due_count,
        static_cast<unsigned long long>(sync.sync_cursor));
    const wqn::DeviceControlState checkpoint = {
        sync.config_revision,
        sync.sync_cursor,
    };
    ESP_RETURN_ON_ERROR(
        wqn::SaveDeviceControlState(checkpoint),
        kTag,
        "commit v3 sync checkpoint");
    g_config_revision = checkpoint.config_revision;
    g_sync_cursor = checkpoint.sync_cursor;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_journal.config_revision = checkpoint.config_revision;
    g_sync_journal.sync_cursor = checkpoint.sync_cursor;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    ESP_RETURN_ON_ERROR(
        PersistLatestSyncJournal(),
        kTag,
        "commit sync journal checkpoint");
    g_sync_request_id.clear();
    g_sync_request_auto_interval_minutes = 0;
    ESP_LOGI(
        kTag,
        "v3 sync checkpoint committed: config_revision=%llu sync_cursor=%llu",
        static_cast<unsigned long long>(g_config_revision),
        static_cast<unsigned long long>(g_sync_cursor));
    return ESP_OK;
}
#endif

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
WordOutboxUploadState UploadPendingWordObservations(const std::string& token)
{
    constexpr size_t kMaxWordObservationsPerRound = 64;
    const uint32_t interaction_generation =
        g_word_interaction_generation.load(std::memory_order_acquire);
    size_t processed = 0;
    size_t uploaded = 0;
    size_t quarantined = 0;
    for (; processed < kMaxWordObservationsPerRound;) {
        if (g_word_interaction_generation.load(std::memory_order_acquire) !=
            interaction_generation) {
            ESP_LOGI(
                kTag,
                "word outbox batch yielded to interaction: uploaded=%u quarantined=%u",
                static_cast<unsigned>(uploaded),
                static_cast<unsigned>(quarantined));
            return WordOutboxUploadState::kYielded;
        }
        wqn::DurableWordObservation pending;
        esp_err_t result = wqn::PeekPendingWordObservation(&pending);
        if (result == ESP_ERR_NOT_FOUND) {
            ResetWordOutboxRetryBackoff();
            if (processed > 0) {
                ESP_LOGI(
                    kTag,
                    "word outbox drained: uploaded=%u quarantined=%u",
                    static_cast<unsigned>(uploaded),
                    static_cast<unsigned>(quarantined));
            }
            return WordOutboxUploadState::kDrained;
        }
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "word outbox read failed: %s", esp_err_to_name(result));
            return WordOutboxUploadState::kFailed;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (WordOutboxRetryDeferred(pending.request_id, now_ms)) {
            ESP_LOGI(
                kTag,
                "word outbox head deferred: request=%s remaining_ms=%lld",
                pending.request_id.c_str(),
                static_cast<long long>(
                    g_word_outbox_retry_not_before_ms - now_ms));
            return WordOutboxUploadState::kPending;
        }

        wqn::protocol::word_study_v1::ObservationRequest request;
        request.metadata = MakeControlMetadata();
        request.metadata.request_id = pending.request_id;
        request.session_id = pending.session_id;
        request.sequence = pending.sequence;
        request.item_id = pending.item_id;
        request.action = pending.action;
        request.mode = pending.mode;
        request.occurred_at = pending.occurred_at;
        wqn::protocol::word_study_v1::ObservationData response;
        wqn::protocol::v3::Error word_error;
        bool transport_failure = false;
        result = wqn::SubmitWordStudyObservationV1(
            token, request, &response, &word_error, &transport_failure);
        if (result != ESP_OK) {
            const OutboxFailureDisposition disposition =
                ClassifyOutboxFailure(word_error, transport_failure);
            if (disposition ==
                OutboxFailureDisposition::kAuthenticationRequired) {
                ResetWordOutboxRetryBackoff();
                ESP_LOGW(
                    kTag,
                    "word outbox paused for credential recovery: request=%s",
                    pending.request_id.c_str());
                return WordOutboxUploadState::kAuthenticationRequired;
            }
            if (disposition == OutboxFailureDisposition::kProtocolBlocked) {
                ESP_LOGE(
                    kTag,
                    "word outbox suspended: server requires newer firmware: request=%s",
                    pending.request_id.c_str());
                return WordOutboxUploadState::kProtocolBlocked;
            }
            if (disposition == OutboxFailureDisposition::kProtocolIntegrity) {
                // Audit §16.D: identity-level failures (idempotency conflict,
                // actor ownership, corrupt identity) forbid unilateral
                // deletion -- the server never consumed this sequence, so a
                // local drop would strand later same-session records in
                // STUDY_SEQUENCE_GAP forever (Case B). Park durably instead.
                ESP_LOGE(
                    kTag,
                    "word observation parked (%s): integrity failure forbids deletion: request=%s code=%s",
                    wqn::OutboxSuspendReasonName(SuspendReasonFor(word_error.code)),
                    pending.request_id.c_str(),
                    word_error.code.c_str());
                const esp_err_t suspend_result =
                    wqn::SuspendPendingWordObservation(
                        pending.request_id,
                        SuspendReasonFor(word_error.code));
                ResetWordOutboxRetryBackoff();
                if (suspend_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "word observation park failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(suspend_result));
                    ScheduleWordOutboxRetry(
                        pending.request_id, 0, OutboxRetryCause::kLocalStorage);
                    return WordOutboxUploadState::kPending;
                }
                ++processed;
                continue;
            }
            if (disposition == OutboxFailureDisposition::kSequenceResolved ||
                disposition == OutboxFailureDisposition::kSessionTerminal) {
                // The server proved this sequence was already consumed (or
                // its whole session is permanently gone): local quarantine
                // is safe and restores queue progress without a tombstone
                // round trip.
                ESP_LOGW(
                    kTag,
                    "word observation %s; quarantining head: request=%s code=%s",
                    disposition == OutboxFailureDisposition::kSessionTerminal
                        ? "session terminally closed"
                        : "sequence already resolved",
                    pending.request_id.c_str(),
                    word_error.code.c_str());
                const esp_err_t resolved_quarantine_result =
                    wqn::QuarantinePendingWordObservation(pending.request_id);
                ResetWordOutboxRetryBackoff();
                if (resolved_quarantine_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "word observation quarantine failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(resolved_quarantine_result));
                    return WordOutboxUploadState::kFailed;
                }
                ++processed;
                ++quarantined;
                continue;
            }
            if (disposition == OutboxFailureDisposition::kTombstoneRecoverable) {
                ESP_LOGW(
                    kTag,
                    "terminal word observation rejected; advancing sequence via tombstone: request=%s sequence=%llu code=%s",
                    pending.request_id.c_str(),
                    static_cast<unsigned long long>(pending.sequence),
                    word_error.code.c_str());

                // A local quarantine removes the record from the device, but
                // the server's monotonic next_sequence must advance too.
                // Otherwise the following record is rejected forever with
                // STUDY_SEQUENCE_GAP. The skip endpoint writes a durable,
                // non-projecting tombstone for this sequence.
                wqn::protocol::word_study_v1::ObservationData skip_response;
                wqn::protocol::v3::Error skip_error;
                bool skip_transport_failure = false;
                const esp_err_t skip_result =
                    wqn::SkipWordStudyObservationV1(
                        token,
                        request,
                        &skip_response,
                        &skip_error,
                        &skip_transport_failure);
                const OutboxFailureDisposition skip_disposition =
                    ClassifyOutboxFailure(skip_error, skip_transport_failure);
                const bool sequence_consumed =
                    skip_result == ESP_OK ||
                    skip_disposition ==
                        OutboxFailureDisposition::kSequenceResolved ||
                    skip_disposition ==
                        OutboxFailureDisposition::kSessionTerminal;
                if (!sequence_consumed) {
                    if (skip_disposition ==
                        OutboxFailureDisposition::kAuthenticationRequired) {
                        ResetWordOutboxRetryBackoff();
                        return WordOutboxUploadState::kAuthenticationRequired;
                    }
                    if (skip_disposition ==
                        OutboxFailureDisposition::kProtocolBlocked) {
                        ESP_LOGE(
                            kTag,
                            "word outbox suspended: server requires newer firmware: request=%s",
                            pending.request_id.c_str());
                        return WordOutboxUploadState::kProtocolBlocked;
                    }
                    if (skip_disposition ==
                        OutboxFailureDisposition::kProtocolIntegrity) {
                        // [gap-1] The tombstone itself was rejected at the
                        // identity level: the sequence can never be consumed
                        // by this device. Park the head durably so the queue
                        // advances without deleting evidence.
                        ESP_LOGE(
                            kTag,
                            "word observation parked (%s): tombstone rejected at identity level: request=%s code=%s",
                            wqn::OutboxSuspendReasonName(
                                SuspendReasonFor(skip_error.code)),
                            pending.request_id.c_str(),
                            skip_error.code.c_str());
                        const esp_err_t suspend_result =
                            wqn::SuspendPendingWordObservation(
                                pending.request_id,
                                SuspendReasonFor(skip_error.code));
                        ResetWordOutboxRetryBackoff();
                        if (suspend_result != ESP_OK) {
                            ESP_LOGE(
                                kTag,
                                "word observation park failed: request=%s error=%s",
                                pending.request_id.c_str(),
                                esp_err_to_name(suspend_result));
                            ScheduleWordOutboxRetry(
                                pending.request_id,
                                0,
                                OutboxRetryCause::kLocalStorage);
                            return WordOutboxUploadState::kPending;
                        }
                        ++processed;
                        continue;
                    }
                    // Transport/server backoff, or a tombstone-recoverable
                    // code on the tombstone endpoint itself (post-relaxation
                    // drift): bounded exponential retry keeps attempting.
                    ESP_LOGW(
                        kTag,
                        "word observation skip deferred: request=%s sequence=%llu code=%s error=%s",
                        pending.request_id.c_str(),
                        static_cast<unsigned long long>(pending.sequence),
                        skip_error.code.empty() ? "TRANSPORT" : skip_error.code.c_str(),
                        esp_err_to_name(skip_result));
                    ScheduleWordOutboxRetry(
                        pending.request_id,
                        skip_error.retryable ? skip_error.retry_after_ms : 0,
                        RetryCauseFor(skip_disposition));
                    return WordOutboxUploadState::kPending;
                }
                const esp_err_t quarantine_result =
                    wqn::QuarantinePendingWordObservation(pending.request_id);
                ResetWordOutboxRetryBackoff();
                if (quarantine_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "word observation quarantine failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(quarantine_result));
                    return WordOutboxUploadState::kFailed;
                }
                ++processed;
                ++quarantined;
                continue;
            }
            ESP_LOGW(
                kTag,
                "word outbox upload deferred: request=%s sequence=%llu code=%s error=%s",
                pending.request_id.c_str(),
                static_cast<unsigned long long>(pending.sequence),
                word_error.code.empty() ? "TRANSPORT" : word_error.code.c_str(),
                esp_err_to_name(result));
            ScheduleWordOutboxRetry(
                pending.request_id,
                word_error.retryable ? word_error.retry_after_ms : 0,
                RetryCauseFor(disposition));
            return WordOutboxUploadState::kPending;
        }
        result = wqn::AcknowledgeWordObservation(pending.request_id);
        if (result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "word outbox ack failed: request=%s error=%s",
                pending.request_id.c_str(),
                esp_err_to_name(result));
            ScheduleWordOutboxRetry(
                pending.request_id, 0, OutboxRetryCause::kLocalStorage);
            return WordOutboxUploadState::kPending;
        }
        ResetWordOutboxRetryBackoff();
        ++processed;
        ++uploaded;
    }

    wqn::DurableWordObservation remaining;
    const esp_err_t remaining_result =
        wqn::PeekPendingWordObservation(&remaining);
    if (processed > 0) {
        ESP_LOGI(
            kTag,
            "word outbox batch complete: uploaded=%u quarantined=%u pending=%d",
            static_cast<unsigned>(uploaded),
            static_cast<unsigned>(quarantined),
            remaining_result == ESP_OK ? 1 : 0);
    }
    if (remaining_result == ESP_ERR_NOT_FOUND) {
        return WordOutboxUploadState::kDrained;
    }
    return remaining_result == ESP_OK
        ? WordOutboxUploadState::kPending
        : WordOutboxUploadState::kFailed;
}

// Uploads pending problem verdicts (32-record batches, interaction-yield,
// same head-retry backoff as note). Each verdict is a standalone idempotent
// observation: a terminal server rejection quarantines it directly -- there
// is no session sequence to advance first.
WordOutboxUploadState UploadPendingProblemObservations(const std::string& token)
{
    constexpr size_t kMaxProblemObservationsPerRound = 32;
    const uint32_t interaction_generation =
        g_word_interaction_generation.load(std::memory_order_acquire);
    size_t processed = 0;
    size_t uploaded = 0;
    size_t quarantined = 0;
    for (; processed < kMaxProblemObservationsPerRound;) {
        if (g_word_interaction_generation.load(std::memory_order_acquire) !=
            interaction_generation) {
            ESP_LOGI(
                kTag,
                "problem outbox batch yielded to interaction: uploaded=%u quarantined=%u",
                static_cast<unsigned>(uploaded),
                static_cast<unsigned>(quarantined));
            return WordOutboxUploadState::kYielded;
        }
        wqn::DurableProblemObservation pending;
        esp_err_t result = wqn::PeekPendingProblemObservation(&pending);
        if (result == ESP_ERR_NOT_FOUND) {
            ResetProblemOutboxRetryBackoff();
            if (processed > 0) {
                ESP_LOGI(
                    kTag,
                    "problem outbox drained: uploaded=%u quarantined=%u",
                    static_cast<unsigned>(uploaded),
                    static_cast<unsigned>(quarantined));
            }
            return WordOutboxUploadState::kDrained;
        }
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "problem outbox read failed: %s", esp_err_to_name(result));
            return WordOutboxUploadState::kFailed;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (ProblemOutboxRetryDeferred(pending.request_id, now_ms)) {
            ESP_LOGI(
                kTag,
                "problem outbox head deferred: request=%s remaining_ms=%lld",
                pending.request_id.c_str(),
                static_cast<long long>(
                    g_problem_outbox_retry_not_before_ms - now_ms));
            return WordOutboxUploadState::kPending;
        }

        wqn::protocol::problem_study_v1::ObservationRequest request;
        request.metadata = MakeControlMetadata();
        request.metadata.request_id = pending.request_id;
        request.problem_id = pending.problem_id;
        request.action = pending.action;
        request.occurred_at = pending.occurred_at;
        wqn::protocol::problem_study_v1::ObservationData response;
        wqn::protocol::v3::Error problem_error;
        bool transport_failure = false;
        result = wqn::SubmitProblemReviewObservationV1(
            token, request, &response, &problem_error, &transport_failure);
        if (result != ESP_OK) {
            const OutboxFailureDisposition disposition =
                ClassifyOutboxFailure(problem_error, transport_failure);
            if (disposition ==
                OutboxFailureDisposition::kAuthenticationRequired) {
                ResetProblemOutboxRetryBackoff();
                ESP_LOGW(
                    kTag,
                    "problem outbox paused for credential recovery: request=%s",
                    pending.request_id.c_str());
                return WordOutboxUploadState::kAuthenticationRequired;
            }
            if (disposition == OutboxFailureDisposition::kProtocolBlocked) {
                ESP_LOGE(
                    kTag,
                    "problem outbox suspended: server requires newer firmware: request=%s",
                    pending.request_id.c_str());
                return WordOutboxUploadState::kProtocolBlocked;
            }
            if (disposition == OutboxFailureDisposition::kProtocolIntegrity) {
                // Verdicts have no session sequence, so a park here cannot
                // wedge successors -- but an idempotency conflict still
                // forbids unilateral deletion (audit §16.D): the server may
                // hold a different payload under this key.
                ESP_LOGE(
                    kTag,
                    "problem verdict parked (%s): integrity failure forbids deletion: request=%s code=%s",
                    wqn::OutboxSuspendReasonName(SuspendReasonFor(problem_error.code)),
                    pending.request_id.c_str(),
                    problem_error.code.c_str());
                const esp_err_t suspend_result =
                    wqn::SuspendPendingProblemObservation(
                        pending.request_id,
                        SuspendReasonFor(problem_error.code));
                ResetProblemOutboxRetryBackoff();
                if (suspend_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "problem verdict park failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(suspend_result));
                    ScheduleProblemOutboxRetry(
                        pending.request_id, 0, OutboxRetryCause::kLocalStorage);
                    return WordOutboxUploadState::kPending;
                }
                ++processed;
                continue;
            }
            if (disposition ==
                    OutboxFailureDisposition::kTombstoneRecoverable ||
                disposition ==
                    OutboxFailureDisposition::kSequenceResolved ||
                disposition ==
                    OutboxFailureDisposition::kSessionTerminal) {
                // Standalone idempotent observation: business-level terminal
                // rejections (not visible / malformed / already applied)
                // cannot wedge any successor, so quarantine restores queue
                // progress without losing the forensic copy. Unrecognized
                // future terminal codes classify as protocol integrity and
                // park above instead.
                ESP_LOGW(
                    kTag,
                    "terminal problem verdict rejected; quarantining: request=%s code=%s",
                    pending.request_id.c_str(),
                    problem_error.code.c_str());
                const esp_err_t quarantine_result =
                    wqn::QuarantinePendingProblemObservation(pending.request_id);
                ResetProblemOutboxRetryBackoff();
                if (quarantine_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "problem verdict quarantine failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(quarantine_result));
                    return WordOutboxUploadState::kFailed;
                }
                ++processed;
                ++quarantined;
                continue;
            }
            ESP_LOGW(
                kTag,
                "problem outbox upload deferred: request=%s code=%s error=%s",
                pending.request_id.c_str(),
                problem_error.code.empty() ? "TRANSPORT" : problem_error.code.c_str(),
                esp_err_to_name(result));
            ScheduleProblemOutboxRetry(
                pending.request_id,
                problem_error.retryable ? problem_error.retry_after_ms : 0,
                RetryCauseFor(disposition));
            return WordOutboxUploadState::kPending;
        }
        result = wqn::AcknowledgeProblemObservation(pending.request_id);
        if (result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "problem outbox ack failed: request=%s error=%s",
                pending.request_id.c_str(),
                esp_err_to_name(result));
            ScheduleProblemOutboxRetry(
                pending.request_id, 0, OutboxRetryCause::kLocalStorage);
            return WordOutboxUploadState::kPending;
        }
        ResetProblemOutboxRetryBackoff();
        ++processed;
        ++uploaded;
    }

    wqn::DurableProblemObservation remaining;
    const esp_err_t remaining_result =
        wqn::PeekPendingProblemObservation(&remaining);
    if (processed > 0) {
        ESP_LOGI(
            kTag,
            "problem outbox batch complete: uploaded=%u quarantined=%u pending=%d",
            static_cast<unsigned>(uploaded),
            static_cast<unsigned>(quarantined),
            remaining_result == ESP_OK ? 1 : 0);
    }
    if (remaining_result == ESP_ERR_NOT_FOUND) {
        return WordOutboxUploadState::kDrained;
    }
    return remaining_result == ESP_OK
        ? WordOutboxUploadState::kPending
        : WordOutboxUploadState::kFailed;
}

WordOutboxUploadState UploadPendingNoteObservations(const std::string& token)
{
    constexpr size_t kMaxNoteObservationsPerRound = 64;
    const uint32_t interaction_generation =
        g_word_interaction_generation.load(std::memory_order_acquire);
    size_t processed = 0;
    size_t uploaded = 0;
    size_t quarantined = 0;
    for (; processed < kMaxNoteObservationsPerRound;) {
        if (g_word_interaction_generation.load(std::memory_order_acquire) !=
            interaction_generation) {
            ESP_LOGI(
                kTag,
                "note outbox batch yielded to interaction: uploaded=%u quarantined=%u",
                static_cast<unsigned>(uploaded),
                static_cast<unsigned>(quarantined));
            return WordOutboxUploadState::kYielded;
        }
        wqn::DurableNoteObservation pending;
        esp_err_t result = wqn::PeekPendingNoteObservation(&pending);
        if (result == ESP_ERR_NOT_FOUND) {
            ResetNoteOutboxRetryBackoff();
            if (processed > 0) {
                ESP_LOGI(
                    kTag,
                    "note outbox drained: uploaded=%u quarantined=%u",
                    static_cast<unsigned>(uploaded),
                    static_cast<unsigned>(quarantined));
            }
            return WordOutboxUploadState::kDrained;
        }
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "note outbox read failed: %s", esp_err_to_name(result));
            return WordOutboxUploadState::kFailed;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (NoteOutboxRetryDeferred(pending.request_id, now_ms)) {
            ESP_LOGI(
                kTag,
                "note outbox head deferred: request=%s remaining_ms=%lld",
                pending.request_id.c_str(),
                static_cast<long long>(
                    g_note_outbox_retry_not_before_ms - now_ms));
            return WordOutboxUploadState::kPending;
        }

        wqn::protocol::note_study_v1::ObservationRequest request;
        request.metadata = MakeControlMetadata();
        request.metadata.request_id = pending.request_id;
        request.session_id = pending.session_id;
        request.sequence = pending.sequence;
        request.item_id = pending.item_id;
        request.action = pending.action;
        request.mode = pending.mode;
        request.occurred_at = pending.occurred_at;
        wqn::protocol::note_study_v1::ObservationData response;
        wqn::protocol::v3::Error note_error;
        bool transport_failure = false;
        result = wqn::SubmitNoteStudyObservationV1(
            token, request, &response, &note_error, &transport_failure);
        if (result != ESP_OK) {
            const OutboxFailureDisposition disposition =
                ClassifyOutboxFailure(note_error, transport_failure);
            if (disposition ==
                OutboxFailureDisposition::kAuthenticationRequired) {
                ResetNoteOutboxRetryBackoff();
                ESP_LOGW(
                    kTag,
                    "note outbox paused for credential recovery: request=%s",
                    pending.request_id.c_str());
                return WordOutboxUploadState::kAuthenticationRequired;
            }
            if (disposition == OutboxFailureDisposition::kProtocolBlocked) {
                ESP_LOGE(
                    kTag,
                    "note outbox suspended: server requires newer firmware: request=%s",
                    pending.request_id.c_str());
                return WordOutboxUploadState::kProtocolBlocked;
            }
            if (disposition == OutboxFailureDisposition::kProtocolIntegrity) {
                // Audit §16.D / Case B mirror of the word domain.
                ESP_LOGE(
                    kTag,
                    "note observation parked (%s): integrity failure forbids deletion: request=%s code=%s",
                    wqn::OutboxSuspendReasonName(SuspendReasonFor(note_error.code)),
                    pending.request_id.c_str(),
                    note_error.code.c_str());
                const esp_err_t suspend_result =
                    wqn::SuspendPendingNoteObservation(
                        pending.request_id,
                        SuspendReasonFor(note_error.code));
                ResetNoteOutboxRetryBackoff();
                if (suspend_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "note observation park failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(suspend_result));
                    ScheduleNoteOutboxRetry(
                        pending.request_id, 0, OutboxRetryCause::kLocalStorage);
                    return WordOutboxUploadState::kPending;
                }
                ++processed;
                continue;
            }
            if (disposition == OutboxFailureDisposition::kSequenceResolved ||
                disposition == OutboxFailureDisposition::kSessionTerminal) {
                ESP_LOGW(
                    kTag,
                    "note observation %s; quarantining head: request=%s code=%s",
                    disposition == OutboxFailureDisposition::kSessionTerminal
                        ? "session terminally closed"
                        : "sequence already resolved",
                    pending.request_id.c_str(),
                    note_error.code.c_str());
                const esp_err_t resolved_quarantine_result =
                    wqn::QuarantinePendingNoteObservation(pending.request_id);
                ResetNoteOutboxRetryBackoff();
                if (resolved_quarantine_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "note observation quarantine failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(resolved_quarantine_result));
                    return WordOutboxUploadState::kFailed;
                }
                ++processed;
                ++quarantined;
                continue;
            }
            if (disposition == OutboxFailureDisposition::kTombstoneRecoverable) {
                ESP_LOGW(
                    kTag,
                    "terminal note observation rejected; advancing sequence via tombstone: request=%s sequence=%llu code=%s",
                    pending.request_id.c_str(),
                    static_cast<unsigned long long>(pending.sequence),
                    note_error.code.c_str());

                // Mirror word: a local quarantine removes the record, but the
                // server's monotonic next_sequence must advance too or the next
                // record is rejected forever with STUDY_SEQUENCE_GAP. The skip
                // endpoint writes a durable, non-projecting tombstone.
                wqn::protocol::note_study_v1::ObservationData skip_response;
                wqn::protocol::v3::Error skip_error;
                bool skip_transport_failure = false;
                const esp_err_t skip_result =
                    wqn::SkipNoteStudyObservationV1(
                        token,
                        request,
                        &skip_response,
                        &skip_error,
                        &skip_transport_failure);
                const OutboxFailureDisposition skip_disposition =
                    ClassifyOutboxFailure(skip_error, skip_transport_failure);
                const bool sequence_consumed =
                    skip_result == ESP_OK ||
                    skip_disposition ==
                        OutboxFailureDisposition::kSequenceResolved ||
                    skip_disposition ==
                        OutboxFailureDisposition::kSessionTerminal;
                if (!sequence_consumed) {
                    if (skip_disposition ==
                        OutboxFailureDisposition::kAuthenticationRequired) {
                        ResetNoteOutboxRetryBackoff();
                        return WordOutboxUploadState::kAuthenticationRequired;
                    }
                    if (skip_disposition ==
                        OutboxFailureDisposition::kProtocolBlocked) {
                        ESP_LOGE(
                            kTag,
                            "note outbox suspended: server requires newer firmware: request=%s",
                            pending.request_id.c_str());
                        return WordOutboxUploadState::kProtocolBlocked;
                    }
                    if (skip_disposition ==
                        OutboxFailureDisposition::kProtocolIntegrity) {
                        // [gap-1] Tombstone rejected at identity level: park
                        // the head durably instead of deleting evidence.
                        ESP_LOGE(
                            kTag,
                            "note observation parked (%s): tombstone rejected at identity level: request=%s code=%s",
                            wqn::OutboxSuspendReasonName(
                                SuspendReasonFor(skip_error.code)),
                            pending.request_id.c_str(),
                            skip_error.code.c_str());
                        const esp_err_t suspend_result =
                            wqn::SuspendPendingNoteObservation(
                                pending.request_id,
                                SuspendReasonFor(skip_error.code));
                        ResetNoteOutboxRetryBackoff();
                        if (suspend_result != ESP_OK) {
                            ESP_LOGE(
                                kTag,
                                "note observation park failed: request=%s error=%s",
                                pending.request_id.c_str(),
                                esp_err_to_name(suspend_result));
                            ScheduleNoteOutboxRetry(
                                pending.request_id,
                                0,
                                OutboxRetryCause::kLocalStorage);
                            return WordOutboxUploadState::kPending;
                        }
                        ++processed;
                        continue;
                    }
                    // Transport/server backoff or post-relaxation drift on
                    // the tombstone endpoint: bounded retry.
                    ESP_LOGW(
                        kTag,
                        "note observation skip deferred: request=%s sequence=%llu code=%s error=%s",
                        pending.request_id.c_str(),
                        static_cast<unsigned long long>(pending.sequence),
                        skip_error.code.empty() ? "TRANSPORT" : skip_error.code.c_str(),
                        esp_err_to_name(skip_result));
                    ScheduleNoteOutboxRetry(
                        pending.request_id,
                        skip_error.retryable ? skip_error.retry_after_ms : 0,
                        RetryCauseFor(skip_disposition));
                    return WordOutboxUploadState::kPending;
                }
                const esp_err_t quarantine_result =
                    wqn::QuarantinePendingNoteObservation(pending.request_id);
                ResetNoteOutboxRetryBackoff();
                if (quarantine_result != ESP_OK) {
                    ESP_LOGE(
                        kTag,
                        "note observation quarantine failed: request=%s error=%s",
                        pending.request_id.c_str(),
                        esp_err_to_name(quarantine_result));
                    return WordOutboxUploadState::kFailed;
                }
                ++processed;
                ++quarantined;
                continue;
            }
            ESP_LOGW(
                kTag,
                "note outbox upload deferred: request=%s sequence=%llu code=%s error=%s",
                pending.request_id.c_str(),
                static_cast<unsigned long long>(pending.sequence),
                note_error.code.empty() ? "TRANSPORT" : note_error.code.c_str(),
                esp_err_to_name(result));
            ScheduleNoteOutboxRetry(
                pending.request_id,
                note_error.retryable ? note_error.retry_after_ms : 0,
                RetryCauseFor(disposition));
            return WordOutboxUploadState::kPending;
        }
        result = wqn::AcknowledgeNoteObservation(pending.request_id);
        if (result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "note outbox ack failed: request=%s error=%s",
                pending.request_id.c_str(),
                esp_err_to_name(result));
            ScheduleNoteOutboxRetry(
                pending.request_id, 0, OutboxRetryCause::kLocalStorage);
            return WordOutboxUploadState::kPending;
        }
        ResetNoteOutboxRetryBackoff();
        ++processed;
        ++uploaded;
    }

    wqn::DurableNoteObservation remaining;
    const esp_err_t remaining_result =
        wqn::PeekPendingNoteObservation(&remaining);
    if (processed > 0) {
        ESP_LOGI(
            kTag,
            "note outbox batch complete: uploaded=%u quarantined=%u pending=%d",
            static_cast<unsigned>(uploaded),
            static_cast<unsigned>(quarantined),
            remaining_result == ESP_OK ? 1 : 0);
    }
    if (remaining_result == ESP_ERR_NOT_FOUND) {
        return WordOutboxUploadState::kDrained;
    }
    return remaining_result == ESP_OK
        ? WordOutboxUploadState::kPending
        : WordOutboxUploadState::kFailed;
}

SyncRoundOutcome RunWordOutboxOnlyRound()
{
    std::string token;
    if (!LoadUsableToken(&token)) {
        // Pairing/bootstrap owns identity recovery. Escalate the next round
        // instead of making the outbox-only path imitate the control plane.
        // Mark the upload pass drained-for-now so the generic outbox re-arm
        // does not create an immediate no-token loop that bypasses claim
        // polling backoff. The durable heads remain on flash; the escalated
        // full round uploads them after identity recovery (and boot probes
        // re-arm them if the device sleeps/restarts first).
        g_full_sync_reasons.fetch_or(
            kFullSyncCredentials, std::memory_order_release);
        g_last_word_outbox_upload_state = WordOutboxUploadState::kDrained;
        g_last_note_outbox_upload_state = WordOutboxUploadState::kDrained;
        g_last_problem_outbox_upload_state = WordOutboxUploadState::kDrained;
        return SyncRoundOutcome::kFailed;
    }
    g_last_word_outbox_upload_state = UploadPendingWordObservations(token);
    if (g_last_word_outbox_upload_state ==
        WordOutboxUploadState::kProtocolBlocked) {
        return CurrentOutboxOutcome();
    }
    if (g_last_word_outbox_upload_state ==
        WordOutboxUploadState::kAuthenticationRequired) {
        g_last_note_outbox_upload_state =
            WordOutboxUploadState::kAuthenticationRequired;
        g_last_problem_outbox_upload_state =
            WordOutboxUploadState::kAuthenticationRequired;
        return CurrentOutboxOutcome();
    }
    g_last_note_outbox_upload_state = UploadPendingNoteObservations(token);
    if (g_last_note_outbox_upload_state ==
        WordOutboxUploadState::kProtocolBlocked) {
        return CurrentOutboxOutcome();
    }
    if (g_last_note_outbox_upload_state ==
        WordOutboxUploadState::kAuthenticationRequired) {
        g_last_problem_outbox_upload_state =
            WordOutboxUploadState::kAuthenticationRequired;
        return CurrentOutboxOutcome();
    }
    g_last_problem_outbox_upload_state = UploadPendingProblemObservations(token);
    return CurrentOutboxOutcome();
}
#endif

SyncRoundOutcome RunSyncRound()
{
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    g_control_protocol_blocked_this_round = false;
    esp_err_t result = EnsureControlStateLoaded();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "v3 control checkpoint unavailable: %s", esp_err_to_name(result));
        return SyncRoundOutcome::kFailed;
    }
    result = RunDeviceClaimRoundV3();
    if (result == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(kTag, "v3 claim awaiting physical approval");
        return SyncRoundOutcome::kFailed;
    }
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "v3 claim round deferred: %s", esp_err_to_name(result));
        return g_control_protocol_blocked_this_round
            ? SyncRoundOutcome::kProtocolBlocked
            : SyncRoundOutcome::kFailed;
    }
#else
    esp_err_t result = wqn::RunPairingFlowIfNeeded();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pairing round deferred: %s", esp_err_to_name(result));
        return SyncRoundOutcome::kFailed;
    }
#endif

    std::string token;
    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "WQN online sync waiting for pairing");
        return SyncRoundOutcome::kFailed;
    }

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    result = BootstrapControlV3(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "v3 bootstrap round failed: %s", esp_err_to_name(result));
        return g_control_protocol_blocked_this_round
            ? SyncRoundOutcome::kProtocolBlocked
            : SyncRoundOutcome::kFailed;
    }
#endif

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    g_last_word_outbox_upload_state = UploadPendingWordObservations(token);
    if (g_last_word_outbox_upload_state ==
        WordOutboxUploadState::kProtocolBlocked) {
        return CurrentOutboxOutcome();
    }
    if (g_last_word_outbox_upload_state !=
        WordOutboxUploadState::kAuthenticationRequired) {
        g_last_note_outbox_upload_state = UploadPendingNoteObservations(token);
    } else {
        g_last_note_outbox_upload_state =
            WordOutboxUploadState::kAuthenticationRequired;
    }
    if (g_last_note_outbox_upload_state ==
        WordOutboxUploadState::kProtocolBlocked) {
        return CurrentOutboxOutcome();
    }
    if (g_last_word_outbox_upload_state !=
            WordOutboxUploadState::kAuthenticationRequired &&
        g_last_note_outbox_upload_state !=
            WordOutboxUploadState::kAuthenticationRequired) {
        g_last_problem_outbox_upload_state =
            UploadPendingProblemObservations(token);
    } else {
        g_last_problem_outbox_upload_state =
            WordOutboxUploadState::kAuthenticationRequired;
    }
    if (g_last_problem_outbox_upload_state ==
        WordOutboxUploadState::kProtocolBlocked) {
        return CurrentOutboxOutcome();
    }
#endif

    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "token cleared during observation upload round");
        return SyncRoundOutcome::kFailed;
    }

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    result = SyncControlPlaneV3(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "control sync round failed: %s", esp_err_to_name(result));
        // The three outboxes already ran independently. Preserve that
        // progress and report partial completion, while retaining a full-sync
        // retry obligation for the failed control plane.
        return g_control_protocol_blocked_this_round
            ? SyncRoundOutcome::kProtocolBlocked
            : SyncRoundOutcome::kPartialNeedsFullRetry;
    }
#endif

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    return CurrentOutboxOutcome();
#else
    return SyncRoundOutcome::kSucceeded;
#endif
}

TickType_t OutboxWaitDelay()
{
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    if (!g_word_outbox_sync_requested.load(std::memory_order_acquire)) {
        return portMAX_DELAY;
    }
    if (g_outbox_immediate_requested.load(std::memory_order_acquire)) {
        return 0;
    }
    const TickType_t word_delay = WordOutboxRetryWaitDelay();
    const TickType_t note_delay = NoteOutboxRetryWaitDelay();
    const TickType_t problem_delay = ProblemOutboxRetryWaitDelay();
    // [fix-b] A domain with backlog but NO armed backoff cursor is ready
    // work: it must not inherit another domain's backoff as its own wake-up
    // delay (audit FINDING B -- up to 300 s of cross-domain scheduling
    // latency coupling). Ready domains contribute 0; cursorless-idle
    // domains stay out of the minimum entirely.
    const auto effective_delay =
        [](TickType_t delay, WordOutboxUploadState last_state) {
            if (delay != portMAX_DELAY) return delay;
            return last_state == WordOutboxUploadState::kPending
                ? static_cast<TickType_t>(0)
                : portMAX_DELAY;
        };
    const TickType_t retry_delay = std::min(
        std::min(
            effective_delay(word_delay, g_last_word_outbox_upload_state),
            effective_delay(note_delay, g_last_note_outbox_upload_state)),
        effective_delay(problem_delay, g_last_problem_outbox_upload_state));
    // A requested outbox round with no retry cursor is new work (or another
    // batch behind the per-round cap), not an infinite wait. A finite cursor
    // is a real transport/server backoff and remains authoritative.
    return retry_delay == portMAX_DELAY ? 0 : retry_delay;
#else
    return portMAX_DELAY;
#endif
}

TickType_t SchedulerWaitDelay()
{
    if (g_full_sync_reasons.load(std::memory_order_acquire) != 0) {
        return 0;
    }
    // Protocol suspension parks the whole scheduler: nothing time-based may
    // wake it. Only a direct task notification (manual sync request) does.
    if (g_outbox_protocol_suspended) {
        return portMAX_DELAY;
    }
    const TickType_t retry_delay = FullSyncRetryWaitDelay();
    const TickType_t full_delay = retry_delay != portMAX_DELAY
        ? retry_delay
        : PeriodicSyncWaitDelay(
              g_auto_sync_interval_minutes.load(std::memory_order_acquire));
    return std::min(
        std::min(full_delay, OutboxWaitDelay()),
        OutboxQuietLeaseWaitDelay());
}

uint32_t FullSyncFailureRetryMs(bool has_token_after_round)
{
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    if (g_control_retry_after_ms > 0) {
        const uint32_t retry_after_ms = g_control_retry_after_ms;
        g_control_retry_after_ms = 0;
        return std::max<uint32_t>(1000, retry_after_ms);
    }
    if (!has_token_after_round) {
        return g_claim_active
            ? ClaimPollDelayMs()
            : AddClaimJitter(kClaimRetryBaseMs);
    }
#endif
    const uint8_t ladder_index = static_cast<uint8_t>(
        std::min<uint8_t>(g_full_sync_retry_attempts,
                          static_cast<uint8_t>(kFullSyncRetryLadderSize - 1)));
    ++g_full_sync_retry_attempts;
    ESP_LOGW(
        kTag,
        "full-sync nominal retry escalated: attempt=%u delay_ms=%lu",
        static_cast<unsigned>(ladder_index),
        static_cast<unsigned long>(kFullSyncRetryLadderMs[ladder_index]));
    return kFullSyncRetryLadderMs[ladder_index];
}

void SyncServiceTask(void*)
{
    ESP_LOGI(kTag, "SyncService task started");
    SetSyncTaskRunning();
    SetSyncStatus("idle");
    // Owned only by SyncServiceTask. Producers publish a quiet-window intent
    // and notify this task; keeping the move-only lease here avoids sharing an
    // RAII object across the UI, timer-service and sync tasks.
    wqn::runtime::SleepLease outbox_quiet_lease;
    while (true) {
        if (g_outbox_transport_resume_requested.exchange(
                false, std::memory_order_acq_rel)) {
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
            ResumeTransportDeferredOutboxes();
#endif
        }
        ForceExpiredOutboxQuietWindow();
        if (OutboxQuietWindowActive() && !outbox_quiet_lease) {
            outbox_quiet_lease = wqn::runtime::SleepLease::TryAcquire(
                wqn::runtime::SleepBlocker::kOnlineSync,
                "outbox-quiet",
                __FILE__,
                __LINE__);
        }
        const TickType_t wait_delay = SchedulerWaitDelay();
        if (wait_delay != 0) {
            SetSyncStatus(wait_delay == portMAX_DELAY ? "idle" : "scheduled-wait");
            ulTaskNotifyTake(pdTRUE, wait_delay);
            continue;
        }

        wqn::runtime::SleepLease sleep_lease;
        if (outbox_quiet_lease) {
            // Transfer quiet-window ownership directly into the upload round;
            // there is no lease-free gap in which PowerCoordinator can sleep.
            sleep_lease = std::move(outbox_quiet_lease);
        } else {
            sleep_lease = wqn::runtime::SleepLease::TryAcquire(
                wqn::runtime::SleepBlocker::kOnlineSync,
                "sync-service",
                __FILE__,
                __LINE__);
        }
        if (!sleep_lease) {
            SetSyncStatus("sleep-quiescing");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        // Claim request intents only after the online-sync lease is owned.
        // A lease failure means the device is quiescing; consuming these
        // flags before that point used to silently lose manual sync and
        // outbox requests.
        const uint32_t full_reasons =
            g_full_sync_reasons.exchange(0, std::memory_order_acq_rel);
        // CLIENT_PROTOCOL_BLOCKED latch: while suspended (and no explicit
        // manual/boot reason says otherwise), retry/periodic/outbox dispatch
        // is suppressed entirely -- probing a contract the firmware cannot
        // satisfy just burns the radio. The flag lives in RAM, so an OTA
        // reboot naturally re-probes once.
        bool protocol_suppressed = g_outbox_protocol_suspended;
        if (protocol_suppressed && full_reasons != 0) {
            // An explicit user/boot reason overrides the latch once; if the
            // server still answers 426 the round re-arms it below.
            protocol_suppressed = false;
        }
        const bool retry_due = !protocol_suppressed && FullSyncRetryDue();
        const uint32_t interval_minutes =
            g_auto_sync_interval_minutes.load(std::memory_order_acquire);
        const bool periodic_due = !protocol_suppressed && !retry_due &&
            FullSyncRetryWaitDelay() == portMAX_DELAY &&
            PeriodicScheduleDue(interval_minutes);
        bool full_requested =
            full_reasons != 0 || retry_due || periodic_due;
        if (full_requested) {
            ClearFullSyncRetry();
        }
        bool word_outbox_requested = false;
        if (!protocol_suppressed && (full_requested || OutboxWaitDelay() == 0)) {
            // Consume the urgency payload before the release-published ready
            // flag. If a producer lands between these exchanges, its ready
            // flag is either consumed with urgency left armed for one harmless
            // follow-up, or remains set for the next round; it is never lost.
            g_outbox_immediate_requested.exchange(
                false, std::memory_order_acq_rel);
            word_outbox_requested =
                g_word_outbox_sync_requested.exchange(
                    false, std::memory_order_acq_rel);
            if (word_outbox_requested) {
                ClaimOutboxReadyGeneration();
            }
        }
        if (!full_requested && !word_outbox_requested) {
            sleep_lease.Reset();
            continue;
        }
        ESP_LOGI(
            kTag,
            "sync dispatch: full=%d reasons=0x%lx periodic=%d retry=%d outbox=%d interval=%u",
            full_requested ? 1 : 0,
            static_cast<unsigned long>(full_reasons),
            periodic_due ? 1 : 0,
            retry_due ? 1 : 0,
            word_outbox_requested ? 1 : 0,
            static_cast<unsigned>(interval_minutes));
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        const bool outbox_only = word_outbox_requested && !full_requested;
#else
        const bool outbox_only = false;
        (void)word_outbox_requested;
#endif

        const bool interactive_sync =
            (full_reasons & (kFullSyncManual | kFullSyncCredentials)) != 0;
        wqn::services::ConnectivityDemand connectivity_demand =
            wqn::services::AcquireConnectivityDemand(
                interactive_sync
                    ? wqn::services::ConnectivityDemandReason::kSyncInteractive
                    : wqn::services::ConnectivityDemandReason::kSyncBackground,
                interactive_sync ? "sync-interactive" : "sync-background",
                __FILE__,
                __LINE__);
        if (!connectivity_demand) {
            ESP_LOGW(kTag, "sync round could not acquire connectivity demand");
        }

        SetSyncRoundStarted(esp_timer_get_time() / 1000);
        SetSyncStatus(outbox_only ? "word-outbox" : "syncing");
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        const SyncRoundOutcome outcome = outbox_only
            ? RunWordOutboxOnlyRound()
            : RunSyncRound();
#else
        const SyncRoundOutcome outcome = RunSyncRound();
#endif
        const int64_t finished_ms = esp_timer_get_time() / 1000;
        CompleteSyncRound(finished_ms, outcome);
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        PublishOutboxSnapshots();
#endif
        const bool has_token_after_round = wqn::services::HasUsableStoredToken();
        if (outcome == SyncRoundOutcome::kProtocolBlocked) {
            // CLIENT_PROTOCOL_BLOCKED: latch suspension. No retry ladder,
            // no periodic re-arm, no outbox self-wake -- only an explicit
            // manual/boot reason may re-probe the server (and fail the same
            // way until an OTA lands). Records stay parked in place.
            g_outbox_protocol_suspended = true;
            SetSyncStatus("protocol-blocked");
            PublishSyncEvent(
                wqn::services::SyncEventStatus::kFailed,
                finished_ms,
                outbox_only
                    ? wqn::services::SyncEventScope::kWordOutbox
                    : wqn::services::SyncEventScope::kFull);
            ClearFullSyncRetry();
        } else if (outcome == SyncRoundOutcome::kSucceeded) {
            g_outbox_protocol_suspended = false;
            SetSyncStatus("success");
            PublishSyncEvent(
                wqn::services::SyncEventStatus::kSucceeded,
                finished_ms,
                outbox_only
                    ? wqn::services::SyncEventScope::kWordOutbox
                    : wqn::services::SyncEventScope::kFull);
        } else if (outcome == SyncRoundOutcome::kPartial ||
                   outcome == SyncRoundOutcome::kPartialNeedsFullRetry) {
            g_outbox_protocol_suspended = false;
            SetSyncStatus("partial");
            PublishSyncEvent(
                wqn::services::SyncEventStatus::kPartial,
                finished_ms,
                outbox_only
                    ? wqn::services::SyncEventScope::kWordOutbox
                    : wqn::services::SyncEventScope::kFull);
        } else {
            g_outbox_protocol_suspended = false;
            SetSyncStatus(has_token_after_round ? "failed" : "waiting-pair");
            PublishSyncEvent(
                has_token_after_round
                    ? wqn::services::SyncEventStatus::kFailed
                    : wqn::services::SyncEventStatus::kAwaitingClaim,
                finished_ms,
                outbox_only
                    ? wqn::services::SyncEventScope::kWordOutbox
                    : wqn::services::SyncEventScope::kFull);
        }
        if (!outbox_only) {
            if (outcome == SyncRoundOutcome::kSucceeded ||
                outcome == SyncRoundOutcome::kPartial) {
                ClearFullSyncRetry();
                ScheduleNextPeriodicSync(
                    g_auto_sync_interval_minutes.load(std::memory_order_acquire));
            } else if (outcome != SyncRoundOutcome::kProtocolBlocked) {
                ScheduleFullSyncRetry(
                    FullSyncFailureRetryMs(has_token_after_round));
            }
        } else if (outcome == SyncRoundOutcome::kPartialNeedsFullRetry) {
            // A local queue/storage failure has no per-item retry cursor.
            // Escalate it to the bounded full-round backoff so the durable
            // head cannot remain stranded when periodic sync is disabled.
            ScheduleFullSyncRetry(FullSyncFailureRetryMs(has_token_after_round));
        }
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        if (g_outbox_protocol_suspended &&
            !g_full_sync_reasons.load(std::memory_order_acquire)) {
            // Suppressed: neither the retry deadline nor the outbox flag may
            // wake the task while protocol-blocked. A manual/boot full-sync
            // reason still dispatches (checked at the loop head).
        } else if (
            g_last_word_outbox_upload_state == WordOutboxUploadState::kYielded ||
            g_last_note_outbox_upload_state == WordOutboxUploadState::kYielded ||
            g_last_problem_outbox_upload_state == WordOutboxUploadState::kYielded) {
            // Do not turn a user-induced yield into the old 100 ms upload
            // loop. Resume only after another complete quiet period.
            const uint32_t generation = ArmOutboxQuietWindow();
            if (g_word_outbox_timer == nullptr ||
                xTimerReset(g_word_outbox_timer, 0) != pdPASS) {
                PublishOutboxReadyGeneration(generation);
            }
        } else if (
            g_last_word_outbox_upload_state == WordOutboxUploadState::kPending ||
            g_last_note_outbox_upload_state == WordOutboxUploadState::kPending ||
            g_last_problem_outbox_upload_state == WordOutboxUploadState::kPending) {
            g_word_outbox_sync_requested.store(true, std::memory_order_release);
            if (g_sync_service_task != nullptr) {
                xTaskNotifyGive(g_sync_service_task);
            }
        }
#endif
        // Publish every retry/outbox/timer-wake obligation before releasing
        // the online-sync SleepLease. Otherwise PowerCoordinator could close
        // quiesce in the tiny gap and sleep without the wake reason installed.
        const bool lease_cycle_expired = TakeOutboxLeaseCycleExpired();
        if (OutboxQuietWindowActive() && !lease_cycle_expired) {
            // A newer observation landed while this round was running. Carry
            // the same lease into its debounce window instead of releasing and
            // immediately reacquiring it on the next loop iteration.
            outbox_quiet_lease = std::move(sleep_lease);
        } else {
            // A max-duration quiet cycle releases at least once after its
            // bounded upload attempt. If newer work is active, the next loop
            // starts a fresh lease rather than extending this one forever.
            sleep_lease.Reset();
        }
    }
}

#endif  // CONFIG_WQN_WIFI_STA_ENABLE

}  // namespace

namespace wqn::services {

bool HasUsableStoredToken()
{
    std::string token;
    return LoadUsableToken(&token);
}

wqn::protocol::v3::RequestMetadata MakeDeviceRequestMetadata()
{
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    wqn::protocol::v3::RequestMetadata metadata;
    metadata.request_id = RandomControlId("req_");
    metadata.boot_id = ControlBootId();
    metadata.firmware_version = WQN_FIRMWARE_VERSION;
    return metadata;
#else
    return {};
#endif
}

#if CONFIG_WQN_WIFI_STA_ENABLE

bool EvaluateSyncWorkAtBoot()
{
    uint32_t interval_minutes = 0;
    if (wqn::LoadAutoSyncIntervalMinutes(&interval_minutes) != ESP_OK) {
        // A settings read failure is not a reason to make a timer wake go
        // offline forever; connect once so diagnostics/control can recover.
        interval_minutes = 0;
        g_auto_sync_interval_minutes.store(0, std::memory_order_release);
        g_boot_outbox_pending.store(true, std::memory_order_release);
        g_boot_policy_evaluated.store(true, std::memory_order_release);
        return true;
    }
    g_auto_sync_interval_minutes.store(interval_minutes, std::memory_order_release);
    const bool outbox_pending = ProbeDurableOutboxWork();
    g_boot_outbox_pending.store(outbox_pending, std::memory_order_release);

    wqn::SyncJournal journal;
    const esp_err_t journal_result = wqn::LoadSyncJournal(&journal);
    const bool content_pending = journal_result != ESP_OK ||
        JournalHasPendingContent(journal);
    g_boot_policy_evaluated.store(true, std::memory_order_release);

    const wqn::runtime::WakeContext& wake = wqn::runtime::GetWakeContext();
    if (wake.kind != wqn::runtime::WakeKind::kScheduledTimer) {
        return true;
    }
    if (!HasUsableStoredToken()) {
        ESP_LOGI(kTag, "timer wake keeps WiFi off: device is not paired");
        return false;
    }
    const bool periodic_due = PeriodicScheduleDue(interval_minutes);
    const bool retry_due = FullSyncRetryDue();
    const bool should_connect = periodic_due || retry_due || outbox_pending ||
        content_pending;
    ESP_LOGI(
        kTag,
        "timer-wake connectivity admission: connect=%d periodic=%d retry=%d outbox=%d content=%d interval=%u",
        should_connect ? 1 : 0,
        periodic_due ? 1 : 0,
        retry_due ? 1 : 0,
        outbox_pending ? 1 : 0,
        content_pending ? 1 : 0,
        static_cast<unsigned>(interval_minutes));
    return should_connect;
}

esp_err_t StartSyncService()
{
    if (!g_boot_policy_evaluated.load(std::memory_order_acquire)) {
        (void)EvaluateSyncWorkAtBoot();
    }
    if (g_sync_journal_mutex == nullptr) {
        g_sync_journal_mutex =
            xSemaphoreCreateMutexStatic(&g_sync_journal_mutex_storage);
        if (g_sync_journal_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(EnsureSyncJournalLoaded(), kTag, "load sync journal");
    if (!wqn::runtime::GetWakeContext().deep_sleep_resume) {
        g_full_sync_reasons.fetch_or(kFullSyncBoot, std::memory_order_release);
    }
    if (g_boot_outbox_pending.load(std::memory_order_acquire)) {
        // A durable record is pending work, not an instruction to bypass an
        // existing transport/server retry cursor. OutboxWaitDelay owns due
        // admission; explicit producers still use the immediate flag.
        g_word_outbox_sync_requested.store(true, std::memory_order_release);
    }
    if (g_word_outbox_timer == nullptr) {
        g_word_outbox_timer = xTimerCreateStatic(
            "word_outbox",
            pdMS_TO_TICKS(kWordOutboxQuietPeriodMs),
            pdFALSE,
            nullptr,
            WordOutboxTimerCallback,
            &g_word_outbox_timer_storage);
        if (g_word_outbox_timer == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    const BaseType_t created = xTaskCreate(
        SyncServiceTask,
        "sync_service",
        12288,
        nullptr,
        5,
        &g_sync_service_task);
    if (created != pdPASS) {
        g_sync_service_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif  // CONFIG_WQN_WIFI_STA_ENABLE

void RequestSyncNow()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    g_full_sync_reasons.fetch_or(kFullSyncManual, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
#endif
}

void NotifySyncCredentialsChanged()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    g_full_sync_reasons.fetch_or(
        kFullSyncCredentials, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
#endif
}

void NotifySyncConnectivityAvailable()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    // Readiness must not invent a full-sync reason, but it may release an
    // outbox transport backoff that was created solely because WiFi was down.
    // The sync task owns the non-atomic retry cursors and applies this intent.
    g_outbox_transport_resume_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
#endif
}

void NotifyAutoSyncIntervalChanged(uint32_t minutes)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (minutes != 0 && minutes != 15 && minutes != 30 && minutes != 60 &&
        minutes != 240) {
        ESP_LOGW(kTag, "ignore invalid auto-sync interval: %u",
                 static_cast<unsigned>(minutes));
        return;
    }
    g_auto_sync_interval_minutes.store(minutes, std::memory_order_release);
    ScheduleNextPeriodicSync(minutes);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
#else
    (void)minutes;
#endif
}

uint32_t SecondsUntilNextSyncWake()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_full_sync_reasons.load(std::memory_order_acquire) != 0) {
        return 1;
    }
    // Normally the task-owned quiet lease prevents sleep. This retained wake
    // deadline closes the producer->task acquisition race: if quiesce wins,
    // deep sleep wakes when the debounce would have fired and the boot outbox
    // probe immediately resumes the durable records.
    uint32_t next_seconds = SecondsUntilOutboxQuietWake();
    if (g_word_outbox_sync_requested.load(std::memory_order_acquire)) {
        const TickType_t outbox_ticks = OutboxWaitDelay();
        next_seconds = outbox_ticks == 0
            ? 1
            : (outbox_ticks == portMAX_DELAY
                   ? 60
                   : std::max<uint32_t>(
                         1,
                         (static_cast<uint64_t>(outbox_ticks) *
                              portTICK_PERIOD_MS +
                          999) /
                             1000));
    }
    uint32_t retry_magic = 0;
    int64_t retry_due_seconds = 0;
    uint32_t schedule_magic = 0;
    uint32_t scheduled_interval = 0;
    int64_t periodic_due_seconds = 0;
    taskENTER_CRITICAL(&g_periodic_schedule_lock);
    retry_magic = g_full_sync_retry_magic;
    retry_due_seconds = g_full_sync_retry_unix_seconds;
    schedule_magic = g_periodic_schedule_magic;
    scheduled_interval = g_periodic_schedule_interval_minutes;
    periodic_due_seconds = g_next_periodic_sync_unix_seconds;
    taskEXIT_CRITICAL(&g_periodic_schedule_lock);
    uint32_t content_seconds = UINT32_MAX;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    const auto include_content =
        [now_ms, &content_seconds](const SyncContentSnapshot& snapshot) {
            if (snapshot.desired_revision <= snapshot.applied_revision ||
                snapshot.phase == SyncContentPhase::kBlocked) {
                return;
            }
            uint32_t seconds = 1;
            if (snapshot.phase == SyncContentPhase::kBackoff &&
                snapshot.next_retry_ms > now_ms) {
                seconds = static_cast<uint32_t>(std::max<int64_t>(
                    1, (snapshot.next_retry_ms - now_ms + 999) / 1000));
            }
            content_seconds = std::min(content_seconds, seconds);
        };
    include_content(g_sync_snapshot.word_packs);
    include_content(g_sync_snapshot.note_packs);
    include_content(g_sync_snapshot.problem_packs);
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    next_seconds = std::min(next_seconds, content_seconds);

    const std::time_t now = CurrentUnixSeconds();
    const auto seconds_until = [now](int64_t due_seconds) -> uint32_t {
        if (due_seconds == 0) {
            return UINT32_MAX;
        }
        if (now < kMinScheduleUnixTime || static_cast<int64_t>(now) >= due_seconds) {
            return 1;
        }
        return static_cast<uint32_t>(std::min<int64_t>(
            UINT32_MAX,
            due_seconds - static_cast<int64_t>(now)));
    };
    if (retry_magic == kFullRetryMagic) {
        uint32_t retry_seconds = UINT32_MAX;
        if (retry_due_seconds >= static_cast<int64_t>(kMinScheduleUnixTime) &&
            now >= kMinScheduleUnixTime) {
            retry_seconds = seconds_until(retry_due_seconds);
        } else {
            const int64_t remaining_ms =
                g_full_sync_retry_not_before_ms.load(std::memory_order_acquire) -
                esp_timer_get_time() / 1000;
            retry_seconds = remaining_ms <= 0
                ? 1
                : static_cast<uint32_t>(std::min<int64_t>(
                      UINT32_MAX, (remaining_ms + 999) / 1000));
        }
        next_seconds = std::min(next_seconds, retry_seconds);
    }
    const uint32_t interval_minutes =
        g_auto_sync_interval_minutes.load(std::memory_order_acquire);
    if (interval_minutes != 0) {
        const bool schedule_matches =
            schedule_magic == kPeriodicScheduleMagic &&
            scheduled_interval == interval_minutes;
        const uint32_t periodic_seconds = !schedule_matches
            ? 1
            : periodic_due_seconds >= static_cast<int64_t>(kMinScheduleUnixTime) &&
                    now >= kMinScheduleUnixTime
                ? seconds_until(periodic_due_seconds)
                : [&]() -> uint32_t {
                    const int64_t remaining_ms =
                        g_next_periodic_sync_not_before_ms.load(
                            std::memory_order_acquire) -
                        esp_timer_get_time() / 1000;
                    return remaining_ms <= 0
                        ? 1
                        : static_cast<uint32_t>(std::min<int64_t>(
                              UINT32_MAX, (remaining_ms + 999) / 1000));
                }();
        next_seconds = std::min(next_seconds, periodic_seconds);
    }
    return next_seconds == UINT32_MAX ? 0 : next_seconds;
#else
    return 0;
#endif
}

void RequestContentRefresh(SyncContentDomain domain)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    const uint32_t bit = ContentRefreshBit(domain);
    if (bit == 0) {
        return;
    }
    g_content_refresh_requested.fetch_or(bit, std::memory_order_release);
    g_full_sync_reasons.fetch_or(
        kFullSyncContentRefresh, std::memory_order_release);
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    SyncContentSnapshot* snapshot = nullptr;
    switch (domain) {
        case SyncContentDomain::kWordPacks:
            snapshot = &g_sync_snapshot.word_packs;
            break;
        case SyncContentDomain::kNotePacks:
            snapshot = &g_sync_snapshot.note_packs;
            break;
        case SyncContentDomain::kProblemPacks:
            snapshot = &g_sync_snapshot.problem_packs;
            break;
    }
    if (snapshot != nullptr &&
        snapshot->phase != SyncContentPhase::kFetching &&
        snapshot->phase != SyncContentPhase::kInstalling) {
        snapshot->phase = SyncContentPhase::kPending;
        snapshot->next_retry_ms = 0;
        ++g_sync_snapshot.state_sequence;
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
#else
    (void)domain;
#endif
}

SyncContentTicket TryClaimContentRefresh(SyncContentDomain domain)
{
    SyncContentTicket ticket;
    ticket.domain = domain;
#if CONFIG_WQN_WIFI_STA_ENABLE
    const uint32_t bit = ContentRefreshBit(domain);
    if (bit == 0) {
        return ticket;
    }
    const bool forced =
        (g_content_refresh_requested.fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    SyncContentSnapshot* snapshot = ContentSnapshotForDomain(domain);
    const bool due = snapshot != nullptr &&
        (snapshot->phase == SyncContentPhase::kPending ||
         (snapshot->phase == SyncContentPhase::kBackoff &&
          snapshot->next_retry_ms <= now_ms));
    const bool needs_convergence = snapshot != nullptr &&
        snapshot->desired_revision > snapshot->applied_revision;
    const size_t index = ContentDomainIndex(domain);
    if (snapshot != nullptr && g_content_active_generation[index] == 0 &&
        (forced || (due && needs_convergence))) {
        uint32_t generation = ++g_content_claim_generation[index];
        if (generation == 0) {
            generation = ++g_content_claim_generation[index];
        }
        g_content_active_generation[index] = generation;
        snapshot->phase = SyncContentPhase::kFetching;
        snapshot->next_retry_ms = 0;
        ++g_sync_snapshot.state_sequence;
        ticket.generation = generation;
        ticket.target_revision = snapshot->desired_revision;
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    if (!ticket && forced) {
        g_content_refresh_requested.fetch_or(bit, std::memory_order_release);
    }
#else
    (void)domain;
#endif
    return ticket;
}

void CancelContentRefreshClaim(const SyncContentTicket& ticket)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (!ticket || ContentRefreshBit(ticket.domain) == 0) {
        return;
    }
    const size_t index = ContentDomainIndex(ticket.domain);
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    if (g_content_active_generation[index] == ticket.generation) {
        g_content_active_generation[index] = 0;
        SyncContentSnapshot* snapshot = ContentSnapshotForDomain(ticket.domain);
        if (snapshot != nullptr) {
            snapshot->phase = SyncContentPhase::kPending;
            ++g_sync_snapshot.state_sequence;
        }
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    g_content_refresh_requested.fetch_or(
        ContentRefreshBit(ticket.domain), std::memory_order_release);
#else
    (void)ticket;
#endif
}

esp_err_t BeginContentInstall(const SyncContentTicket& ticket)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (!ticket || ContentRefreshBit(ticket.domain) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool accepted = false;
    const size_t index = ContentDomainIndex(ticket.domain);
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    if (g_content_active_generation[index] == ticket.generation) {
        SyncContentSnapshot* snapshot = ContentSnapshotForDomain(ticket.domain);
        wqn::SyncJournalContentState* journal = JournalStateForDomain(ticket.domain);
        if (snapshot != nullptr && journal != nullptr) {
            snapshot->phase = SyncContentPhase::kInstalling;
            journal->phase = wqn::SyncJournalPhase::kInstalling;
            ++g_sync_snapshot.state_sequence;
            accepted = true;
        }
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    if (!accepted) {
        return ESP_ERR_INVALID_STATE;
    }
    return PersistLatestSyncJournal();
#else
    (void)ticket;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void CompleteContentRefresh(
    const SyncContentTicket& ticket,
    esp_err_t result,
    const char* snapshot_id,
    const char* error)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (!ticket || ContentRefreshBit(ticket.domain) == 0) {
        return;
    }
    const size_t index = ContentDomainIndex(ticket.domain);
    bool accepted = false;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    if (g_content_active_generation[index] == ticket.generation) {
        g_content_active_generation[index] = 0;
        SyncContentSnapshot* snapshot = ContentSnapshotForDomain(ticket.domain);
        wqn::SyncJournalContentState* journal = JournalStateForDomain(ticket.domain);
        if (snapshot != nullptr && journal != nullptr) {
            if (result == ESP_OK) {
                snapshot->applied_revision = std::max(
                    snapshot->applied_revision, ticket.target_revision);
                snapshot->retry_attempt = 0;
                snapshot->next_retry_ms = 0;
                snapshot->last_error[0] = '\0';
                if (snapshot_id != nullptr && std::strlen(snapshot_id) == 64) {
                    std::snprintf(snapshot->snapshot_id,
                                  sizeof(snapshot->snapshot_id), "%s", snapshot_id);
                    std::snprintf(journal->desired_snapshot_id,
                                  sizeof(journal->desired_snapshot_id), "%s", snapshot_id);
                    std::snprintf(journal->active_snapshot_id,
                                  sizeof(journal->active_snapshot_id), "%s", snapshot_id);
                }
                snapshot->phase = snapshot->desired_revision > snapshot->applied_revision
                    ? SyncContentPhase::kPending
                    : SyncContentPhase::kClean;
                journal->applied_revision = snapshot->applied_revision;
                journal->phase = snapshot->phase == SyncContentPhase::kClean
                    ? wqn::SyncJournalPhase::kClean
                    : wqn::SyncJournalPhase::kPending;
                journal->retry_attempt = 0;
            } else {
                if (snapshot->retry_attempt < UINT8_MAX) {
                    ++snapshot->retry_attempt;
                }
                const uint8_t shift = std::min<uint8_t>(snapshot->retry_attempt - 1, 7);
                const uint32_t base_ms = std::min<uint32_t>(5000u << shift, 900000u);
                const uint32_t low_ms = base_ms * 4u / 5u;
                const uint32_t spread_ms = base_ms * 2u / 5u;
                const uint32_t delay_ms = low_ms + esp_random() % (spread_ms + 1u);
                snapshot->next_retry_ms =
                    esp_timer_get_time() / 1000 + static_cast<int64_t>(delay_ms);
                snapshot->phase = SyncContentPhase::kBackoff;
                std::snprintf(snapshot->last_error,
                              sizeof(snapshot->last_error), "%s",
                              error == nullptr ? esp_err_to_name(result) : error);
                journal->phase = wqn::SyncJournalPhase::kBackoff;
                journal->retry_attempt = snapshot->retry_attempt;
            }
            ++g_sync_snapshot.state_sequence;
            accepted = true;
        }
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    if (accepted && PersistLatestSyncJournal() != ESP_OK) {
        ESP_LOGE(kTag, "content completion journal save failed: domain=%u",
                 static_cast<unsigned>(ticket.domain));
    }
    if (accepted && result != ESP_OK) {
        // Content lanes converge independently from the control plane and
        // outboxes. Surface their durable backoff as partial completion; a
        // single pack failure must not relabel already-synced domains as a
        // failed global round.
        const int64_t finished_ms = esp_timer_get_time() / 1000;
        SetSyncStatus("partial");
        PublishSyncEvent(SyncEventStatus::kPartial, finished_ms, SyncEventScope::kFull);
    }
#else
    (void)ticket;
    (void)result;
    (void)snapshot_id;
    (void)error;
#endif
}

void NoteWordInteraction()
{
#if CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    g_word_interaction_generation.fetch_add(1, std::memory_order_acq_rel);
#endif
}

void RequestWordOutboxUpload()
{
#if CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    const uint32_t generation = ArmOutboxQuietWindow();
    if (g_word_outbox_timer != nullptr &&
        xTimerReset(g_word_outbox_timer, 0) == pdPASS) {
        // Wake SyncService now so it acquires the bounded quiet-window lease;
        // the timer callback still owns publication of the upload-ready flag.
        if (g_sync_service_task != nullptr) {
            xTaskNotifyGive(g_sync_service_task);
        }
        return;
    }
    // Timer command queue pressure must not strand a durable observation.
    // Fall back to an immediate outbox-only notification.
    PublishOutboxReadyGeneration(generation);
#endif
}

void RequestNoteOutboxUpload()
{
    // Note observations share word's quiet-window timer and outbox-only round
    // (the round uploads both queues), so this reuses the same trigger path.
#if CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    const uint32_t generation = ArmOutboxQuietWindow();
    if (g_word_outbox_timer != nullptr &&
        xTimerReset(g_word_outbox_timer, 0) == pdPASS) {
        if (g_sync_service_task != nullptr) {
            xTaskNotifyGive(g_sync_service_task);
        }
        return;
    }
    // Timer command queue pressure must not strand a durable observation.
    // Fall back to an immediate outbox-only notification.
    PublishOutboxReadyGeneration(generation);
#endif
}

void RequestProblemOutboxUpload()
{
    // Problem verdicts share the same quiet-window timer and outbox-only
    // round (the round uploads all three queues).
#if CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    const uint32_t generation = ArmOutboxQuietWindow();
    if (g_word_outbox_timer != nullptr &&
        xTimerReset(g_word_outbox_timer, 0) == pdPASS) {
        if (g_sync_service_task != nullptr) {
            xTaskNotifyGive(g_sync_service_task);
        }
        return;
    }
    // Timer command queue pressure must not strand a durable observation.
    // Fall back to an immediate outbox-only notification.
    PublishOutboxReadyGeneration(generation);
#endif
}

void GetSyncSnapshot(SyncSnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
#if CONFIG_WQN_WIFI_STA_ENABLE
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    *snapshot = g_sync_snapshot;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
    snapshot->interval_minutes =
        g_auto_sync_interval_minutes.load(std::memory_order_acquire);
#else
    *snapshot = {};
    std::snprintf(snapshot->status, sizeof(snapshot->status), "%s", "wifi-disabled");
#endif
}

}  // namespace wqn::services

namespace wqn::services {

void GetLatestSyncEvent(SyncEvent* event)
{
    if (event == nullptr) {
        return;
    }
#if CONFIG_WQN_WIFI_STA_ENABLE
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    *event = g_latest_sync_event;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
#else
    *event = SyncEvent{};
#endif
}

void SetSyncEventSink(SyncEventSink sink)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    g_sync_event_sink.store(sink, std::memory_order_release);
#else
    (void)sink;
#endif
}

}  // namespace wqn::services
