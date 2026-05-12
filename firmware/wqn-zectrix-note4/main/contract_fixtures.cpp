#include "contract_fixtures.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "text_render.h"

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
        "problem_type": "short",
        "answer_config": { "mode": "text" },
        "solution_text": "<p>x = 3</p>"
      }
    ]
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kReviewComplete[] = R"json({
  "success": true,
  "data": { "processed": 1 },
  "timestamp": "2026-05-10T12:00:00.000Z"
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
    cJSON* content = cJSON_GetObjectItemCaseSensitive(first, "content");

    const std::string text = cJSON_IsString(content) ? wqn::HtmlToPlainText(content->valuestring) : "";
    return Require(CheckSuccess(document.root()), "problem details success") &&
           Require(cJSON_IsArray(problems) && cJSON_GetArraySize(problems) == 1, "problem details count") &&
           Require(text.find("x + 2 = 5") != std::string::npos, "problem html plain text");
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
        CheckReviewComplete() &&
        CheckUnauthorizedError();

    if (ok) {
        ESP_LOGI(kTag, "contract fixture self-test passed");
    }
    return ok;
}

}  // namespace wqn
