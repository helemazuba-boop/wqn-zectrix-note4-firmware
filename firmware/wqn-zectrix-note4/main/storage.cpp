#include "storage.h"

#include <cstdint>
#include <ctime>
#include <vector>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "wqn_storage";
constexpr size_t kAccessTokenLength = 64;
constexpr char kProblemsKey[] = "problems";
constexpr char kPendingReviewsKey[] = "pending_reviews";
constexpr char kAiSessionKey[] = "ai_session_day";
constexpr char kAccessTokenSavedAtKey[] = "access_token_saved_at";
constexpr char kAccessTokenExpiresAtKey[] = "access_token_expires_at";
constexpr uint64_t kAccessTokenMaxAgeSeconds = 30ULL * 24ULL * 60ULL * 60ULL;
constexpr std::time_t kMinReasonableUnixTime = 1704067200;  // 2024-01-01 UTC

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

int GetOptionalInt(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
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

esp_err_t LoadU64FromNvs(const char* key, uint64_t* value, bool* found)
{
    if (value == nullptr || found == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *value = 0;
    *found = false;
    NvsHandle nvs;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    result = nvs_get_u64(nvs.handle, key, value);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "read NVS u64");
    *found = true;
    return ESP_OK;
}

esp_err_t SaveU64ToNvs(const char* key, uint64_t value)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_u64(nvs.handle, key, value);
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

uint64_t CurrentUnixTime()
{
    std::time_t now = 0;
    std::time(&now);
    return now > 0 ? static_cast<uint64_t>(now) : 0;
}

bool ClockIsReasonable()
{
    return CurrentUnixTime() >= static_cast<uint64_t>(kMinReasonableUnixTime);
}

esp_err_t ClearAccessTokenKeys()
{
    esp_err_t result = ClearNvsKey(WQN_NVS_ACCESS_TOKEN_KEY);
    const esp_err_t clear_saved_at = ClearNvsKey(kAccessTokenSavedAtKey);
    const esp_err_t clear_expires_at = ClearNvsKey(kAccessTokenExpiresAtKey);
    if (result == ESP_OK) {
        result = clear_saved_at;
    }
    if (result == ESP_OK) {
        result = clear_expires_at;
    }
    return result;
}

esp_err_t SaveAccessTokenMetadata(uint64_t now)
{
    if (now < static_cast<uint64_t>(kMinReasonableUnixTime)) {
        ESP_LOGW(kTag, "clock is not synced; token expiry metadata not written");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(SaveU64ToNvs(kAccessTokenSavedAtKey, now), kTag, "save token saved_at");
    return SaveU64ToNvs(kAccessTokenExpiresAtKey, now + kAccessTokenMaxAgeSeconds);
}

esp_err_t EnsureAccessTokenFresh(std::string* token)
{
    if (token == nullptr || token->empty() || !wqn::IsValidAccessToken(*token)) {
        return ESP_OK;
    }

    uint64_t expires_at = 0;
    bool has_expires_at = false;
    ESP_RETURN_ON_ERROR(LoadU64FromNvs(kAccessTokenExpiresAtKey, &expires_at, &has_expires_at), kTag, "load token expires_at");

    const uint64_t now = CurrentUnixTime();
    if (!has_expires_at || expires_at == 0) {
        if (ClockIsReasonable()) {
            ESP_LOGW(kTag, "stored token has no expiry metadata; adding local max-age metadata");
            return SaveAccessTokenMetadata(now);
        }
        ESP_LOGW(kTag, "stored token has no expiry metadata; accepting temporarily until clock is synced");
        return ESP_OK;
    }

    if (!ClockIsReasonable()) {
        ESP_LOGW(kTag, "clock is not synced; token expiry check deferred");
        return ESP_OK;
    }

    if (expires_at <= now) {
        ESP_LOGW(kTag, "stored token expired; clearing token");
        token->clear();
        return ClearAccessTokenKeys();
    }
    return ESP_OK;
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
#if CONFIG_NVS_ENCRYPTION
    ESP_LOGI(kTag, "NVS encryption is enabled by sdkconfig");
#else
    ESP_LOGW(kTag, "NVS encryption is disabled; access token is protected by local expiry metadata only");
#endif
    ESP_LOGI(kTag, "NVS ready");
    return ESP_OK;
}

esp_err_t LoadAccessToken(std::string* token)
{
    ESP_RETURN_ON_ERROR(LoadStringFromNvs(WQN_NVS_ACCESS_TOKEN_KEY, token), kTag, "load access token");
    return EnsureAccessTokenFresh(token);
}

esp_err_t SaveAccessToken(const std::string& token)
{
    if (!IsValidAccessToken(token)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = SaveStringToNvs(WQN_NVS_ACCESS_TOKEN_KEY, token);
    if (result == ESP_OK) {
        result = SaveAccessTokenMetadata(CurrentUnixTime());
    }
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "access token metadata save failed, clearing token: %s", esp_err_to_name(result));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ClearAccessTokenKeys());
    }
    return result;
}

esp_err_t ClearAccessToken()
{
    return ClearAccessTokenKeys();
}

bool IsAccessTokenExpired()
{
    uint64_t expires_at = 0;
    bool found = false;
    if (LoadU64FromNvs(kAccessTokenExpiresAtKey, &expires_at, &found) != ESP_OK || !found || expires_at == 0) {
        return false;
    }
    return ClockIsReasonable() && expires_at <= CurrentUnixTime();
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
            !cJSON_AddStringToObject(item, "status", problem.status.c_str()) ||
            !cJSON_AddStringToObject(item, "content_text", problem.content_text.c_str()) ||
            !cJSON_AddStringToObject(item, "solution_text", problem.solution_text.c_str()) ||
            cJSON_AddNumberToObject(item, "asset_count", problem.asset_count) == nullptr ||
            cJSON_AddNumberToObject(item, "solution_asset_count", problem.solution_asset_count) == nullptr ||
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
        problem.status = GetOptionalString(item, "status");
        problem.content_text = GetOptionalString(item, "content_text");
        problem.solution_text = GetOptionalString(item, "solution_text");
        problem.asset_count = GetOptionalInt(item, "asset_count");
        problem.solution_asset_count = GetOptionalInt(item, "solution_asset_count");
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
    bool replaced = false;
    for (PendingReviewResult& pending : results) {
        if (pending.problem_id == result.problem_id) {
            pending = result;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        results.push_back(result);
    }

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

esp_err_t SaveAiSessionForDay(const CachedAiSession& session)
{
    if (session.day.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr ||
        !cJSON_AddStringToObject(document.root(), "day", session.day.c_str()) ||
        !cJSON_AddStringToObject(document.root(), "conversation_id", session.conversation_id.c_str()) ||
        !cJSON_AddStringToObject(document.root(), "transcript", session.transcript.c_str()) ||
        !cJSON_AddStringToObject(document.root(), "reply_text", session.reply_text.c_str()) ||
        !cJSON_AddStringToObject(document.root(), "status_detail", session.status_detail.c_str()) ||
        cJSON_AddNumberToObject(document.root(), "latency_ms", session.latency_ms) == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    cJSON* calls = cJSON_AddArrayToObject(document.root(), "function_call_summaries");
    if (calls == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    for (const std::string& summary : session.function_call_summaries) {
        cJSON* item = cJSON_CreateString(summary.c_str());
        if (item == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(calls, item);
    }

    std::string payload;
    ESP_RETURN_ON_ERROR(JsonToString(document.root(), &payload), kTag, "serialize AI session");
    return SaveBlobToNvs(kAiSessionKey, payload);
}

esp_err_t LoadAiSessionForDay(const std::string& day, CachedAiSession* session)
{
    if (session == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *session = CachedAiSession{};
    if (day.empty()) {
        return ESP_OK;
    }

    std::string payload;
    ESP_RETURN_ON_ERROR(LoadBlobFromNvs(kAiSessionKey, &payload), kTag, "load AI session");
    if (payload.empty()) {
        return ESP_OK;
    }

    JsonDocument document(payload);
    if (!cJSON_IsObject(document.root())) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::string stored_day = GetOptionalString(document.root(), "day");
    if (stored_day != day) {
        return ESP_OK;
    }

    session->day = stored_day;
    session->conversation_id = GetOptionalString(document.root(), "conversation_id");
    session->transcript = GetOptionalString(document.root(), "transcript");
    session->reply_text = GetOptionalString(document.root(), "reply_text");
    session->status_detail = GetOptionalString(document.root(), "status_detail");
    session->latency_ms = GetOptionalInt(document.root(), "latency_ms");

    cJSON* calls = cJSON_GetObjectItemCaseSensitive(document.root(), "function_call_summaries");
    if (cJSON_IsArray(calls)) {
        const int count = cJSON_GetArraySize(calls);
        session->function_call_summaries.reserve(count);
        for (int i = 0; i < count; ++i) {
            cJSON* item = cJSON_GetArrayItem(calls, i);
            if (cJSON_IsString(item) && item->valuestring != nullptr) {
                session->function_call_summaries.emplace_back(item->valuestring);
            }
        }
    }
    return ESP_OK;
}

esp_err_t ClearAiSession()
{
    return ClearNvsKey(kAiSessionKey);
}

}  // namespace wqn
