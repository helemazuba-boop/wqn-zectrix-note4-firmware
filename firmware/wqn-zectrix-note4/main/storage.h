#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

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

esp_err_t InitStorage();
bool ReadStorageCapacitySnapshot(StorageCapacitySnapshot* snapshot);
esp_err_t LoadAccessToken(std::string* token);
esp_err_t SaveAccessToken(const std::string& token);
esp_err_t ClearAccessToken();
bool IsValidAccessToken(const std::string& token);
std::string MaskTokenForLog(const std::string& token);
esp_err_t LoadDeviceControlState(DeviceControlState* state);
esp_err_t SaveDeviceControlState(const DeviceControlState& state);

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
std::string AutoSyncIntervalLabel(uint32_t minutes);
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

// PowerCoordinator boundary. Writes are serialized by StorageService and each
// accepted transaction holds kStorage, so Ready means every commit is durable.
esp_err_t PrepareStorageForSleep(int64_t deadline_us);
void RollbackStorageAfterSleepAbort();

}  // namespace wqn
