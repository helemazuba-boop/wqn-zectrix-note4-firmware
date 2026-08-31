#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

enum class ImageRenderMode : uint8_t {
    kBlackWhite = 0,
    kGray16 = 1,
};

struct StorageCapacitySnapshot {
    bool spiffs_valid = false;
    size_t spiffs_total_bytes = 0;
    size_t spiffs_used_bytes = 0;
    bool nvs_valid = false;
    size_t nvs_used_entries = 0;
    size_t nvs_free_entries = 0;
    size_t nvs_total_entries = 0;
};

struct CachedAiSession {
    std::string day;
    std::string conversation_id;
    std::string transcript;
    std::string reply_text;
    std::string status_detail;
    std::vector<std::string> function_call_summaries;
    int latency_ms = 0;
};

struct DeviceControlState {
    uint64_t config_revision = 0;
    uint64_t sync_cursor = 0;
};

enum class SyncJournalPhase : uint8_t {
    kClean = 0,
    kPending = 1,
    kFetching = 2,
    kInstalling = 3,
    kBackoff = 4,
    kBlocked = 5,
};

struct SyncJournalContentState {
    uint64_t desired_revision = 0;
    uint64_t applied_revision = 0;
    SyncJournalPhase phase = SyncJournalPhase::kClean;
    uint8_t retry_attempt = 0;
    uint64_t retry_not_before_unix_seconds = 0;
    char desired_snapshot_id[65] = {};
    char active_snapshot_id[65] = {};
};

struct SyncJournalRetryState {
    uint64_t not_before_unix_seconds = 0;
    uint8_t attempt = 0;
};

struct SyncJournalOutboxRetryState {
    char request_id[65] = {};
    uint64_t not_before_unix_seconds = 0;
    uint8_t attempt = 0;
    uint8_t cause = 0;
};

struct SyncJournal {
    uint32_t schema_version = 2;
    uint64_t config_revision = 0;
    uint64_t sync_cursor = 0;
    SyncJournalRetryState full_sync_retry = {};
    SyncJournalContentState word_packs = {};
    SyncJournalContentState note_packs = {};
    SyncJournalContentState problem_packs = {};
    SyncJournalOutboxRetryState word_outbox = {};
    SyncJournalOutboxRetryState note_outbox = {};
    SyncJournalOutboxRetryState problem_outbox = {};
    char protocol_blocked_image_id[65] = {};
};

esp_err_t InitStorage();
bool ReadStorageCapacitySnapshot(StorageCapacitySnapshot* snapshot);
// Reclaims only disposable files (stale temps and the shared note/problem
// image cache), runs bounded SPIFFS GC, then requires `required_bytes` plus a
// fixed safety reserve. Referenced pack/manifests are never removed here.
esp_err_t EnsurePackDownloadCapacity(
    size_t required_bytes,
    size_t safety_reserve_bytes = 256U * 1024U);
esp_err_t LoadAccessToken(std::string* token);
esp_err_t SaveAccessToken(const std::string& token);
esp_err_t ClearAccessToken();
bool IsValidAccessToken(const std::string& token);
std::string MaskTokenForLog(const std::string& token);
esp_err_t LoadDeviceControlState(DeviceControlState* state);
esp_err_t SaveDeviceControlState(const DeviceControlState& state);
// Durable coordinator checkpoint. The file is committed through a
// temp/backup/rename sequence and is safe to replay after a power cut.
esp_err_t LoadSyncJournal(SyncJournal* journal);
esp_err_t SaveSyncJournal(const SyncJournal& journal);

esp_err_t SaveAiSessionForDay(const CachedAiSession& session);
esp_err_t LoadAiSessionForDay(const std::string& day, CachedAiSession* session);
esp_err_t ClearAiSession();

esp_err_t LoadAutoSyncIntervalMinutes(uint32_t* minutes);
esp_err_t SaveAutoSyncIntervalMinutes(uint32_t minutes);
// [persist-worker] Worker-dedicated variant: routes the NVS commit through the
// FOREGROUND storage queue so the persist worker's wait behind background pack
// writes is bounded (once a transaction starts it may still wait without a
// fixed deadline). UI code must not call this synchronously.
esp_err_t SaveAutoSyncIntervalMinutesForeground(uint32_t minutes);
esp_err_t LoadBootFullSyncAttemptUnixSeconds(int64_t* seconds);
esp_err_t SaveBootFullSyncAttemptUnixSeconds(int64_t seconds);
std::string AutoSyncIntervalLabel(uint32_t minutes);
esp_err_t LoadImageRenderMode(ImageRenderMode* mode);
esp_err_t SaveImageRenderModeForeground(ImageRenderMode mode);
std::string ImageRenderModeLabel(ImageRenderMode mode);
// Default word deck for the device (empty = all decks). The word page's
// study sessions scope to it; the other decks enter via the note screen's
// mixed [词] rows.
esp_err_t LoadDefaultWordDeckId(std::string* deck_id);
esp_err_t SaveDefaultWordDeckId(const std::string& deck_id);
// [deck-scope] Recoverable default-deck change protocol (c5). A deck switch
// must clear both persisted word sessions (SPIFFS) and save the new deck +
// scope generation (NVS); there is no cross-NVS/SPIFFS atomicity, so a marker
// makes the sequence recoverable instead: write marker -> clear sessions ->
// save deck+generation -> clear marker. A power cut leaves the marker behind
// and boot replays the (idempotent) tail. Runs the whole sequence as ONE
// foreground storage transaction on the persist worker (never call from the
// UI task synchronously).
esp_err_t ChangeDefaultWordDeckForeground(const std::string& deck_id);
// Boot-time recovery + generation-cache seeding. Call once in InitStorage
// (after StartStorageService, before any word session loads). Idempotent.
esp_err_t RecoverDefaultDeckScopeChange();
// Current deck-scope generation (RAM cache of the NVS value, seeded at boot,
// bumped when a deck change commits). Sessions stamp it on save and are
// rejected on load when it no longer matches (second line of defense behind
// the marker protocol). Atomic; safe from any task.
uint32_t GetDeckScopeGeneration();
esp_err_t LoadVolumePercent(int* percent);
esp_err_t SaveVolumePercent(int percent);
// [persist-worker] Worker-dedicated variant (see SaveAutoSyncIntervalMinutesForeground).
esp_err_t SaveVolumePercentForeground(int percent);
std::string VolumeLabel(int percent);
int GetPlaybackVolumePercent();  // cached level (0-100); applied to ES8311 DAC registers
// [persist-worker] Update the runtime playback cache without a durable write.
// The UI thread calls this right after a successful volume-save reserve/enqueue
// so playback uses the new level immediately (the durable NVS commit runs async
// on the persist worker). Atomic; safe from any task.
void SetPlaybackVolumeCache(int percent);
esp_err_t FactoryResetNvsAndRestart();

esp_err_t LoadWifiCredentials(std::string* ssid, std::string* password);
esp_err_t SaveWifiCredentials(const std::string& ssid, const std::string& password);
esp_err_t ClearWifiCredentials();
bool HasWifiCredentials();

// [wifi-redundancy] Dual-slot WiFi credential store. The store is a versioned
// NVS blob holding up to two (ssid, password) slots plus a `preferred` index
// pointing at the last slot that connected successfully. Persistence is a
// single atomic blob commit (the legacy per-key wifi_ssid/wifi_pass pair could
// tear across power loss). Load migrates legacy keys on first read.
struct WifiCredentialSlot {
    char ssid[33];       // 32 + NUL
    char password[65];   // 64 + NUL
};
struct WifiCredentialStore {
    uint8_t version = 0;    // kWifiCredentialStoreVersion when valid
    uint8_t preferred = 0;  // index of last successfully-connected slot
    uint8_t count = 0;      // 0..2 occupied slots
    WifiCredentialSlot slots[2] = {};
};

// Stable roles exposed by the provisioning UI. `kPrimary` addresses the
// preferred slot; `kBackup` addresses the other slot without changing which
// network is preferred.
enum class WifiCredentialRole : uint8_t {
    kPrimary = 0,
    kBackup,
};

// Loads the store, validating the blob and migrating legacy wifi_ssid/wifi_pass
// keys when the blob is absent. On success `store` always holds a coherent
// (possibly empty) store with version == 1. Returns ESP_OK when a valid store
// (blob or migrated) was loaded; legacy migration with no keys yields an empty
// store and ESP_OK.
esp_err_t LoadWifiCredentialStore(WifiCredentialStore* store);
// Persists the whole store as one atomic NVS blob commit.
esp_err_t SaveWifiCredentialStore(const WifiCredentialStore& store);
// Insert or update a credential: same-SSID slots get their password refreshed,
// a free slot is appended when available, otherwise the non-preferred slot is
// replaced. The touched slot becomes preferred. No-op writes (identical
// ssid+password already preferred) skip the NVS commit.
esp_err_t UpsertWifiCredential(const std::string& ssid, const std::string& password);
// Updates one provisioning role explicitly. `keep_existing_password` is only
// accepted when the submitted SSID still matches the credential currently in
// that role. A backup cannot be created before a primary credential exists.
esp_err_t SetWifiCredentialForRole(
    WifiCredentialRole role,
    const std::string& ssid,
    const std::string& password,
    bool keep_existing_password);
// Marks `index` as the preferred (last-good) slot. Writes only when the value
// actually changes, to bound NVS wear.
esp_err_t MarkWifiSlotPreferred(uint8_t index);

// PowerCoordinator boundary. Writes are serialized by StorageService and each
// accepted transaction holds kStorage, so Ready means every commit is durable.
esp_err_t PrepareStorageForSleep(int64_t deadline_us);
void RollbackStorageAfterSleepAbort();

}  // namespace wqn
