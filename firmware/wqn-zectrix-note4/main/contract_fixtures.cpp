#include "contract_fixtures.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "text_render.h"
#include "wqn_api.h"

namespace {

constexpr char kTag[] = "wqn_contract";

const char kPollPaired[] = R"json({
  "success": true,
  "data": {
    "status": "paired",
    "access_token": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kPollNoPending[] = R"json({
  "success": true,
  "data": { "status": "no_pending" },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kSyncDue[] = R"json({
  "success": true,
  "data": {
    "due_problems": [
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222"
    ],
    "total": 2
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kProblemDetails[] = R"json({
  "success": true,
  "data": {
    "problems": [
      {
        "id": "11111111-1111-4111-8111-111111111111",
        "title": "Linear equation",
        "content": "<p>Solve <strong>x + 2 = 5</strong>.</p>",
        "content_format": "esp32_text_v1",
        "content_text": "Solve x + 2 = 5.",
        "problem_type": "short",
        "answer_config": { "mode": "text" },
        "solution_text": "x = 3",
        "assets": [
          {
            "role": "problem",
            "kind": "image",
            "mime_type": "image/png",
            "url": "https://wqn.helema.cn/api/esp32/assets?path=user%2Fdemo%2Fproblems%2Fp1%2Fproblem%2Fscan.png",
            "sha256": "",
            "width": 0,
            "height": 0,
            "bytes": 0
          }
        ]
      }
    ]
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kProblemWithMath[] = R"json({
  "success": true,
  "data": {
    "problems": [
      {
        "id": "33333333-3333-4333-8333-333333333333",
        "title": "Quadratic",
        "content_format": "esp32_text_v1",
        "content_text": "求 x^2 >= 4 的解。",
        "problem_type": "short",
        "solution_text": "x <= -2 或 x >= 2"
      }
    ]
  }
})json";

const char kReviewComplete[] = R"json({
  "success": true,
  "data": { "processed": 1 },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kTodoList[] = R"json({
  "success": true,
  "data": {
    "todos": [
      {
        "id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "pending",
        "priority": "normal",
        "due_at": "2026-06-02T12:00:00.000Z",
        "reminder_at": null,
        "subject_name": "Math",
        "updated_at": "2026-06-01T00:00:00.000Z",
        "ignored_future_field": true
      }
    ],
    "next_cursor": null,
    "has_more": false,
    "total": 1,
    "server_time": "2026-06-01T00:00:00.000Z"
  }
})json";

const char kTodoComplete[] = R"json({
  "success": true,
  "data": {
    "todo": {
      "id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
      "title": "Review algebra",
      "status": "completed",
      "completed_at": "2026-06-01T00:00:00.000Z"
    }
  }
})json";

const char kAiTodoActions[] = R"json({
  "success": true,
  "data": {
    "transcript": "Add a todo",
    "reply_text": "Done",
    "conversation_id": "conversation-id",
    "latency_ms": 123,
    "actions": [
      {
        "type": "todo_created",
        "todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "pending",
        "due_at": "2026-06-02T12:00:00.000Z",
        "reminder_at": null,
        "unknown": "ignored"
      },
      {
        "type": "todo_status_updated",
        "todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "completed"
      },
      {
        "type": "future_action",
        "title": "Ignored but parsed"
      }
    ]
  }
})json";

const char kUnauthorizedError[] = R"json({
  "error": "invalid token",
  "status": 401,
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

class JsonDocument {
public:
    explicit JsonDocument(const char* payload) : root_(cJSON_Parse(payload)) {}
    ~JsonDocument() { cJSON_Delete(root_); }

    cJSON* root() const { return root_; }
    bool ok() const { return root_ != nullptr; }

private:
    cJSON* root_ = nullptr;
};

bool Require(bool condition, const char* message)
{
    if (!condition) {
        ESP_LOGE(kTag, "fixture check failed: %s", message);
    }
    return condition;
}

bool CheckSuccess(cJSON* root)
{
    const cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    return cJSON_IsBool(success) && cJSON_IsTrue(success);
}

bool CheckPollPaired()
{
    JsonDocument document(kPollPaired);
    if (!Require(document.ok(), "poll paired parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
    cJSON* token = cJSON_GetObjectItemCaseSensitive(data, "access_token");

    return Require(CheckSuccess(document.root()), "poll paired success") &&
           Require(cJSON_IsString(status) && std::strcmp(status->valuestring, "paired") == 0, "poll paired status") &&
           Require(cJSON_IsString(token) && std::strlen(token->valuestring) == 64, "poll paired token");
}

bool CheckPollNoPending()
{
    JsonDocument document(kPollNoPending);
    if (!Require(document.ok(), "poll no_pending parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");

    return Require(CheckSuccess(document.root()), "poll no_pending success") &&
           Require(cJSON_IsString(status) && std::strcmp(status->valuestring, "no_pending") == 0, "poll no_pending status");
}

bool CheckSyncDue()
{
    JsonDocument document(kSyncDue);
    if (!Require(document.ok(), "sync due parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* due = cJSON_GetObjectItemCaseSensitive(data, "due_problems");
    cJSON* total = cJSON_GetObjectItemCaseSensitive(data, "total");

    return Require(CheckSuccess(document.root()), "sync due success") &&
           Require(cJSON_IsArray(due) && cJSON_GetArraySize(due) == 2, "sync due ids") &&
           Require(cJSON_IsNumber(total) && total->valueint == 2, "sync due total");
}

bool CheckProblemDetails()
{
    JsonDocument document(kProblemDetails);
    if (!Require(document.ok(), "problem details parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* problems = cJSON_GetObjectItemCaseSensitive(data, "problems");
    cJSON* first = cJSON_GetArrayItem(problems, 0);
    cJSON* content = cJSON_GetObjectItemCaseSensitive(first, "content_text");
    cJSON* assets = cJSON_GetObjectItemCaseSensitive(first, "assets");

    const std::string text = cJSON_IsString(content) ? content->valuestring : "";
    return Require(CheckSuccess(document.root()), "problem details success") &&
           Require(cJSON_IsArray(problems) && cJSON_GetArraySize(problems) == 1, "problem details count") &&
           Require(text.find("x + 2 = 5") != std::string::npos, "problem device text") &&
           Require(cJSON_IsArray(assets) && cJSON_GetArraySize(assets) == 1, "problem image asset manifest");
}

bool CheckProblemMathFallback()
{
    JsonDocument document(kProblemWithMath);
    if (!Require(document.ok(), "problem math parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* problems = cJSON_GetObjectItemCaseSensitive(data, "problems");
    cJSON* first = cJSON_GetArrayItem(problems, 0);
    cJSON* content = cJSON_GetObjectItemCaseSensitive(first, "content_text");
    cJSON* solution = cJSON_GetObjectItemCaseSensitive(first, "solution_text");

    return Require(CheckSuccess(document.root()), "problem math success") &&
           Require(cJSON_IsString(content) && std::strstr(content->valuestring, "x^2 >= 4") != nullptr,
                   "problem math content fallback") &&
           Require(cJSON_IsString(solution) && std::strstr(solution->valuestring, "x <= -2") != nullptr,
                   "problem math solution fallback");
}

bool CheckReviewComplete()
{
    JsonDocument document(kReviewComplete);
    if (!Require(document.ok(), "review complete parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* processed = cJSON_GetObjectItemCaseSensitive(data, "processed");

    return Require(CheckSuccess(document.root()), "review complete success") &&
           Require(cJSON_IsNumber(processed) && processed->valueint == 1, "review complete processed");
}

bool CheckTodoList()
{
    wqn::WqnTodoListPage page;
    const esp_err_t result = wqn::ParseTodoListResponse(kTodoList, &page);
    return Require(result == ESP_OK, "todo list parse result") &&
           Require(page.todos.size() == 1, "todo list count") &&
           Require(page.total == 1, "todo list total") &&
           Require(page.todos[0].id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "todo id") &&
           Require(page.todos[0].title == "Review algebra", "todo title") &&
           Require(page.todos[0].status == "pending", "todo pending status") &&
           Require(page.todos[0].due_at == "2026-06-02T12:00:00.000Z", "todo due_at") &&
           Require(page.todos[0].subject_name == "Math", "todo subject");
}

bool CheckTodoComplete()
{
    wqn::WqnTodoItem todo;
    const esp_err_t result = wqn::ParseTodoCompleteResponse(kTodoComplete, &todo);
    return Require(result == ESP_OK, "todo complete parse result") &&
           Require(todo.id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "todo complete id") &&
           Require(todo.status == "completed", "todo complete status") &&
           Require(todo.completed_at == "2026-06-01T00:00:00.000Z", "todo completed_at");
}

bool CheckAiTodoActions()
{
    wqn::WqnAiChatResponse response;
    const esp_err_t result = wqn::ParseAiChatResponseBody(kAiTodoActions, &response);
    return Require(result == ESP_OK, "AI todo action parse result") &&
           Require(response.transcript == "Add a todo", "AI transcript") &&
           Require(response.reply_text == "Done", "AI reply_text") &&
           Require(response.actions.size() == 3, "AI action count") &&
           Require(response.actions[0].type == "todo_created", "AI create action type") &&
           Require(response.actions[0].todo_id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "AI todo_id") &&
           Require(response.actions[0].status == "pending", "AI todo pending status") &&
           Require(response.actions[0].due_at == "2026-06-02T12:00:00.000Z", "AI due_at") &&
           Require(response.actions[1].type == "todo_status_updated", "AI update action type") &&
           Require(response.actions[1].status == "completed", "AI completed status") &&
           Require(response.actions[2].type == "future_action", "AI unknown action preserved");
}

bool CheckUnauthorizedError()
{
    JsonDocument document(kUnauthorizedError);
    if (!Require(document.ok(), "error parses")) {
        return false;
    }

    cJSON* error = cJSON_GetObjectItemCaseSensitive(document.root(), "error");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(document.root(), "status");

    return Require(cJSON_IsString(error), "error message") &&
           Require(cJSON_IsNumber(status) && status->valueint == 401, "error status");
}

}  // namespace

namespace wqn {

bool RunContractFixtureSelfTest()
{
    const bool ok =
        CheckPollPaired() &&
        CheckPollNoPending() &&
        CheckSyncDue() &&
        CheckProblemDetails() &&
        CheckProblemMathFallback() &&
        CheckReviewComplete() &&
        CheckTodoList() &&
        CheckTodoComplete() &&
        CheckAiTodoActions() &&
        CheckUnauthorizedError();

    if (ok) {
        ESP_LOGI(kTag, "contract fixture self-test passed");
    }
    return ok;
}

}  // namespace wqn
