#include "storage.h"

#include <vector>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "wqn_storage";
constexpr size_t kAccessTokenLength = 64;
constexpr char kProblemsKey[] = "problems";
constexpr char kPendingReviewsKey[] = "pending_reviews";

struct NvsHandle {
    ~NvsHandle()
    {
        if (handle != 0) {
            nvs_close(handle);
        }
    }

    nvs_handle_t handle = 0;
};

class JsonDocument {
public:
    explicit JsonDocument(cJSON* root) : root_(root) {}
    explicit JsonDocument(const std::string& payload) : root_(cJSON_Parse(payload.c_str())) {}
    ~JsonDocument() { cJSON_Delete(root_); }

    cJSON* root() const { return root_; }

    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;

private:
    cJSON* root_ = nullptr;
};

std::string GetOptionalString(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

bool GetOptionalBool(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) && cJSON_IsTrue(item);
}

esp_err_t LoadStringFromNvs(const char* key, std::string* value)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    NvsHandle nvs;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        value->clear();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    size_t required_size = 0;
    result = nvs_get_str(nvs.handle, key, nullptr, &required_size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        value->clear();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "measure NVS string");

    std::vector<char> buffer(required_size);
    ESP_RETURN_ON_ERROR(nvs_get_str(nvs.handle, key, buffer.data(), &required_size), kTag, "read NVS string");

    *value = std::string(buffer.data());
    return ESP_OK;
}

esp_err_t SaveStringToNvs(const char* key, const std::string& value)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_str(nvs.handle, key, value.c_str());
    if (result == ESP_OK) {
        result = nvs_commit(nvs.handle);
    }
    return result;
}

esp_err_t LoadBlobFromNvs(const char* key, std::string* value)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    NvsHandle nvs;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        value->clear();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    size_t required_size = 0;
    result = nvs_get_blob(nvs.handle, key, nullptr, &required_size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        value->clear();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "measure NVS blob");

    std::vector<char> buffer(required_size);
    ESP_RETURN_ON_ERROR(nvs_get_blob(nvs.handle, key, buffer.data(), &required_size), kTag, "read NVS blob");

    value->assign(buffer.data(), required_size);
    return ESP_OK;
}

esp_err_t SaveBlobToNvs(const char* key, const std::string& value)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_blob(nvs.handle, key, value.data(), value.size());
    if (result == ESP_OK) {
        result = nvs_commit(nvs.handle);
    }
    return result;
}

esp_err_t ClearNvsKey(const char* key)
{
    NvsHandle nvs;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    result = nvs_erase_key(nvs.handle, key);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_commit(nvs.handle);
    }
    return result;
}

esp_err_t JsonToString(cJSON* root, std::string* output)
{
    if (root == nullptr || output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    char* rendered = cJSON_PrintUnformatted(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *output = rendered;
    cJSON_free(rendered);
    return ESP_OK;
}

esp_err_t ParseArrayPayload(const std::string& payload, cJSON** array)
{
    if (array == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    JsonDocument document(payload);
    if (!cJSON_IsArray(document.root())) {
        return ESP_ERR_INVALID_STATE;
    }

    *array = cJSON_Duplicate(document.root(), true);
    return *array != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

}  // namespace

namespace wqn {

bool IsValidAccessToken(const std::string& token)
{
    if (token.size() != kAccessTokenLength) {
        return false;
    }
    for (const char c : token) {
        const bool hex =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

esp_err_t InitStorage()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS requires erase before init: %s", esp_err_to_name(result));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "erase NVS");
        result = nvs_flash_init();
    }

    ESP_RETURN_ON_ERROR(result, kTag, "init NVS");
    ESP_LOGI(kTag, "NVS ready");
    return ESP_OK;
}

esp_err_t LoadAccessToken(std::string* token)
{
    return LoadStringFromNvs(WQN_NVS_ACCESS_TOKEN_KEY, token);
}

esp_err_t SaveAccessToken(const std::string& token)
{
    if (!IsValidAccessToken(token)) {
        return ESP_ERR_INVALID_ARG;
    }

    return SaveStringToNvs(WQN_NVS_ACCESS_TOKEN_KEY, token);
}

esp_err_t ClearAccessToken()
{
    return ClearNvsKey(WQN_NVS_ACCESS_TOKEN_KEY);
}

std::string MaskTokenForLog(const std::string& token)
{
    if (token.size() < 12) {
        return "<invalid-token>";
    }
    return token.substr(0, 4) + "..." + token.substr(token.size() - 4);
}

esp_err_t SaveProblems(const std::vector<CachedProblem>& problems)
{
    JsonDocument document(cJSON_CreateArray());
    if (document.root() == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    for (const CachedProblem& problem : problems) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr ||
            !cJSON_AddStringToObject(item, "id", problem.id.c_str()) ||
            !cJSON_AddStringToObject(item, "title", problem.title.c_str()) ||
            !cJSON_AddStringToObject(item, "type", problem.type.c_str()) ||
            !cJSON_AddStringToObject(item, "content_text", problem.content_text.c_str()) ||
            !cJSON_AddStringToObject(item, "solution_text", problem.solution_text.c_str()) ||
            !cJSON_AddStringToObject(item, "updated_at", problem.updated_at.c_str())) {
            cJSON_Delete(item);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(document.root(), item);
    }

    std::string payload;
    ESP_RETURN_ON_ERROR(JsonToString(document.root(), &payload), kTag, "serialize problem cache");
    return SaveBlobToNvs(kProblemsKey, payload);
}

esp_err_t LoadProblems(std::vector<CachedProblem>* problems)
{
    if (problems == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::string payload;
    ESP_RETURN_ON_ERROR(LoadBlobFromNvs(kProblemsKey, &payload), kTag, "load problem cache");
    problems->clear();
    if (payload.empty()) {
        return ESP_OK;
    }

    cJSON* array = nullptr;
    ESP_RETURN_ON_ERROR(ParseArrayPayload(payload, &array), kTag, "parse problem cache");
    JsonDocument document(array);

    const int count = cJSON_GetArraySize(document.root());
    problems->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(document.root(), i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        CachedProblem problem;
        problem.id = GetOptionalString(item, "id");
        problem.title = GetOptionalString(item, "title");
        problem.type = GetOptionalString(item, "type");
        problem.content_text = GetOptionalString(item, "content_text");
        problem.solution_text = GetOptionalString(item, "solution_text");
        problem.updated_at = GetOptionalString(item, "updated_at");
        problems->push_back(problem);
    }
    return ESP_OK;
}

esp_err_t ClearProblems()
{
    return ClearNvsKey(kProblemsKey);
}

esp_err_t EnqueueReviewResult(const PendingReviewResult& result)
{
    if (result.problem_id.empty() || result.selected_status.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    std::vector<PendingReviewResult> results;
    ESP_RETURN_ON_ERROR(LoadPendingReviewResults(&results), kTag, "load pending reviews");
    results.push_back(result);

    JsonDocument document(cJSON_CreateArray());
    if (document.root() == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    for (const PendingReviewResult& pending : results) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr ||
            !cJSON_AddStringToObject(item, "problem_id", pending.problem_id.c_str()) ||
            !cJSON_AddStringToObject(item, "selected_status", pending.selected_status.c_str()) ||
            cJSON_AddBoolToObject(item, "is_correct", pending.is_correct) == nullptr ||
            !cJSON_AddStringToObject(item, "submitted_answer", pending.submitted_answer.c_str()) ||
            !cJSON_AddStringToObject(item, "created_at", pending.created_at.c_str())) {
            cJSON_Delete(item);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(document.root(), item);
    }

    std::string payload;
    ESP_RETURN_ON_ERROR(JsonToString(document.root(), &payload), kTag, "serialize pending reviews");
    return SaveBlobToNvs(kPendingReviewsKey, payload);
}

esp_err_t LoadPendingReviewResults(std::vector<PendingReviewResult>* results)
{
    if (results == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::string payload;
    ESP_RETURN_ON_ERROR(LoadBlobFromNvs(kPendingReviewsKey, &payload), kTag, "load pending reviews");
    results->clear();
    if (payload.empty()) {
        return ESP_OK;
    }

    cJSON* array = nullptr;
    ESP_RETURN_ON_ERROR(ParseArrayPayload(payload, &array), kTag, "parse pending reviews");
    JsonDocument document(array);

    const int count = cJSON_GetArraySize(document.root());
    results->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(document.root(), i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        PendingReviewResult pending;
        pending.problem_id = GetOptionalString(item, "problem_id");
        pending.selected_status = GetOptionalString(item, "selected_status");
        pending.is_correct = GetOptionalBool(item, "is_correct");
        pending.submitted_answer = GetOptionalString(item, "submitted_answer");
        pending.created_at = GetOptionalString(item, "created_at");
        results->push_back(pending);
    }
    return ESP_OK;
}

esp_err_t ClearPendingReviewResults()
{
    return ClearNvsKey(kPendingReviewsKey);
}

}  // namespace wqn
