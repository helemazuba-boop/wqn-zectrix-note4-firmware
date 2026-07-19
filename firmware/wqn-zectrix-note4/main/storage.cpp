#include "storage.h"

#include <cstdint>
#include <vector>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "online_sync.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/storage_schema.h"
#include "services/storage_service.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "wqn_storage";
constexpr size_t kAccessTokenLength = 64;
constexpr char kProblemsKey[] = "problems";
constexpr char kPendingReviewsKey[] = "pending_reviews";
constexpr char kAiSessionKey[] = "ai_session_day";
constexpr char kAutoSyncIntervalMinKey[] = "sync_min";
constexpr char kWifiSsidKey[] = "wifi_ssid";
constexpr char kWifiPasswordKey[] = "wifi_pass";
constexpr char kControlConfigRevisionKey[] = "v3_cfg_rev";
constexpr char kControlSyncCursorKey[] = "v3_cursor";
static_assert(sizeof(kAutoSyncIntervalMinKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kWifiSsidKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kWifiPasswordKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kControlConfigRevisionKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kControlSyncCursorKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
constexpr char kStoragePartitionLabel[] = "storage";
constexpr char kStorageBasePath[] = "/storage";

struct NvsHandle {
    ~NvsHandle()
    {
        if (handle != 0) {
            nvs_close(handle);
        }
    }

    nvs_handle_t handle = 0;
};

class StorageWriteGuard {
public:
    StorageWriteGuard(const char* holder, const char* file, int line)
        : lease_(wqn::runtime::SleepLease::TryAcquire(
              wqn::runtime::SleepBlocker::kStorage, holder, file, line))
    {
        if (!lease_) {
            ESP_LOGW(kTag, "storage write rejected during sleep quiesce: holder=%s", holder);
        }
    }

    explicit operator bool() const { return static_cast<bool>(lease_); }

private:
    wqn::runtime::SleepLease lease_;
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

esp_err_t SaveStringToNvsRaw(const char* key, const std::string& value)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_str(nvs.handle, key, value.c_str());
    if (result == ESP_OK) {
        result = nvs_commit(nvs.handle);
    }
    return result;
}

struct StringWriteContext {
    const char* key;
    const std::string* value;
};

esp_err_t SaveStringTransaction(void* opaque)
{
    const auto* context = static_cast<const StringWriteContext*>(opaque);
    return SaveStringToNvsRaw(context->key, *context->value);
}

esp_err_t SaveStringToNvs(const char* key, const std::string& value)
{
    StringWriteContext context = {key, &value};
    return wqn::services::ExecuteStorageTransaction(SaveStringTransaction, &context);
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

esp_err_t SaveU64ToNvsRaw(const char* key, uint64_t value)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_u64(nvs.handle, key, value);
    if (result == ESP_OK) {
        result = nvs_commit(nvs.handle);
    }
    return result;
}

struct U64WriteContext {
    const char* key;
    uint64_t value;
};

esp_err_t SaveU64Transaction(void* opaque)
{
    const auto* context = static_cast<const U64WriteContext*>(opaque);
    return SaveU64ToNvsRaw(context->key, context->value);
}

esp_err_t SaveU64ToNvs(const char* key, uint64_t value)
{
    U64WriteContext context = {key, value};
    return wqn::services::ExecuteStorageTransaction(SaveU64Transaction, &context);
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

esp_err_t SaveBlobToNvsRaw(const char* key, const std::string& value)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_blob(nvs.handle, key, value.data(), value.size());
    if (result == ESP_OK) {
        result = nvs_commit(nvs.handle);
    }
    return result;
}

struct BlobWriteContext {
    const char* key;
    const std::string* value;
};

esp_err_t SaveBlobTransaction(void* opaque)
{
    const auto* context = static_cast<const BlobWriteContext*>(opaque);
    return SaveBlobToNvsRaw(context->key, *context->value);
}

esp_err_t SaveBlobToNvs(const char* key, const std::string& value)
{
    BlobWriteContext context = {key, &value};
    return wqn::services::ExecuteStorageTransaction(SaveBlobTransaction, &context);
}

esp_err_t ClearNvsKeyRaw(const char* key)
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

esp_err_t ClearNvsKeyTransaction(void* opaque)
{
    return ClearNvsKeyRaw(static_cast<const char*>(opaque));
}

esp_err_t ClearNvsKey(const char* key)
{
    return wqn::services::ExecuteStorageTransaction(
        ClearNvsKeyTransaction,
        const_cast<char*>(key));
}

bool IsValidAutoSyncInterval(uint32_t minutes)
{
    return minutes == 0 || minutes == 15 || minutes == 30 || minutes == 60 || minutes == 240;
}

esp_err_t InitStoragePartition()
{
    esp_vfs_spiffs_conf_t config = {};
    config.base_path = kStorageBasePath;
    config.partition_label = kStoragePartitionLabel;
    config.max_files = 8;
    // M7 owns all destructive recovery in the pre-business schema gate. A
    // mount failure here must stop startup instead of silently replacing data
    // and then running with an uncommitted schema generation.
    config.format_if_mount_failed = false;

    esp_err_t result = esp_vfs_spiffs_register(&config);
    if (result == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "mount storage SPIFFS");

    size_t total = 0;
    size_t used = 0;
    result = esp_spiffs_info(kStoragePartitionLabel, &total, &used);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "storage SPIFFS ready: total=%u used=%u", static_cast<unsigned>(total), static_cast<unsigned>(used));
    } else {
        ESP_LOGW(kTag, "storage SPIFFS info failed: %s", esp_err_to_name(result));
    }
    return ESP_OK;
}

esp_err_t ClearIdentityStateRaw()
{
    NvsHandle nvs;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    const char* const keys[] = {
        WQN_NVS_ACCESS_TOKEN_KEY,
        kControlConfigRevisionKey,
        kControlSyncCursorKey,
    };
    for (const char* key : keys) {
        result = nvs_erase_key(nvs.handle, key);
        if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
            return result;
        }
    }
    return nvs_commit(nvs.handle);
}

esp_err_t ClearIdentityStateTransaction(void*)
{
    return ClearIdentityStateRaw();
}

esp_err_t ClearAccessTokenKeys()
{
    return wqn::services::ExecuteStorageTransaction(
        ClearIdentityStateTransaction,
        nullptr);
}

esp_err_t SaveDeviceControlStateRaw(const wqn::DeviceControlState& state)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(
        nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle),
        kTag,
        "open NVS namespace");
    ESP_RETURN_ON_ERROR(
        nvs_set_u64(nvs.handle, kControlConfigRevisionKey, state.config_revision),
        kTag,
        "stage v3 config revision");
    ESP_RETURN_ON_ERROR(
        nvs_set_u64(nvs.handle, kControlSyncCursorKey, state.sync_cursor),
        kTag,
        "stage v3 sync cursor");
    return nvs_commit(nvs.handle);
}

esp_err_t SaveDeviceControlStateTransaction(void* opaque)
{
    return SaveDeviceControlStateRaw(
        *static_cast<const wqn::DeviceControlState*>(opaque));
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
    // EnsureStorageSchema() has already initialized and validated default NVS.
    // Re-initialization is harmless, but no erase is allowed after the marker
    // gate because it would let business services run without generation=3.
    ESP_RETURN_ON_ERROR(nvs_flash_init(), kTag, "init validated NVS");
#if CONFIG_NVS_ENCRYPTION
    ESP_LOGI(kTag, "NVS encryption is enabled by sdkconfig");
#else
    ESP_LOGW(kTag, "NVS encryption is disabled; device credentials are stored as plaintext NVS values");
#endif
    ESP_LOGI(kTag, "NVS ready");
    ESP_RETURN_ON_ERROR(InitStoragePartition(), kTag, "init storage partition");
    return services::StartStorageService();
}

esp_err_t LoadAccessToken(std::string* token)
{
    return LoadStringFromNvs(WQN_NVS_ACCESS_TOKEN_KEY, token);
}

esp_err_t SaveAccessToken(const std::string& token)
{
    StorageWriteGuard write("save-access-token", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!IsValidAccessToken(token)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t result = SaveStringToNvs(WQN_NVS_ACCESS_TOKEN_KEY, token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "access token save failed: %s", esp_err_to_name(result));
        return result;
    }

    // [power-fix] The online sync task is parked on portMAX_DELAY while
    // the device is unpaired. Waking it here lets it run the first real
    // sync round immediately after pairing instead of waiting for the
    // next user action or scheduled interval.
    RequestOnlineSyncNow();
    return result;
}

esp_err_t ClearAccessToken()
{
    StorageWriteGuard write("clear-access-token", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    // [power-fix] If clearing the token was triggered by some external
    // event (e.g. the server returning 401 during sync), we want the
    // online task to re-evaluate its delay immediately rather than stay
    // parked on whatever value it was using.
    const esp_err_t result = ClearAccessTokenKeys();
    wqn::RequestOnlineSyncNow();
    return result;
}

std::string MaskTokenForLog(const std::string& token)
{
    if (token.size() < 12) {
        return "<invalid-token>";
    }
    return token.substr(0, 4) + "..." + token.substr(token.size() - 4);
}

esp_err_t LoadDeviceControlState(DeviceControlState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = {};
    bool config_found = false;
    bool cursor_found = false;
    ESP_RETURN_ON_ERROR(
        LoadU64FromNvs(kControlConfigRevisionKey, &state->config_revision, &config_found),
        kTag,
        "load v3 config revision");
    ESP_RETURN_ON_ERROR(
        LoadU64FromNvs(kControlSyncCursorKey, &state->sync_cursor, &cursor_found),
        kTag,
        "load v3 sync cursor");
    if (config_found != cursor_found) {
        ESP_LOGW(kTag, "incomplete v3 control checkpoint; resetting both values");
        *state = {};
    }
    return ESP_OK;
}

esp_err_t SaveDeviceControlState(const DeviceControlState& state)
{
    StorageWriteGuard write("save-v3-control-state", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    return services::ExecuteStorageTransaction(
        SaveDeviceControlStateTransaction,
        const_cast<DeviceControlState*>(&state));
}

esp_err_t SaveProblems(const std::vector<CachedProblem>& problems)
{
    StorageWriteGuard write("save-problems", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
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
    StorageWriteGuard write("clear-problems", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    return ClearNvsKey(kProblemsKey);
}

esp_err_t EnqueueReviewResult(const PendingReviewResult& result)
{
    StorageWriteGuard write("enqueue-review", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
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
    StorageWriteGuard write("clear-pending-reviews", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    return ClearNvsKey(kPendingReviewsKey);
}

esp_err_t SaveAiSessionForDay(const CachedAiSession& session)
{
    StorageWriteGuard write("save-ai-session", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
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
    StorageWriteGuard write("clear-ai-session", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    return ClearNvsKey(kAiSessionKey);
}

esp_err_t LoadAutoSyncIntervalMinutes(uint32_t* minutes)
{
    if (minutes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t raw = 0;
    bool found = false;
    ESP_RETURN_ON_ERROR(LoadU64FromNvs(kAutoSyncIntervalMinKey, &raw, &found), kTag, "load auto sync interval");
    const uint32_t value = found ? static_cast<uint32_t>(raw) : 0;
    *minutes = IsValidAutoSyncInterval(value) ? value : 0;
    return ESP_OK;
}

esp_err_t SaveAutoSyncIntervalMinutes(uint32_t minutes)
{
    StorageWriteGuard write("save-sync-interval", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!IsValidAutoSyncInterval(minutes)) {
        return ESP_ERR_INVALID_ARG;
    }
    return SaveU64ToNvs(kAutoSyncIntervalMinKey, minutes);
}

std::string AutoSyncIntervalLabel(uint32_t minutes)
{
    switch (minutes) {
        case 0:
            return "从不";
        case 15:
            return "15分";
        case 30:
            return "30分";
        case 60:
            return "1小时";
        case 240:
            return "4小时";
        default:
            return "从不";
    }
}

// [hw-volume] Global playback volume (0-100%). Persisted in the "audio"
// namespace as "output_volume" (i32) to match the original closed-source
// firmware, so the setting survives a firmware swap. Read also accepts u8 in
// case the blob was written that way. A process-wide cache backs
// GetPlaybackVolumePercent() so the audio path never touches flash; the level
// is applied to ES8311 DAC registers (0x32/0x31) during codec init.
constexpr int kVolumeDefaultPercent = 100;
constexpr const char* kAudioNamespace = "audio";
constexpr const char* kVolumeNvsKey = "output_volume";
int g_volume_percent_cache = kVolumeDefaultPercent;

esp_err_t SaveVolumeTransaction(void* opaque)
{
    const int percent = *static_cast<const int*>(opaque);
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kAudioNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_i32(handle, kVolumeNvsKey, static_cast<int32_t>(percent));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t FactoryResetTransaction(void*)
{
    ESP_LOGW(kTag, "factory reset requested: invalidating schema marker and restarting");
    ESP_RETURN_ON_ERROR(
        wqn::runtime::InvalidateStorageSchemaForFactoryReset(),
        kTag,
        "invalidate storage schema marker");
    esp_restart();
    return ESP_OK;
}

esp_err_t LoadVolumePercent(int* percent)
{
    if (percent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    int v = kVolumeDefaultPercent;
    nvs_handle_t h = 0;
    esp_err_t ret = nvs_open(kAudioNamespace, NVS_READONLY, &h);
    if (ret == ESP_OK) {
        int32_t i32v = 0;
        esp_err_t gr = nvs_get_i32(h, kVolumeNvsKey, &i32v);
        if (gr == ESP_ERR_NVS_TYPE_MISMATCH) {
            // Origin firmware may have stored it as u8.
            uint8_t u8v = 0;
            gr = nvs_get_u8(h, kVolumeNvsKey, &u8v);
            if (gr == ESP_OK) v = static_cast<int>(u8v);
        } else if (gr == ESP_OK) {
            v = static_cast<int>(i32v);
        }
        nvs_close(h);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet (first boot) - use default.
        ret = ESP_OK;
    }
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    *percent = v;
    g_volume_percent_cache = v;
    return ret;
}

esp_err_t SaveVolumePercent(int percent)
{
    StorageWriteGuard write("save-volume", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    g_volume_percent_cache = percent;
    return services::ExecuteStorageTransaction(SaveVolumeTransaction, &percent);
}

std::string VolumeLabel(int percent)
{
    if (percent <= 0) {
        return "静音";
    }
    return std::to_string(percent) + "%";
}

int GetPlaybackVolumePercent()
{
    return g_volume_percent_cache;
}

esp_err_t FactoryResetNvsAndRestart()
{
    StorageWriteGuard write("factory-reset", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    return services::ExecuteStorageTransaction(FactoryResetTransaction, nullptr);
}

esp_err_t LoadWifiCredentials(std::string* ssid, std::string* password)
{
    if (ssid == nullptr || password == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(LoadStringFromNvs(kWifiSsidKey, ssid), kTag, "load WiFi SSID");
    return LoadStringFromNvs(kWifiPasswordKey, password);
}

esp_err_t SaveWifiCredentials(const std::string& ssid, const std::string& password)
{
    StorageWriteGuard write("save-wifi-credentials", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(SaveStringToNvs(kWifiSsidKey, ssid), kTag, "save WiFi SSID");
    return SaveStringToNvs(kWifiPasswordKey, password);
}

esp_err_t ClearWifiCredentials()
{
    StorageWriteGuard write("clear-wifi-credentials", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ClearNvsKey(kWifiSsidKey);
    const esp_err_t clear_pass = ClearNvsKey(kWifiPasswordKey);
    if (result == ESP_OK) {
        result = clear_pass;
    }
    return result;
}

bool HasWifiCredentials()
{
    std::string ssid;
    return LoadStringFromNvs(kWifiSsidKey, &ssid) == ESP_OK && !ssid.empty();
}

esp_err_t PrepareStorageForSleep(int64_t deadline_us)
{
    if (deadline_us > 0 && esp_timer_get_time() >= deadline_us) {
        return ESP_ERR_TIMEOUT;
    }
    return runtime::ActiveSleepBlockerCount(runtime::SleepBlocker::kStorage) == 0
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}

void RollbackStorageAfterSleepAbort()
{
    // Accepted writes are synchronous NVS commits or fclose/rename SPIFFS
    // transactions. The global quiesce gate reopening is the only rollback.
}

}  // namespace wqn
