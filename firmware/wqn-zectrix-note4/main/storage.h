#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

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

esp_err_t InitStorage();
esp_err_t LoadAccessToken(std::string* token);
esp_err_t SaveAccessToken(const std::string& token);
esp_err_t ClearAccessToken();
bool IsValidAccessToken(const std::string& token);
bool IsAccessTokenExpired();
std::string MaskTokenForLog(const std::string& token);

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
esp_err_t FactoryResetNvsAndRestart();

esp_err_t LoadWifiCredentials(std::string* ssid, std::string* password);
esp_err_t SaveWifiCredentials(const std::string& ssid, const std::string& password);
esp_err_t ClearWifiCredentials();
bool HasWifiCredentials();

}  // namespace wqn
