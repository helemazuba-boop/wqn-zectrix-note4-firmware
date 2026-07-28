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

struct CachedProblem {
    std::string id;
    std::string title;
    std::string type;
    std::string status;
    std::string content_text;
    std::string solution_text;
    int asset_count = 0;
    int solution_asset_count = 0;
    std::string updated_at;
};

struct PendingReviewResult {
    std::string problem_id;
    std::string selected_status;
    bool is_correct = false;
    std::string submitted_answer;
    std::string created_at;
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

esp_err_t SaveProblems(const std::vector<CachedProblem>& problems);
esp_err_t LoadProblems(std::vector<CachedProblem>* problems);
esp_err_t ClearProblems();

esp_err_t EnqueueReviewResult(const PendingReviewResult& result);
esp_err_t LoadPendingReviewResults(std::vector<PendingReviewResult>* results);
esp_err_t ClearPendingReviewResults();

esp_err_t SaveAiSessionForDay(const CachedAiSession& session);
esp_err_t LoadAiSessionForDay(const std::string& day, CachedAiSession* session);
esp_err_t ClearAiSession();

esp_err_t LoadAutoSyncIntervalMinutes(uint32_t* minutes);
esp_err_t SaveAutoSyncIntervalMinutes(uint32_t minutes);
std::string AutoSyncIntervalLabel(uint32_t minutes);
// Default word deck for the device (empty = all decks). The word page's
// study sessions scope to it; the other decks enter via the note screen's
// mixed [词] rows.
esp_err_t LoadDefaultWordDeckId(std::string* deck_id);
esp_err_t SaveDefaultWordDeckId(const std::string& deck_id);
esp_err_t LoadVolumePercent(int* percent);
esp_err_t SaveVolumePercent(int percent);
std::string VolumeLabel(int percent);
int GetPlaybackVolumePercent();  // cached level (0-100); applied to ES8311 DAC registers
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
