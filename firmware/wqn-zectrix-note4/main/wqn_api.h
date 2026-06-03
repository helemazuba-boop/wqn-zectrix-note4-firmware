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
    int asset_count = 0;
    int solution_asset_count = 0;
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

struct WqnTodoItem {
    std::string id;
    std::string title;
    std::string status;
    std::string priority;
    std::string due_at;
    std::string reminder_at;
    std::string subject_name;
    std::string updated_at;
    std::string completed_at;
};

struct WqnTodoListPage {
    std::vector<WqnTodoItem> todos;
    std::string next_cursor;
    bool has_more = false;
    int total = 0;
    std::string server_time;
};

struct WqnAiAction {
    std::string type;
    std::string notebook_id;
    std::string note_id;
    std::string todo_id;
    std::string title;
    std::string status;
    std::string due_at;
    std::string reminder_at;
};

struct WqnAiStatusTraceItem {
    std::string stage;
    std::string status;
    std::string detail;
    int elapsed_ms = 0;
};

struct WqnAiAsrSummary {
    std::string provider;
    std::string model;
    std::string status;
    std::string text;
    std::string request_id;
    int elapsed_ms = 0;
};

struct WqnAiFunctionCallSummary {
    std::string name;
    std::string status;
    std::string display;
    std::string action_type;
    std::string title;
};

struct WqnAiChatResponse {
    std::string transcript;
    std::string reply_text;
    std::string conversation_id;
    std::string error_code;
    std::string error_message;
    int latency_ms = 0;
    std::vector<WqnAiAction> actions;
    std::vector<WqnAiStatusTraceItem> status_trace;
    WqnAiAsrSummary asr;
    std::vector<WqnAiFunctionCallSummary> function_calls;
};

esp_err_t RunPairingFlowIfNeeded();
esp_err_t ProbeSyncAndClearTokenOnUnauthorized(const std::string& token);
esp_err_t SyncDueProblemIds(const std::string& token, std::vector<std::string>* due_problem_ids, int* total);
esp_err_t FetchProblems(const std::string& token, const std::vector<std::string>& problem_ids, std::vector<WqnProblem>* problems);
esp_err_t FetchProblemIndex(const std::string& token, const WqnProblemIndexRequest& request, WqnProblemIndexPage* page);
esp_err_t UploadReviewComplete(const std::string& token, const std::vector<WqnReviewResult>& results);
esp_err_t FetchTodayPendingTodos(const std::string& token, WqnTodoListPage* page);
esp_err_t CompleteTodo(const std::string& token, const std::string& todo_id, const std::string& completed_at, WqnTodoItem* todo);
esp_err_t SyncDueProblemsAndLog(const std::string& token);
esp_err_t UploadAiAudioChat(
    const std::string& token,
    const uint8_t* pcm_data,
    size_t pcm_size,
    int duration_ms,
    const std::string& conversation_id,
    WqnAiChatResponse* response);

esp_err_t ParseTodoListResponse(const std::string& body, WqnTodoListPage* page);
esp_err_t ParseTodoCompleteResponse(const std::string& body, WqnTodoItem* todo);
esp_err_t ParseAiChatResponseBody(const std::string& body, WqnAiChatResponse* response);

}  // namespace wqn
