#include "storage.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
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
#include "services/sync_service.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/storage_schema.h"
#include "services/storage_service.h"
#include "sdkconfig.h"
#include "word_study_store.h"

namespace {

constexpr char kTag[] = "wqn_storage";
constexpr size_t kAccessTokenLength = 64;
constexpr char kProblemsKey[] = "problems";
constexpr char kPendingReviewsKey[] = "pending_reviews";
constexpr char kAiSessionKey[] = "ai_session_day";
constexpr char kAutoSyncIntervalMinKey[] = "sync_min";
constexpr char kImageRenderModeKey[] = "img_render";
constexpr char kDefaultWordDeckKey[] = "word_deck";
constexpr char kWifiSsidKey[] = "wifi_ssid";
constexpr char kWifiPasswordKey[] = "wifi_pass";
// [wifi-redundancy] Versioned dual-slot credential blob (replaces the per-key
// wifi_ssid/wifi_pass pair with a single atomic commit).
constexpr char kWifiCredsBlobKey[] = "wifi_creds";
constexpr uint8_t kWifiCredentialStoreVersion = 1;
constexpr char kControlConfigRevisionKey[] = "v3_cfg_rev";
constexpr char kControlSyncCursorKey[] = "v3_cursor";
static_assert(sizeof(kAutoSyncIntervalMinKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kImageRenderModeKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kWifiSsidKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kWifiPasswordKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kWifiCredsBlobKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kControlConfigRevisionKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kControlSyncCursorKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
constexpr char kStoragePartitionLabel[] = "storage";
constexpr char kStorageBasePath[] = "/storage";
constexpr char kSyncJournalPath[] = "/storage/sync_journal.json";
constexpr char kSyncJournalTempPath[] = "/storage/sync_journal.tmp";
constexpr char kSyncJournalBackupPath[] = "/storage/sync_journal.bak";

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

esp_err_t ReadStorageTextFile(const char* path, std::string* output)
{
    if (path == nullptr || output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    output->clear();
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    output->resize(static_cast<size_t>(length));
    if (!output->empty() &&
        std::fread(output->data(), 1, output->size(), file) != output->size()) {
        std::fclose(file);
        output->clear();
        return ESP_FAIL;
    }
    return std::fclose(file) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t WriteSyncJournalFileAtomic(const std::string& payload)
{
    FILE* file = std::fopen(kSyncJournalTempPath, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    const bool complete = payload.empty() ||
        std::fwrite(payload.data(), 1, payload.size(), file) == payload.size();
    const bool flushed = complete && std::fflush(file) == 0 &&
        ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!complete || !flushed || !closed) {
        std::remove(kSyncJournalTempPath);
        return ESP_FAIL;
    }
    std::remove(kSyncJournalBackupPath);
    if (std::rename(kSyncJournalPath, kSyncJournalBackupPath) != 0 && errno != ENOENT) {
        std::remove(kSyncJournalTempPath);
        return ESP_FAIL;
    }
    if (std::rename(kSyncJournalTempPath, kSyncJournalPath) != 0) {
        std::rename(kSyncJournalBackupPath, kSyncJournalPath);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void AddJournalContentState(cJSON* parent, const char* key,
                            const wqn::SyncJournalContentState& state)
{
    cJSON* object = cJSON_CreateObject();
    if (object == nullptr) {
        return;
    }
    cJSON_AddNumberToObject(object, "desired_revision",
                            static_cast<double>(state.desired_revision));
    cJSON_AddNumberToObject(object, "applied_revision",
                            static_cast<double>(state.applied_revision));
    cJSON_AddNumberToObject(object, "phase",
                            static_cast<double>(static_cast<uint8_t>(state.phase)));
    cJSON_AddNumberToObject(object, "retry_attempt",
                            static_cast<double>(state.retry_attempt));
    cJSON_AddStringToObject(object, "desired_snapshot_id", state.desired_snapshot_id);
    cJSON_AddStringToObject(object, "active_snapshot_id", state.active_snapshot_id);
    cJSON_AddItemToObject(parent, key, object);
}

bool ReadJournalU64(cJSON* object, const char* key, uint64_t* value)
{
    if (object == nullptr || value == nullptr) {
        return false;
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0 ||
        item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *value = static_cast<uint64_t>(item->valuedouble);
    return true;
}

std::string ReadJournalString(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr
        ? item->valuestring
        : std::string();
}

bool ReadJournalContentState(cJSON* parent, const char* key,
                             wqn::SyncJournalContentState* state)
{
    if (parent == nullptr || state == nullptr) {
        return false;
    }
    cJSON* object = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(object)) {
        return false;
    }
    uint64_t desired = 0;
    uint64_t applied = 0;
    uint64_t phase = 0;
    uint64_t retry = 0;
    const auto valid_snapshot_id = [](const std::string& value) {
        return value.empty() ||
            (value.size() == 64 && std::all_of(
                value.begin(), value.end(), [](char ch) {
                    return (ch >= '0' && ch <= '9') ||
                        (ch >= 'a' && ch <= 'f');
                }));
    };
    if (!ReadJournalU64(object, "desired_revision", &desired) ||
        !ReadJournalU64(object, "applied_revision", &applied) ||
        !ReadJournalU64(object, "phase", &phase) ||
        !ReadJournalU64(object, "retry_attempt", &retry) ||
        phase > static_cast<uint64_t>(wqn::SyncJournalPhase::kBlocked) ||
        retry > UINT8_MAX || applied > desired) {
        return false;
    }
    const std::string desired_snapshot = ReadJournalString(object, "desired_snapshot_id");
    const std::string active_snapshot = ReadJournalString(object, "active_snapshot_id");
    if (!valid_snapshot_id(desired_snapshot) ||
        !valid_snapshot_id(active_snapshot)) {
        return false;
    }
    state->desired_revision = desired;
    state->applied_revision = applied;
    state->phase = static_cast<wqn::SyncJournalPhase>(phase);
    state->retry_attempt = static_cast<uint8_t>(retry);
    std::snprintf(state->desired_snapshot_id,
                  sizeof(state->desired_snapshot_id), "%s", desired_snapshot.c_str());
    std::snprintf(state->active_snapshot_id,
                  sizeof(state->active_snapshot_id), "%s", active_snapshot.c_str());
    return true;
}

esp_err_t ParseSyncJournalPayload(
    const std::string& payload,
    wqn::SyncJournal* journal)
{
    if (journal == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *journal = {};
    if (payload.empty() || payload.find('\0') != std::string::npos) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON* root = cJSON_ParseWithOpts(payload.c_str(), nullptr, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t schema = 0;
    uint64_t config_revision = 0;
    uint64_t sync_cursor = 0;
    const bool valid =
        ReadJournalU64(root, "schema_version", &schema) &&
        ReadJournalU64(root, "config_revision", &config_revision) &&
        ReadJournalU64(root, "sync_cursor", &sync_cursor) &&
        schema == 1 &&
        ReadJournalContentState(root, "word_packs", &journal->word_packs) &&
        ReadJournalContentState(root, "note_packs", &journal->note_packs) &&
        ReadJournalContentState(root, "problem_packs", &journal->problem_packs);
    cJSON_Delete(root);
    if (!valid) {
        *journal = {};
        return ESP_ERR_INVALID_STATE;
    }
    journal->schema_version = static_cast<uint32_t>(schema);
    journal->config_revision = config_revision;
    journal->sync_cursor = sync_cursor;
    return ESP_OK;
}

esp_err_t RemovePrototypeProblemFile(const char* path)
{
    if (std::remove(path) == 0 || errno == ENOENT) {
        return ESP_OK;
    }
    ESP_LOGW(kTag, "remove obsolete problem file failed: path=%s errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t CleanupPrototypeProblemStorageTransaction(void*)
{
    constexpr const char* kObsoleteFiles[] = {
        "/storage/problems.v1",
        "/storage/problems.tmp",
        "/storage/problems.bak",
    };
    constexpr const char* kObsoleteKeys[] = {
        kProblemsKey,
        kPendingReviewsKey,
    };

    esp_err_t first_error = ESP_OK;
    for (const char* path : kObsoleteFiles) {
        const esp_err_t result = RemovePrototypeProblemFile(path);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
    }
    NvsHandle nvs;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &nvs.handle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
        if (first_error == ESP_OK) {
            first_error = result;
        }
        ESP_LOGW(kTag, "open obsolete problem NVS cleanup failed: %s",
                 esp_err_to_name(result));
        return first_error;
    }
    if (result == ESP_OK) {
        for (const char* key : kObsoleteKeys) {
            result = nvs_erase_key(nvs.handle, key);
            if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
                if (first_error == ESP_OK) {
                    first_error = result;
                }
                ESP_LOGW(kTag, "erase obsolete problem key failed: key=%s error=%s",
                         key, esp_err_to_name(result));
            }
        }
        result = nvs_commit(nvs.handle);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
    }
    return first_error;
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
    ESP_RETURN_ON_ERROR(services::StartStorageService(), kTag, "start storage service");
    // [deck-scope] Replay an interrupted default-deck change and seed the
    // scope-generation cache BEFORE any word session can be loaded. A failure
    // here MUST abort startup into the existing storage recovery mode: the
    // marker may still be set with the deck/sessions/generation half-applied,
    // and business services must not run against that state.
    ESP_RETURN_ON_ERROR(RecoverDefaultDeckScopeChange(), kTag,
                        "recover deck scope change");
    // The removed prototype used three compressed files and two NVS blobs. Reclaim
    // those exact artifacts on StorageService so cleanup is serialized with
    // all current pack/outbox writes. It is intentionally idempotent and does
    // not touch problem-study-v1 pp_* / po_* files.
    const esp_err_t cleanup_result = services::ExecuteStorageTransaction(
        CleanupPrototypeProblemStorageTransaction,
        nullptr);
    if (cleanup_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "obsolete problem storage cleanup deferred: %s",
            esp_err_to_name(cleanup_result));
    }
    return ESP_OK;
}

bool ReadStorageCapacitySnapshot(StorageCapacitySnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return false;
    }
    *snapshot = {};

    snapshot->spiffs_valid =
        esp_spiffs_info(
            kStoragePartitionLabel,
            &snapshot->spiffs_total_bytes,
            &snapshot->spiffs_used_bytes) == ESP_OK;

    nvs_stats_t stats = {};
    snapshot->nvs_valid = nvs_get_stats(nullptr, &stats) == ESP_OK;
    if (snapshot->nvs_valid) {
        snapshot->nvs_used_entries = stats.used_entries;
        snapshot->nvs_free_entries = stats.free_entries;
        snapshot->nvs_total_entries = stats.total_entries;
    }
    return snapshot->spiffs_valid || snapshot->nvs_valid;
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

    // Credentials are an explicit full-sync reason. Publishing it here lets
    // the scheduler run the first authenticated round immediately after
    // pairing instead of waiting for the next periodic deadline.
    services::NotifySyncCredentialsChanged();
    return result;
}

esp_err_t ClearAccessToken()
{
    StorageWriteGuard write("clear-access-token", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    // If clearing the token was triggered by 401, make the scheduler enter
    // the claim path immediately rather than wait on its prior deadline.
    const esp_err_t result = ClearAccessTokenKeys();
    wqn::services::NotifySyncCredentialsChanged();
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

esp_err_t LoadSyncJournal(SyncJournal* journal)
{
    if (journal == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    std::string payload;
    esp_err_t primary_result = ReadStorageTextFile(kSyncJournalPath, &payload);
    if (primary_result == ESP_OK) {
        primary_result = ParseSyncJournalPayload(payload, journal);
        if (primary_result == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(kTag, "primary sync journal invalid; trying backup");
    } else if (primary_result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "primary sync journal unreadable; trying backup");
    }

    payload.clear();
    esp_err_t backup_result = ReadStorageTextFile(kSyncJournalBackupPath, &payload);
    if (backup_result == ESP_OK) {
        backup_result = ParseSyncJournalPayload(payload, journal);
        if (backup_result == ESP_OK) {
            ESP_LOGW(kTag, "recovered sync journal from backup");
            return ESP_OK;
        }
    }
    *journal = {};
    if (primary_result == ESP_ERR_NOT_FOUND &&
        backup_result == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    return backup_result != ESP_ERR_NOT_FOUND
        ? backup_result
        : primary_result;
}

esp_err_t SaveSyncJournal(const SyncJournal& journal)
{
    if (journal.schema_version != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    StorageWriteGuard write("save-sync-journal", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", journal.schema_version);
    cJSON_AddNumberToObject(root, "config_revision",
                            static_cast<double>(journal.config_revision));
    cJSON_AddNumberToObject(root, "sync_cursor",
                            static_cast<double>(journal.sync_cursor));
    AddJournalContentState(root, "word_packs", journal.word_packs);
    AddJournalContentState(root, "note_packs", journal.note_packs);
    AddJournalContentState(root, "problem_packs", journal.problem_packs);
    std::string payload;
    const esp_err_t render_result = JsonToString(root, &payload);
    cJSON_Delete(root);
    if (render_result != ESP_OK) {
        return render_result;
    }
    return WriteSyncJournalFileAtomic(payload);
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

esp_err_t SaveAutoSyncIntervalMinutesForeground(uint32_t minutes)
{
    StorageWriteGuard write("save-sync-interval", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!IsValidAutoSyncInterval(minutes)) {
        return ESP_ERR_INVALID_ARG;
    }
    U64WriteContext context = {kAutoSyncIntervalMinKey, minutes};
    return services::ExecuteForegroundStorageTransaction(
        SaveU64Transaction, &context, "save-sync-interval");
}

esp_err_t LoadImageRenderMode(ImageRenderMode* mode)
{
    if (mode == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t raw = 0;
    bool found = false;
    ESP_RETURN_ON_ERROR(
        LoadU64FromNvs(kImageRenderModeKey, &raw, &found), kTag,
        "load image render mode");
    // GRAY16 is the product default. Unknown future values degrade to the
    // same safe full-refresh path rather than silently selecting BW1.
    *mode = found && raw == static_cast<uint64_t>(ImageRenderMode::kBlackWhite)
        ? ImageRenderMode::kBlackWhite
        : ImageRenderMode::kGray16;
    return ESP_OK;
}

esp_err_t SaveImageRenderModeForeground(ImageRenderMode mode)
{
    if (mode != ImageRenderMode::kBlackWhite && mode != ImageRenderMode::kGray16) {
        return ESP_ERR_INVALID_ARG;
    }
    StorageWriteGuard write("save-image-render", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    U64WriteContext context = {
        kImageRenderModeKey, static_cast<uint64_t>(mode)};
    return services::ExecuteForegroundStorageTransaction(
        SaveU64Transaction, &context, "save-image-render");
}

std::string ImageRenderModeLabel(ImageRenderMode mode)
{
    return mode == ImageRenderMode::kBlackWhite ? "黑白" : "16阶灰度";
}

esp_err_t LoadDefaultWordDeckId(std::string* deck_id)
{
    if (deck_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    deck_id->clear();
    // LoadStringFromNvs maps a missing key to ESP_OK + empty (unset = all
    // decks).
    const esp_err_t result = LoadStringFromNvs(kDefaultWordDeckKey, deck_id);
    if (result == ESP_OK && !deck_id->empty() && deck_id->size() != 36) {
        // Anything but a UUID is treated as unset rather than poisoning the
        // word scope forever.
        deck_id->clear();
    }
    return result;
}

esp_err_t SaveDefaultWordDeckId(const std::string& deck_id)
{
    StorageWriteGuard write("save-word-deck", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (deck_id.empty()) {
        // Empty selection = all decks; drop the key entirely.
        return ClearNvsKey(kDefaultWordDeckKey);
    }
    if (deck_id.size() != 36) {
        return ESP_ERR_INVALID_ARG;
    }
    return SaveStringToNvs(kDefaultWordDeckKey, deck_id);
}

namespace {

// [deck-scope] Recoverable default-deck change (c5). NVS keys:
//  - kDeckScopeMarkerKey: "<target_gen>|<deck_id>" while a change is in
//    flight (deck_id empty = all decks). Present marker = the session clears
//    and/or the deck+generation save may not have completed.
//  - kDeckScopeGenKey: committed scope generation (u64 slot, u32 values).
// The RAM cache lets session save/load stamp & validate without a storage
// transaction (word_study_store reads it via GetDeckScopeGeneration).
constexpr const char* kDeckScopeMarkerKey = "word_deck_chg";
constexpr const char* kDeckScopeGenKey = "word_deck_gen";
std::atomic<uint32_t> g_deck_scope_generation{0};

// Replays the tail of the change protocol. Idempotent: session-file removal
// tolerates ENOENT, NVS writes overwrite. STORAGE TASK ONLY (the direct
// ClearPersistedWordSession calls rely on the service-task passthrough).
esp_err_t ApplyDeckScopeChangeLocked(uint32_t target_generation, const std::string& deck_id)
{
    ESP_RETURN_ON_ERROR(
        ClearPersistedWordSession(protocol::word_study_v1::Mode::kSequential),
        kTag, "deck change: clear sequential session");
    ESP_RETURN_ON_ERROR(
        ClearPersistedWordSession(protocol::word_study_v1::Mode::kRandom),
        kTag, "deck change: clear random session");
    if (deck_id.empty()) {
        ESP_RETURN_ON_ERROR(ClearNvsKeyRaw(kDefaultWordDeckKey), kTag,
                            "deck change: clear deck key");
    } else {
        ESP_RETURN_ON_ERROR(SaveStringToNvsRaw(kDefaultWordDeckKey, deck_id),
                            kTag, "deck change: save deck key");
    }
    ESP_RETURN_ON_ERROR(SaveU64ToNvsRaw(kDeckScopeGenKey, target_generation),
                        kTag, "deck change: save scope generation");
    ESP_RETURN_ON_ERROR(ClearNvsKeyRaw(kDeckScopeMarkerKey), kTag,
                        "deck change: clear marker");
    g_deck_scope_generation.store(target_generation, std::memory_order_release);
    return ESP_OK;
}

struct DeckScopeChangeContext {
    const std::string* deck_id;
};

esp_err_t DeckScopeChangeTransaction(void* opaque)
{
    auto* context = static_cast<DeckScopeChangeContext*>(opaque);
    if (context == nullptr || context->deck_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t committed_generation =
        g_deck_scope_generation.load(std::memory_order_acquire);
    uint32_t target_generation = committed_generation + 1;
    if (target_generation == 0) {
        // uint32 wrap: 0 is the "never initialized" sentinel, skip it.
        target_generation = 1;
    }
    // Step 1: marker first. If power dies anywhere past this commit, boot
    // recovery replays the tail from the marker.
    ESP_RETURN_ON_ERROR(
        SaveStringToNvsRaw(
            kDeckScopeMarkerKey,
            std::to_string(target_generation) + "|" + *context->deck_id),
        kTag, "deck change: write marker");
    return ApplyDeckScopeChangeLocked(target_generation, *context->deck_id);
}

esp_err_t RecoverDeckScopeTransaction(void*)
{
    // Seed the generation cache from the committed value first.
    uint64_t committed = 0;
    bool found = false;
    ESP_RETURN_ON_ERROR(LoadU64FromNvs(kDeckScopeGenKey, &committed, &found),
                        kTag, "deck recover: load generation");
    g_deck_scope_generation.store(
        found ? static_cast<uint32_t>(committed) : 0, std::memory_order_release);

    std::string marker;
    ESP_RETURN_ON_ERROR(LoadStringFromNvs(kDeckScopeMarkerKey, &marker),
                        kTag, "deck recover: load marker");
    if (marker.empty()) {
        return ESP_OK;  // no interrupted change
    }
    const size_t split = marker.find('|');
    uint32_t target_generation = 0;
    bool parsed_ok = false;
    if (split != std::string::npos && split > 0) {
        const std::string generation_text = marker.substr(0, split);
        errno = 0;
        char* end = nullptr;
        const unsigned long parsed =
            std::strtoul(generation_text.c_str(), &end, 10);
        // Full consumption and no overflow (ERANGE surfaces via errno; on this
        // 32-bit target unsigned long IS uint32). 0 stays the sentinel.
        if (errno == 0 && end != nullptr && *end == '\0' &&
            end != generation_text.c_str() && parsed != 0) {
            target_generation = static_cast<uint32_t>(parsed);
            parsed_ok = true;
        }
    }
    const std::string deck_id =
        split == std::string::npos ? std::string() : marker.substr(split + 1);
    // The deck must be empty (all decks) or a UUID, and the target generation
    // must be the committed one (crash after the generation save) or its
    // successor (crash before it, accounting for the wrap-skip over 0).
    uint32_t successor =
        g_deck_scope_generation.load(std::memory_order_acquire) + 1;
    if (successor == 0) {
        successor = 1;
    }
    const bool deck_ok = deck_id.empty() || deck_id.size() == 36;
    const bool generation_ok = parsed_ok &&
        (target_generation == g_deck_scope_generation.load(std::memory_order_acquire) ||
         target_generation == successor);
    if (!deck_ok || !generation_ok) {
        // Corrupt marker: drop it rather than replaying garbage as a switch.
        // The committed deck/generation stay as-is; sessions may already be
        // cleared, which is safe (they are only browse cursors).
        ESP_LOGW(kTag, "deck recover: malformed marker '%s' dropped", marker.c_str());
        return ClearNvsKeyRaw(kDeckScopeMarkerKey);
    }
    ESP_LOGW(kTag,
             "deck recover: replaying interrupted change: gen=%u deck=%s",
             static_cast<unsigned>(target_generation),
             deck_id.empty() ? "all" : deck_id.c_str());
    return ApplyDeckScopeChangeLocked(target_generation, deck_id);
}

}  // namespace

esp_err_t ChangeDefaultWordDeckForeground(const std::string& deck_id)
{
    StorageWriteGuard write("word-deck-change", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!deck_id.empty() && deck_id.size() != 36) {
        return ESP_ERR_INVALID_ARG;
    }
    DeckScopeChangeContext context{&deck_id};
    return services::ExecuteForegroundStorageTransaction(
        DeckScopeChangeTransaction, &context, "word-deck-change");
}

esp_err_t RecoverDefaultDeckScopeChange()
{
    return services::ExecuteStorageTransactionNamed(
        RecoverDeckScopeTransaction, nullptr, "word-deck-recover");
}

uint32_t GetDeckScopeGeneration()
{
    return g_deck_scope_generation.load(std::memory_order_acquire);
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
// [persist-worker] Read by the audio task, written by the UI task (the sync
// SaveVolumePercent and SubmitVolumeSave's post-reserve update via
// SetPlaybackVolumeCache) plus the boot restore in main.cpp. Atomic + relaxed
// removes the cross-task data race; the value is a self-contained scalar so no
// ordering with other state is needed.
std::atomic<int> g_volume_percent_cache{kVolumeDefaultPercent};

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
    // Pure persistent read: deliberately does NOT touch the runtime playback
    // cache. Callers such as the settings diagnostics and the periodic status
    // reload re-read NVS while an async volume save may still be pending or
    // failed-awaiting-retry; writing the old durable value back here silently
    // rolled the audible volume back under the user. Boot explicitly seeds the
    // cache via SetPlaybackVolumeCache (main.cpp).
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
    g_volume_percent_cache.store(percent, std::memory_order_relaxed);
    return services::ExecuteStorageTransaction(SaveVolumeTransaction, &percent);
}

esp_err_t SaveVolumePercentForeground(int percent)
{
    StorageWriteGuard write("save-volume", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    // The runtime cache is NOT touched here: the UI thread updates it right
    // after a successful reserve/enqueue (SetPlaybackVolumeCache), so playback
    // uses the new level immediately regardless of how long the durable NVS
    // write waits behind other worker commands.
    return services::ExecuteForegroundStorageTransaction(
        SaveVolumeTransaction, &percent, "save-volume");
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
    return g_volume_percent_cache.load(std::memory_order_relaxed);
}

void SetPlaybackVolumeCache(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume_percent_cache.store(percent, std::memory_order_relaxed);
}

esp_err_t FactoryResetNvsAndRestart()
{
    StorageWriteGuard write("factory-reset", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    return services::ExecuteStorageTransaction(FactoryResetTransaction, nullptr);
}

namespace {

constexpr size_t kWifiSsidMaxLen = 32;
constexpr size_t kWifiPasswordMaxLen = 64;

// [wifi-redundancy] The blob is a raw memcpy of WifiCredentialStore; lock the
// layout so an accidental field change is a build error, not silent corruption.
static_assert(sizeof(wqn::WifiCredentialSlot) == 33 + 65,
              "WifiCredentialSlot layout changed; bump kWifiCredentialStoreVersion");
static_assert(sizeof(wqn::WifiCredentialStore) == 3 + 2 * (33 + 65),
              "WifiCredentialStore layout changed; bump kWifiCredentialStoreVersion");

void CopyWifiCredentialField(char* destination, size_t destination_size, const char* value)
{
    if (destination == nullptr || destination_size == 0) {
        return;
    }
    std::snprintf(destination, destination_size, "%s", value == nullptr ? "" : value);
}

// [wifi-redundancy] Validates a store deserialized from the NVS blob. Returns
// false when the blob is unusable (bad version / impossible count/preferred) so
// the caller falls back to legacy migration. Otherwise normalizes in place:
// forces NUL termination, drops empty-SSID slots, and merges duplicate SSIDs
// keeping the preferred slot's password.
bool ValidateWifiCredentialStore(wqn::WifiCredentialStore* store)
{
    if (store == nullptr || store->version != kWifiCredentialStoreVersion) {
        return false;
    }
    if (store->count > 2 || store->preferred >= 2) {
        return false;
    }
    for (auto& slot : store->slots) {
        slot.ssid[sizeof(slot.ssid) - 1] = '\0';
        slot.password[sizeof(slot.password) - 1] = '\0';
    }

    // Compact to occupied (non-empty-SSID) slots, tracking where the preferred
    // entry lands. Identical SSIDs collapse to one slot; the preferred copy's
    // password wins.
    wqn::WifiCredentialStore compacted{};
    compacted.version = kWifiCredentialStoreVersion;
    const bool preferred_valid =
        store->preferred < store->count && store->slots[store->preferred].ssid[0] != '\0';
    for (uint8_t i = 0; i < store->count; ++i) {
        const auto& slot = store->slots[i];
        if (slot.ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (uint8_t j = 0; j < compacted.count; ++j) {
            if (std::strcmp(compacted.slots[j].ssid, slot.ssid) == 0) {
                duplicate = true;
                if (preferred_valid && i == store->preferred) {
                    CopyWifiCredentialField(
                        compacted.slots[j].password, sizeof(compacted.slots[j].password), slot.password);
                    compacted.preferred = j;
                }
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        const uint8_t destination = compacted.count;
        compacted.slots[destination] = slot;
        if (preferred_valid && i == store->preferred) {
            compacted.preferred = destination;
        }
        ++compacted.count;
    }
    if (!preferred_valid) {
        compacted.preferred = 0;
    }
    *store = compacted;
    return true;
}

}  // namespace

esp_err_t LoadWifiCredentialStore(WifiCredentialStore* store)
{
    if (store == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *store = WifiCredentialStore{};

    std::string blob;
    ESP_RETURN_ON_ERROR(LoadBlobFromNvs(kWifiCredsBlobKey, &blob), kTag, "load wifi credential blob");
    if (!blob.empty()) {
        if (blob.size() != sizeof(WifiCredentialStore)) {
            ESP_LOGW(
                kTag,
                "wifi credential blob size mismatch: %u != %u; ignoring",
                static_cast<unsigned>(blob.size()),
                static_cast<unsigned>(sizeof(WifiCredentialStore)));
        } else {
            WifiCredentialStore candidate;
            std::memcpy(&candidate, blob.data(), sizeof(candidate));
            if (ValidateWifiCredentialStore(&candidate)) {
                *store = candidate;
                return ESP_OK;
            }
            ESP_LOGW(kTag, "wifi credential blob failed validation; trying legacy migration");
        }
    }

    // Legacy migration: synthesize slot 0 from the per-key wifi_ssid/wifi_pass,
    // then persist as a blob and clear the legacy keys so the blob becomes the
    // single source of truth.
    std::string ssid;
    std::string password;
    ESP_RETURN_ON_ERROR(LoadStringFromNvs(kWifiSsidKey, &ssid), kTag, "load legacy wifi ssid");
    if (ssid.empty()) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(LoadStringFromNvs(kWifiPasswordKey, &password), kTag, "load legacy wifi password");
    store->version = kWifiCredentialStoreVersion;
    store->preferred = 0;
    store->count = 1;
    CopyWifiCredentialField(store->slots[0].ssid, sizeof(store->slots[0].ssid), ssid.c_str());
    CopyWifiCredentialField(store->slots[0].password, sizeof(store->slots[0].password), password.c_str());
    ESP_LOGI(kTag, "migrated legacy wifi credentials into slot 0 (SSID=%s)", ssid.c_str());
    if (SaveWifiCredentialStore(*store) == ESP_OK) {
        ClearNvsKey(kWifiSsidKey);
        ClearNvsKey(kWifiPasswordKey);
    }
    return ESP_OK;
}

esp_err_t SaveWifiCredentialStore(const WifiCredentialStore& store)
{
    if (store.count > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    StorageWriteGuard write("save-wifi-credential-store", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::string blob(reinterpret_cast<const char*>(&store), sizeof(store));
    return SaveBlobToNvs(kWifiCredsBlobKey, blob);
}

esp_err_t UpsertWifiCredential(const std::string& ssid, const std::string& password)
{
    if (ssid.empty() || ssid.size() > kWifiSsidMaxLen || password.size() > kWifiPasswordMaxLen) {
        return ESP_ERR_INVALID_ARG;
    }
    WifiCredentialStore store;
    ESP_RETURN_ON_ERROR(LoadWifiCredentialStore(&store), kTag, "load store for upsert");

    bool hit = false;
    bool changed = false;
    for (uint8_t i = 0; i < store.count; ++i) {
        if (std::strcmp(store.slots[i].ssid, ssid.c_str()) != 0) {
            continue;
        }
        // Refresh every slot carrying this SSID so a stale password cannot
        // survive on the other slot.
        hit = true;
        if (std::strcmp(store.slots[i].password, password.c_str()) != 0) {
            CopyWifiCredentialField(store.slots[i].password, sizeof(store.slots[i].password), password.c_str());
            changed = true;
        }
        if (store.preferred != i) {
            store.preferred = i;
            changed = true;
        }
    }
    if (!hit) {
        const uint8_t target = (store.count < 2) ? store.count++ : (store.preferred == 0 ? 1 : 0);
        store.slots[target] = WifiCredentialSlot{};
        CopyWifiCredentialField(store.slots[target].ssid, sizeof(store.slots[target].ssid), ssid.c_str());
        CopyWifiCredentialField(store.slots[target].password, sizeof(store.slots[target].password), password.c_str());
        store.preferred = target;
        changed = true;
    }
    if (!changed) {
        return ESP_OK;  // identical credential already preferred; skip the NVS commit
    }
    return SaveWifiCredentialStore(store);
}

esp_err_t MarkWifiSlotPreferred(uint8_t index)
{
    WifiCredentialStore store;
    ESP_RETURN_ON_ERROR(LoadWifiCredentialStore(&store), kTag, "load store for mark preferred");
    if (index >= store.count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (store.preferred == index) {
        return ESP_OK;  // unchanged; skip the NVS commit
    }
    store.preferred = index;
    return SaveWifiCredentialStore(store);
}

esp_err_t LoadWifiCredentials(std::string* ssid, std::string* password)
{
    if (ssid == nullptr || password == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Back-compat shim over the slot store: returns the preferred slot. Empty
    // store yields empty strings (matches the legacy LoadStringFromNvs contract
    // callers already handle).
    WifiCredentialStore store;
    ESP_RETURN_ON_ERROR(LoadWifiCredentialStore(&store), kTag, "load wifi credential store");
    if (store.count == 0) {
        ssid->clear();
        password->clear();
        return ESP_OK;
    }
    *ssid = store.slots[store.preferred].ssid;
    *password = store.slots[store.preferred].password;
    return ESP_OK;
}

esp_err_t SaveWifiCredentials(const std::string& ssid, const std::string& password)
{
    return UpsertWifiCredential(ssid, password);
}

esp_err_t ClearWifiCredentials()
{
    StorageWriteGuard write("clear-wifi-credentials", __FILE__, __LINE__);
    if (!write) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ClearNvsKey(kWifiCredsBlobKey);
    const esp_err_t legacy_ssid = ClearNvsKey(kWifiSsidKey);
    const esp_err_t legacy_pass = ClearNvsKey(kWifiPasswordKey);
    if (result == ESP_OK) {
        result = legacy_ssid;
    }
    if (result == ESP_OK) {
        result = legacy_pass;
    }
    return result;
}

bool HasWifiCredentials()
{
    WifiCredentialStore store;
    return LoadWifiCredentialStore(&store) == ESP_OK && store.count > 0;
}

esp_err_t PrepareStorageForSleep(int64_t deadline_us)
{
    if (deadline_us > 0 && esp_timer_get_time() >= deadline_us) {
        return ESP_ERR_TIMEOUT;
    }
    if (runtime::ActiveSleepBlockerCount(runtime::SleepBlocker::kStorage) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return PrepareWordObservationOutboxForSleep(deadline_us);
}

void RollbackStorageAfterSleepAbort()
{
    // Accepted writes are synchronous NVS commits or fclose/rename SPIFFS
    // transactions. The global quiesce gate reopening is the only rollback.
}

}  // namespace wqn
