#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

struct CachedProblem {
    std::string id;
    std::string title;
    std::string type;
    std::string content_text;
    std::string solution_text;
    std::string updated_at;
};

struct PendingReviewResult {
    std::string problem_id;
    std::string selected_status;
    bool is_correct = false;
    std::string submitted_answer;
    std::string created_at;
};

esp_err_t InitStorage();
esp_err_t LoadAccessToken(std::string* token);
esp_err_t SaveAccessToken(const std::string& token);
esp_err_t ClearAccessToken();
bool IsValidAccessToken(const std::string& token);
std::string MaskTokenForLog(const std::string& token);

esp_err_t SaveProblems(const std::vector<CachedProblem>& problems);
esp_err_t LoadProblems(std::vector<CachedProblem>* problems);
esp_err_t ClearProblems();

esp_err_t EnqueueReviewResult(const PendingReviewResult& result);
esp_err_t LoadPendingReviewResults(std::vector<PendingReviewResult>* results);
esp_err_t ClearPendingReviewResults();

}  // namespace wqn
