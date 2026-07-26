#include "services/sync_service.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "device_protocol/claim_crypto.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "runtime/sleep_coordinator.h"
#include "storage.h"
#include "note_store.h"
#include "word_study_store.h"
#include "wqn_api.h"

namespace {

constexpr char kTag[] = "sync_service";

#if CONFIG_WQN_WIFI_STA_ENABLE
wqn::services::SyncSnapshot g_sync_snapshot = {};
portMUX_TYPE g_sync_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<wqn::services::SyncEventSink> g_sync_event_sink{nullptr};
uint32_t g_sync_event_sequence = 1;
constexpr TickType_t kSyncRetryDelay = pdMS_TO_TICKS(10000);
bool LoadUsableToken(std::string* token);
TaskHandle_t g_sync_service_task = nullptr;
std::atomic<bool> g_full_sync_requested{false};
std::atomic<bool> g_word_outbox_sync_requested{false};
std::atomic<uint32_t> g_word_interaction_generation{0};
constexpr uint32_t kWordOutboxQuietPeriodMs = 5000;
StaticTimer_t g_word_outbox_timer_storage;
TimerHandle_t g_word_outbox_timer = nullptr;

void WordOutboxTimerCallback(TimerHandle_t)
{
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
}
#endif

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
uint64_t g_config_revision = 0;
uint64_t g_sync_cursor = 0;
bool g_bootstrap_complete = false;
bool g_control_state_loaded = false;
uint32_t g_control_retry_after_ms = 0;
constexpr uint32_t kClaimPollFloorMs = 10000;
constexpr uint32_t kClaimPollJitterMaxMs = 2000;
constexpr uint32_t kClaimRetryBaseMs = 15000;
constexpr uint32_t kClaimRetryMaxMs = 5 * 60 * 1000;
constexpr uint32_t kStorageCapacityRetryMs = 5 * 60 * 1000;
constexpr uint8_t kClaimRetryMaxShift = 4;
constexpr uint32_t kWordOutboxRetryBaseMs = 30000;
constexpr uint32_t kWordOutboxRetryMaxMs = 5 * 60 * 1000;
constexpr uint32_t kWordOutboxRetryJitterMaxMs = 2000;
constexpr uint8_t kWordOutboxRetryMaxShift = 4;
std::string g_bootstrap_request_id;
std::string g_sync_request_id;
wqn::protocol::v3::ClaimKeyPair g_claim_key_pair;
std::string g_claim_start_request_id;
std::string g_claim_id;
uint32_t g_claim_poll_interval_ms = kClaimPollFloorMs;
uint8_t g_claim_retry_attempts = 0;
bool g_claim_active = false;
std::string g_word_outbox_retry_request_id;
int64_t g_word_outbox_retry_not_before_ms = 0;
uint8_t g_word_outbox_retry_attempts = 0;

enum class WordOutboxUploadState : uint8_t {
    kDrained,
    kPending,
    kYielded,
    kFailed,
};

WordOutboxUploadState g_last_word_outbox_upload_state =
    WordOutboxUploadState::kDrained;

// Note observations ride the same quiet-window timer and interaction-yield
// signal as word, but keep an independent retry cursor so a deferred note head
// never blocks word progress (and vice versa). WordOutboxUploadState is reused
// verbatim; the semantics are identical.
std::string g_note_outbox_retry_request_id;
int64_t g_note_outbox_retry_not_before_ms = 0;
uint8_t g_note_outbox_retry_attempts = 0;
WordOutboxUploadState g_last_note_outbox_upload_state =
    WordOutboxUploadState::kDrained;

void ResetWordOutboxRetryBackoff()
{
    g_word_outbox_retry_request_id.clear();
    g_word_outbox_retry_not_before_ms = 0;
    g_word_outbox_retry_attempts = 0;
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
    uint32_t server_retry_after_ms)
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
    ESP_LOGW(
        kTag,
        "word outbox retry scheduled: request=%s attempt=%u retry_after_ms=%lu",
        request_id.c_str(),
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
        return 1;
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
    uint32_t server_retry_after_ms)
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
    ESP_LOGW(
        kTag,
        "note outbox retry scheduled: request=%s attempt=%u retry_after_ms=%lu",
        request_id.c_str(),
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
        return 1;
    }
    return pdMS_TO_TICKS(
        static_cast<uint32_t>(
            std::min<int64_t>(remaining_ms, kWordOutboxRetryMaxMs)));
}

bool IsStorageCapacityError(esp_err_t error)
{
    return error == ESP_ERR_NO_MEM || error == ESP_ERR_INVALID_SIZE ||
        error == ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}

void ApplyStorageCapacityBackoff(esp_err_t error, const char* stage)
{
    if (!IsStorageCapacityError(error)) {
        return;
    }
    g_control_retry_after_ms = std::max(
        g_control_retry_after_ms,
        kStorageCapacityRetryMs);
    ESP_LOGW(
        kTag,
        "sync storage-full: stage=%s error=%s retry_after_ms=%lu",
        stage,
        esp_err_to_name(error),
        static_cast<unsigned long>(g_control_retry_after_ms));
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

void CompleteSyncRound(int64_t finished_ms, bool synced)
{
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    g_sync_snapshot.last_finished_ms = finished_ms;
    g_sync_snapshot.last_round_success = synced;
    if (synced) {
        ++g_sync_snapshot.success_count;
    } else {
        ++g_sync_snapshot.failure_count;
    }
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);
}

void PublishSyncEvent(
    wqn::services::SyncEventStatus status,
    int64_t finished_ms,
    wqn::services::SyncEventScope scope)
{
    wqn::services::SyncEvent event;
    event.status = status;
    event.scope = scope;
    event.sequence = g_sync_event_sequence++;
    if (event.sequence == 0) {
        event.sequence = g_sync_event_sequence++;
    }
    event.finished_ms = finished_ms;
    taskENTER_CRITICAL(&g_sync_snapshot_lock);
    std::snprintf(
        event.claim_code,
        sizeof(event.claim_code),
        "%s",
        g_sync_snapshot.claim_code);
    event.claim_expires_at_ms = g_sync_snapshot.claim_expires_at_ms;
    taskEXIT_CRITICAL(&g_sync_snapshot_lock);

    const wqn::services::SyncEventSink sink =
        g_sync_event_sink.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(event);
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

std::vector<wqn::CachedProblem> ToCachedProblems(const std::vector<wqn::WqnProblem>& problems)
{
    std::vector<wqn::CachedProblem> cached;
    cached.reserve(problems.size());
    for (const wqn::WqnProblem& problem : problems) {
        wqn::CachedProblem item;
        item.id = problem.id;
        item.title = problem.title;
        item.type = problem.problem_type;
        item.status = problem.status;
        item.content_text = problem.content_text;
        item.solution_text = problem.solution_text;
        item.asset_count = problem.asset_count;
        item.solution_asset_count = problem.solution_asset_count;
        item.updated_at = problem.updated_at;
        cached.push_back(std::move(item));
    }
    return cached;
}

bool UpsertProblem(std::vector<wqn::CachedProblem>* cached, wqn::CachedProblem problem)
{
    if (cached == nullptr || problem.id.empty()) {
        return false;
    }

    for (wqn::CachedProblem& item : *cached) {
        if (item.id == problem.id) {
            bool changed = false;
            if (!problem.title.empty() && item.title != problem.title) {
                item.title = std::move(problem.title);
                changed = true;
            }
            if (!problem.type.empty() && item.type != problem.type) {
                item.type = std::move(problem.type);
                changed = true;
            }
            if (!problem.status.empty() && item.status != problem.status) {
                item.status = std::move(problem.status);
                changed = true;
            }
            if (!problem.content_text.empty() && item.content_text != problem.content_text) {
                item.content_text = std::move(problem.content_text);
                changed = true;
            }
            if (!problem.solution_text.empty() && item.solution_text != problem.solution_text) {
                item.solution_text = std::move(problem.solution_text);
                changed = true;
            }
            if (item.asset_count != problem.asset_count) {
                item.asset_count = problem.asset_count;
                changed = true;
            }
            if (item.solution_asset_count != problem.solution_asset_count) {
                item.solution_asset_count = problem.solution_asset_count;
                changed = true;
            }
            if (!problem.updated_at.empty() && item.updated_at != problem.updated_at) {
                item.updated_at = std::move(problem.updated_at);
                changed = true;
            }
            return changed;
        }
    }
    cached->push_back(std::move(problem));
    return true;
}

esp_err_t MergeProblemCache(const std::vector<wqn::WqnProblem>& fresh, const char* source)
{
    if (fresh.empty()) {
        return ESP_OK;
    }

    std::vector<wqn::CachedProblem> cached;
    const esp_err_t load_result = wqn::LoadProblems(&cached);
    if (load_result != ESP_OK) {
        ESP_LOGW(kTag, "dropping unreadable problem cache before merge: %s", esp_err_to_name(load_result));
        cached.clear();
    }

    std::vector<wqn::CachedProblem> incoming = ToCachedProblems(fresh);
    bool changed = false;
    for (wqn::CachedProblem& problem : incoming) {
        changed = UpsertProblem(&cached, std::move(problem)) || changed;
    }
    if (!changed) {
        ESP_LOGI(
            kTag,
            "%s cache unchanged: fresh=%u total_cached=%u; write skipped",
            source,
            static_cast<unsigned>(fresh.size()),
            static_cast<unsigned>(cached.size()));
        return ESP_OK;
    }

    const esp_err_t save_result = wqn::SaveProblems(cached);
    if (save_result != ESP_OK) {
        ESP_LOGW(kTag, "save problem cache failed: %s", esp_err_to_name(save_result));
        return save_result;
    }

    ESP_LOGI(
        kTag,
        "%s cached: fresh=%u total_cached=%u",
        source,
        static_cast<unsigned>(fresh.size()),
        static_cast<unsigned>(cached.size()));
    return ESP_OK;
}

esp_err_t UploadPendingReviewsIfAny(const std::string& token)
{
    std::vector<wqn::PendingReviewResult> pending;
    esp_err_t result = wqn::LoadPendingReviewResults(&pending);
    if (result != ESP_OK) {
        return result;
    }
    if (pending.empty()) {
        ESP_LOGI(kTag, "no pending review uploads");
        return ESP_OK;
    }

    std::vector<wqn::WqnReviewResult> uploads;
    uploads.reserve(pending.size());
    for (const wqn::PendingReviewResult& item : pending) {
        if (item.problem_id.empty() || item.selected_status.empty()) {
            ESP_LOGW(kTag, "pending review queue contains an invalid item; keeping queue");
            return ESP_ERR_INVALID_STATE;
        }

        wqn::WqnReviewResult upload;
        upload.problem_id = item.problem_id;
        upload.selected_status = item.selected_status;
        upload.reviewed_at = item.created_at;
        uploads.push_back(std::move(upload));
    }

    result = wqn::UploadReviewComplete(token, uploads);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pending review upload kept for retry: %s", esp_err_to_name(result));
        return result;
    }

    result = wqn::ClearPendingReviewResults();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "clear uploaded review queue failed: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(kTag, "pending review uploads complete: count=%u", static_cast<unsigned>(uploads.size()));
    return ESP_OK;
}

#if !CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
esp_err_t SyncDueProblemsAndCache(const std::string& token)
{
    std::vector<std::string> due_problem_ids;
    int total = 0;
    esp_err_t result = wqn::SyncDueProblemIds(token, &due_problem_ids, &total);
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(kTag, "due problem sync: returned=%u total=%d", static_cast<unsigned>(due_problem_ids.size()), total);
    if (due_problem_ids.empty()) {
        return ESP_OK;
    }

    std::vector<wqn::WqnProblem> problems;
    result = wqn::FetchProblems(token, due_problem_ids, &problems);
    if (result != ESP_OK) {
        return result;
    }

    for (const wqn::WqnProblem& problem : problems) {
        ESP_LOGI(kTag, "due problem ready: id=%s title=%s", problem.id.c_str(), problem.title.c_str());
    }
    return MergeProblemCache(problems, "due problems");
}
#endif

esp_err_t RefreshProblemIndexIfAvailable(const std::string& token)
{
    wqn::WqnProblemIndexRequest request;
    request.limit = WQN_SYNC_LIMIT;

    wqn::WqnProblemIndexPage page;
    const esp_err_t result = wqn::FetchProblemIndex(token, request, &page);
    if (result == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(kTag, "problem index endpoint is not available yet; will retry next sync round");
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(
        kTag,
        "problem index fetched: count=%u total=%d has_more=%s",
        static_cast<unsigned>(page.problems.size()),
        page.total,
        page.has_more ? "true" : "false");
    return MergeProblemCache(page.problems, "problem index");
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
    g_bootstrap_complete = true;
    g_bootstrap_request_id.clear();
    ESP_LOGI(
        kTag,
        "v3 bootstrap complete: config_revision=%llu sync_cursor=%llu",
        static_cast<unsigned long long>(g_config_revision),
        static_cast<unsigned long long>(g_sync_cursor));
    return ESP_OK;
}

esp_err_t SyncDueProblemsAndCacheV3(const std::string& token)
{
    if (g_sync_request_id.empty()) {
        g_sync_request_id = RandomControlId("req_sync_");
    }
    wqn::protocol::v3::RequestMetadata metadata = MakeControlMetadata();
    metadata.request_id = g_sync_request_id;
    wqn::protocol::v3::SyncData sync;
    wqn::protocol::v3::Error error;
    const esp_err_t sync_result =
        wqn::SyncDeviceControlV3(token, metadata, &sync, &error);
    if (sync_result != ESP_OK) {
        if (error.retryable) {
            g_control_retry_after_ms = error.retry_after_ms;
        }
        ESP_LOGW(kTag, "v3 sync failed: %s", esp_err_to_name(sync_result));
        return sync_result;
    }
    g_control_retry_after_ms = 0;
    if (sync.auto_sync_interval_minutes == 0 ||
        sync.auto_sync_interval_minutes == 15 ||
        sync.auto_sync_interval_minutes == 30 ||
        sync.auto_sync_interval_minutes == 60 ||
        sync.auto_sync_interval_minutes == 240) {
        uint32_t current_interval = 0;
        ESP_RETURN_ON_ERROR(
            wqn::LoadAutoSyncIntervalMinutes(&current_interval),
            kTag,
            "load v3 auto-sync configuration");
        if (current_interval != sync.auto_sync_interval_minutes) {
            ESP_RETURN_ON_ERROR(
                wqn::SaveAutoSyncIntervalMinutes(sync.auto_sync_interval_minutes),
                kTag,
                "save v3 auto-sync configuration");
        }
    }
    ESP_LOGI(
        kTag,
        "v3 sync summary: due=%u todos=%d words=%d cursor=%llu",
        static_cast<unsigned>(sync.due_problem_ids.size()),
        sync.todo_count,
        sync.word_due_count,
        static_cast<unsigned long long>(sync.sync_cursor));
    if (!sync.due_problem_ids.empty()) {
        std::vector<wqn::WqnProblem> problems;
        ESP_RETURN_ON_ERROR(
            wqn::FetchProblems(token, sync.due_problem_ids, &problems),
            kTag,
            "fetch v3 manifest problems");
        const esp_err_t cache_result = MergeProblemCache(problems, "v3 due problems");
        if (cache_result != ESP_OK) {
            ApplyStorageCapacityBackoff(cache_result, "problem-cache");
            ESP_RETURN_ON_ERROR(
                cache_result,
                kTag,
                "commit v3 manifest problems");
        }
    }

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
    g_sync_request_id.clear();
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
            if (!transport_failure && !word_error.retryable) {
                ESP_LOGW(
                    kTag,
                    "terminal word observation rejected; advancing sequence before quarantine: request=%s sequence=%llu code=%s",
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
                const bool sequence_consumed =
                    skip_result == ESP_OK ||
                    skip_error.code == "SEQUENCE_ALREADY_APPLIED" ||
                    skip_error.code == "SESSION_NOT_ACTIVE";
                if (!sequence_consumed) {
                    ESP_LOGW(
                        kTag,
                        "word observation skip deferred: request=%s sequence=%llu code=%s error=%s",
                        pending.request_id.c_str(),
                        static_cast<unsigned long long>(pending.sequence),
                        skip_error.code.empty() ? "TRANSPORT" : skip_error.code.c_str(),
                        esp_err_to_name(skip_result));
                    if (skip_transport_failure || skip_error.retryable ||
                        skip_error.code == "SEQUENCE_GAP") {
                        ScheduleWordOutboxRetry(
                            pending.request_id,
                            skip_error.retryable ? skip_error.retry_after_ms : 0);
                        return WordOutboxUploadState::kPending;
                    }
                    ESP_LOGE(
                        kTag,
                        "word observation skip failed terminally; leaving head for inspection: request=%s code=%s",
                        pending.request_id.c_str(),
                        skip_error.code.c_str());
                    return WordOutboxUploadState::kFailed;
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
                word_error.retryable ? word_error.retry_after_ms : 0);
            return WordOutboxUploadState::kPending;
        }
        result = wqn::AcknowledgeWordObservation(pending.request_id);
        if (result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "word outbox ack failed: request=%s error=%s",
                pending.request_id.c_str(),
                esp_err_to_name(result));
            ScheduleWordOutboxRetry(pending.request_id, 0);
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
            if (!transport_failure && !note_error.retryable) {
                ESP_LOGW(
                    kTag,
                    "terminal note observation rejected; advancing sequence before quarantine: request=%s sequence=%llu code=%s",
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
                const bool sequence_consumed =
                    skip_result == ESP_OK ||
                    skip_error.code == "SEQUENCE_ALREADY_APPLIED" ||
                    skip_error.code == "SESSION_NOT_ACTIVE";
                if (!sequence_consumed) {
                    ESP_LOGW(
                        kTag,
                        "note observation skip deferred: request=%s sequence=%llu code=%s error=%s",
                        pending.request_id.c_str(),
                        static_cast<unsigned long long>(pending.sequence),
                        skip_error.code.empty() ? "TRANSPORT" : skip_error.code.c_str(),
                        esp_err_to_name(skip_result));
                    if (skip_transport_failure || skip_error.retryable ||
                        skip_error.code == "SEQUENCE_GAP") {
                        ScheduleNoteOutboxRetry(
                            pending.request_id,
                            skip_error.retryable ? skip_error.retry_after_ms : 0);
                        return WordOutboxUploadState::kPending;
                    }
                    ESP_LOGE(
                        kTag,
                        "note observation skip failed terminally; leaving head for inspection: request=%s code=%s",
                        pending.request_id.c_str(),
                        skip_error.code.c_str());
                    return WordOutboxUploadState::kFailed;
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
                note_error.retryable ? note_error.retry_after_ms : 0);
            return WordOutboxUploadState::kPending;
        }
        result = wqn::AcknowledgeNoteObservation(pending.request_id);
        if (result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "note outbox ack failed: request=%s error=%s",
                pending.request_id.c_str(),
                esp_err_to_name(result));
            ScheduleNoteOutboxRetry(pending.request_id, 0);
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

bool RunWordOutboxOnlyRound()
{
    std::string token;
    if (!LoadUsableToken(&token)) {
        // Pairing/bootstrap owns identity recovery. Escalate the next round
        // instead of making the outbox-only path imitate the control plane.
        g_full_sync_requested.store(true, std::memory_order_release);
        g_last_word_outbox_upload_state = WordOutboxUploadState::kPending;
        return false;
    }
    g_last_word_outbox_upload_state = UploadPendingWordObservations(token);
    g_last_note_outbox_upload_state = UploadPendingNoteObservations(token);
    return g_last_word_outbox_upload_state != WordOutboxUploadState::kFailed &&
        g_last_note_outbox_upload_state != WordOutboxUploadState::kFailed;
}
#endif

bool RunSyncRound()
{
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    esp_err_t result = EnsureControlStateLoaded();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "v3 control checkpoint unavailable: %s", esp_err_to_name(result));
        return false;
    }
    result = RunDeviceClaimRoundV3();
    if (result == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(kTag, "v3 claim awaiting physical approval");
        return false;
    }
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "v3 claim round deferred: %s", esp_err_to_name(result));
        return false;
    }
#else
    esp_err_t result = wqn::RunPairingFlowIfNeeded();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pairing round deferred: %s", esp_err_to_name(result));
        return false;
    }
#endif

    std::string token;
    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "WQN online sync waiting for pairing");
        return false;
    }

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    result = BootstrapControlV3(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "v3 bootstrap round failed: %s", esp_err_to_name(result));
        return false;
    }
#endif

    result = UploadPendingReviewsIfAny(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pending review upload round failed: %s", esp_err_to_name(result));
        return false;
    }

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    g_last_word_outbox_upload_state = UploadPendingWordObservations(token);
    g_last_note_outbox_upload_state = UploadPendingNoteObservations(token);
#endif

    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "token cleared during review upload round");
        return false;
    }

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    result = SyncDueProblemsAndCacheV3(token);
#else
    result = SyncDueProblemsAndCache(token);
#endif
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "due problem sync round failed: %s", esp_err_to_name(result));
        return false;
    }

    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "token cleared during due problem sync round");
        return false;
    }

    result = RefreshProblemIndexIfAvailable(token);
    if (result != ESP_OK) {
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        ApplyStorageCapacityBackoff(result, "problem-index-cache");
#endif
        ESP_LOGW(kTag, "problem index refresh round failed: %s", esp_err_to_name(result));
        return false;
    }

    return true;
}

TickType_t NextSyncWaitDelay(bool round_synced, bool has_token_after_round)
{
    TickType_t wait_delay = portMAX_DELAY;
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    if (g_control_retry_after_ms > 0) {
        const uint32_t retry_after_ms = g_control_retry_after_ms;
        g_control_retry_after_ms = 0;
        wait_delay = pdMS_TO_TICKS(retry_after_ms);
    } else
#endif
    if (!has_token_after_round) {
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        const uint32_t wait_ms = g_claim_active
            ? ClaimPollDelayMs()
            : AddClaimJitter(kClaimRetryBaseMs);
        wait_delay = pdMS_TO_TICKS(wait_ms);
#else
        // [power-fix] Once the device has lost (or never had) an access
        // token it is in provisioning mode. Polling the server every 2s
        // serves no purpose -- the device cannot authenticate -- and it
        // keeps the CPU + radio hot for no benefit. Block on the
        // notification until something (e.g. a fresh token save in
        // wqn::SaveAccessToken) wakes us back up.
        wait_delay = portMAX_DELAY;
#endif
    } else if (round_synced) {
        wait_delay = wqn::services::GetConfiguredSyncDelayTicks();
    } else {
        const TickType_t configured_delay =
            wqn::services::GetConfiguredSyncDelayTicks();
        wait_delay = configured_delay == portMAX_DELAY
            ? portMAX_DELAY
            : kSyncRetryDelay;
    }

#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    if (has_token_after_round) {
        wait_delay = std::min(wait_delay, WordOutboxRetryWaitDelay());
        wait_delay = std::min(wait_delay, NoteOutboxRetryWaitDelay());
    }
#endif
    return wait_delay;
}

void SyncServiceTask(void*)
{
    ESP_LOGI(kTag, "SyncService task started");
    SetSyncTaskRunning();
    SetSyncStatus("idle");
    bool first_round = true;
    while (true) {
        if (first_round && wqn::services::GetConfiguredSyncDelayTicks() == portMAX_DELAY && wqn::services::HasUsableStoredToken()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        first_round = false;

        const bool full_requested =
            g_full_sync_requested.exchange(false, std::memory_order_acq_rel);
        const bool word_outbox_requested =
            g_word_outbox_sync_requested.exchange(false, std::memory_order_acq_rel);
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        const bool outbox_only = word_outbox_requested && !full_requested;
#else
        const bool outbox_only = false;
        (void)word_outbox_requested;
#endif

        wqn::runtime::SleepLease sleep_lease =
            wqn::runtime::SleepLease::TryAcquire(
                wqn::runtime::SleepBlocker::kOnlineSync, "sync-service", __FILE__, __LINE__);
        if (!sleep_lease) {
            SetSyncStatus("sleep-quiescing");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        SetSyncRoundStarted(esp_timer_get_time() / 1000);
        SetSyncStatus(outbox_only ? "word-outbox" : "syncing");
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        const bool synced = outbox_only
            ? RunWordOutboxOnlyRound()
            : RunSyncRound();
#else
        const bool synced = RunSyncRound();
#endif
        const int64_t finished_ms = esp_timer_get_time() / 1000;
        CompleteSyncRound(finished_ms, synced);
        const bool has_token_after_round = wqn::services::HasUsableStoredToken();
        if (synced) {
            SetSyncStatus("success");
            PublishSyncEvent(
                wqn::services::SyncEventStatus::kSucceeded,
                finished_ms,
                outbox_only
                    ? wqn::services::SyncEventScope::kWordOutbox
                    : wqn::services::SyncEventScope::kFull);
        } else {
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
        sleep_lease.Reset();
        TickType_t delay = NextSyncWaitDelay(synced, has_token_after_round);
#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
        if (g_last_word_outbox_upload_state == WordOutboxUploadState::kYielded ||
            g_last_note_outbox_upload_state == WordOutboxUploadState::kYielded) {
            // Do not turn a user-induced yield into the old 100 ms upload
            // loop. Resume only after another complete quiet period.
            if (g_word_outbox_timer == nullptr ||
                xTimerReset(g_word_outbox_timer, 0) != pdPASS) {
                g_word_outbox_sync_requested.store(true, std::memory_order_release);
                delay = std::min(delay, pdMS_TO_TICKS(100));
            }
        } else if (
            g_last_word_outbox_upload_state == WordOutboxUploadState::kPending ||
            g_last_note_outbox_upload_state == WordOutboxUploadState::kPending) {
            g_word_outbox_sync_requested.store(true, std::memory_order_release);
            const TickType_t retry_delay = std::min(
                WordOutboxRetryWaitDelay(), NoteOutboxRetryWaitDelay());
            delay = std::min(
                delay,
                retry_delay == portMAX_DELAY ? pdMS_TO_TICKS(100) : retry_delay);
        }
#endif
        if (delay == portMAX_DELAY) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            ulTaskNotifyTake(pdTRUE, delay);
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

esp_err_t StartSyncService()
{
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
    g_full_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
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
    if (g_word_outbox_timer != nullptr &&
        xTimerReset(g_word_outbox_timer, 0) == pdPASS) {
        return;
    }
    // Timer command queue pressure must not strand a durable observation.
    // Fall back to an immediate outbox-only notification.
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
#endif
}

void RequestNoteOutboxUpload()
{
    // Note observations share word's quiet-window timer and outbox-only round
    // (the round uploads both queues), so this reuses the same trigger path.
#if CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE
    if (g_word_outbox_timer != nullptr &&
        xTimerReset(g_word_outbox_timer, 0) == pdPASS) {
        return;
    }
    // Timer command queue pressure must not strand a durable observation.
    // Fall back to an immediate outbox-only notification.
    g_word_outbox_sync_requested.store(true, std::memory_order_release);
    if (g_sync_service_task != nullptr) {
        xTaskNotifyGive(g_sync_service_task);
    }
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
#else
    *snapshot = {};
    std::snprintf(snapshot->status, sizeof(snapshot->status), "%s", "wifi-disabled");
#endif
    uint32_t minutes = 0;
    if (LoadAutoSyncIntervalMinutes(&minutes) == ESP_OK) {
        snapshot->interval_minutes = minutes;
    }
}

TickType_t GetConfiguredSyncDelayTicks()
{
    uint32_t minutes = 0;
    if (LoadAutoSyncIntervalMinutes(&minutes) != ESP_OK || minutes == 0) {
        return portMAX_DELAY;
    }
    const uint64_t milliseconds = static_cast<uint64_t>(minutes) * 60ULL * 1000ULL;
    const uint64_t ticks = milliseconds / portTICK_PERIOD_MS;
    return ticks > static_cast<uint64_t>(portMAX_DELAY - 1) ? portMAX_DELAY - 1 : static_cast<TickType_t>(ticks);
}

}  // namespace wqn::services

namespace wqn::services {

void SetSyncEventSink(SyncEventSink sink)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    g_sync_event_sink.store(sink, std::memory_order_release);
#else
    (void)sink;
#endif
}

}  // namespace wqn::services
