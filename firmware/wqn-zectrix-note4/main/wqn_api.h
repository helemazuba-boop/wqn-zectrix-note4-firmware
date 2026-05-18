#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

struct WqnAssetManifestItem {
    std::string role;
    std::string kind;
    std::string mime_type;
    std::string url;
    std::string sha256;
    int width = 0;
    int height = 0;
    int bytes = 0;
};

struct WqnProblem {
    std::string id;
    std::string title;
    std::string problem_type;
    std::string status;
    std::string subject_id;
    std::string subject_name;
    std::string updated_at;
    std::string next_review_at;
    std::string content_text;
    std::string solution_text;
    std::vector<WqnAssetManifestItem> assets;
    std::vector<WqnAssetManifestItem> solution_assets;
};

struct WqnProblemIndexRequest {
    std::string cursor;
    std::string status;
    std::string subject_id;
    int limit = 50;
};

struct WqnProblemIndexPage {
    std::vector<WqnProblem> problems;
    std::string next_cursor;
    bool has_more = false;
    int total = 0;
};

struct WqnReviewResult {
    std::string problem_id;
    std::string selected_status;
    std::string reviewed_at;
    int duration_ms = 0;
};

esp_err_t RunPairingFlowIfNeeded();
esp_err_t ProbeSyncAndClearTokenOnUnauthorized(const std::string& token);
esp_err_t SyncDueProblemIds(const std::string& token, std::vector<std::string>* due_problem_ids, int* total);
esp_err_t FetchProblems(const std::string& token, const std::vector<std::string>& problem_ids, std::vector<WqnProblem>* problems);
esp_err_t FetchProblemIndex(const std::string& token, const WqnProblemIndexRequest& request, WqnProblemIndexPage* page);
esp_err_t UploadReviewComplete(const std::string& token, const std::vector<WqnReviewResult>& results);
esp_err_t SyncDueProblemsAndLog(const std::string& token);

}  // namespace wqn
