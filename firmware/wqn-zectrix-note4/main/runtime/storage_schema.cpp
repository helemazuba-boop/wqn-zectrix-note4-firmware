#include "runtime/storage_schema.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "runtime/sleep_coordinator.h"

namespace {

constexpr char kTag[] = "storage_schema";
constexpr char kSchemaNamespace[] = "wqn_boot";
constexpr char kGenerationKey[] = "schema_gen";
constexpr char kResetIdKey[] = "reset_id";
constexpr char kResetIntentKey[] = "reset_pending";
constexpr char kStoragePartitionLabel[] = "storage";
constexpr char kStorageBasePath[] = "/storage";
constexpr size_t kResetIdBytes = 16;
using ResetId = std::array<uint8_t, kResetIdBytes>;
// Fixed wire layout: magic + protocol version + little-endian target generation.
constexpr std::array<uint8_t, 12> kIntentHeader = {
    'W', 'Q', 'N', 'R', 'S', 'T', 1, 0, 3, 0, 0, 0};
static_assert(wqn::runtime::kStorageSchemaGeneration == 3);
using ResetIntent = std::array<uint8_t, kIntentHeader.size() + kResetIdBytes>;

static_assert(sizeof(kGenerationKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kResetIdKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kResetIntentKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");

struct NvsHandle {
    ~NvsHandle()
    {
        if (handle != 0) {
            nvs_close(handle);
        }
    }

    nvs_handle_t handle = 0;
};

constexpr bool IsCurrentSchema(bool generation_found, uint32_t generation, size_t reset_id_size)
{
    return generation_found &&
        generation == wqn::runtime::kStorageSchemaGeneration &&
        reset_id_size == kResetIdBytes;
}

static_assert(IsCurrentSchema(true, 3, 16));
static_assert(!IsCurrentSchema(false, 3, 16));
static_assert(!IsCurrentSchema(true, 2, 16));
static_assert(!IsCurrentSchema(true, 3, 0));

void FormatResetId(const std::array<uint8_t, kResetIdBytes>& bytes, char* output, size_t output_size)
{
    if (output == nullptr || output_size < (bytes.size() * 2 + 1)) {
        return;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        std::snprintf(output + i * 2, output_size - i * 2, "%02x", bytes[i]);
    }
}

esp_err_t ReadSchemaMarker(
    uint32_t* generation,
    bool* generation_found,
    std::array<uint8_t, kResetIdBytes>* reset_id,
    size_t* reset_id_size)
{
    if (generation == nullptr || generation_found == nullptr || reset_id == nullptr ||
        reset_id_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *generation = 0;
    *generation_found = false;
    reset_id->fill(0);
    *reset_id_size = 0;

    NvsHandle nvs;
    esp_err_t result = nvs_open(kSchemaNamespace, NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open storage schema marker");

    result = nvs_get_u32(nvs.handle, kGenerationKey, generation);
    if (result == ESP_OK) {
        *generation_found = true;
    } else if (result != ESP_ERR_NVS_NOT_FOUND) {
        return result;
    }

    size_t required_size = 0;
    result = nvs_get_blob(nvs.handle, kResetIdKey, nullptr, &required_size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    *reset_id_size = required_size;
    if (required_size != reset_id->size()) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        nvs_get_blob(nvs.handle, kResetIdKey, reset_id->data(), &required_size),
        kTag,
        "read storage reset id");
    *reset_id_size = required_size;
    return ESP_OK;
}

esp_err_t CommitSchemaMarker(const std::array<uint8_t, kResetIdBytes>& reset_id)
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(
        nvs_open(kSchemaNamespace, NVS_READWRITE, &nvs.handle),
        kTag,
        "open storage schema marker for commit");
    ESP_RETURN_ON_ERROR(
        nvs_set_u32(
            nvs.handle,
            kGenerationKey,
            wqn::runtime::kStorageSchemaGeneration),
        kTag,
        "stage storage schema generation");
    ESP_RETURN_ON_ERROR(
        nvs_set_blob(nvs.handle, kResetIdKey, reset_id.data(), reset_id.size()),
        kTag,
        "stage storage reset id");
    return nvs_commit(nvs.handle);
}

esp_err_t ReadResetIntent(bool* found, ResetId* reset_id)
{
    *found = false;
    NvsHandle nvs;
    esp_err_t result = nvs_open(kSchemaNamespace, NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open reset intent");
    size_t size = 0;
    result = nvs_get_blob(nvs.handle, kResetIntentKey, nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "read reset intent size");
    if (size != sizeof(ResetIntent)) {
        return ESP_ERR_INVALID_STATE;
    }
    ResetIntent intent = {};
    ESP_RETURN_ON_ERROR(
        nvs_get_blob(nvs.handle, kResetIntentKey, intent.data(), &size),
        kTag, "read reset intent");
    if (size != intent.size() ||
        std::memcmp(intent.data(), kIntentHeader.data(), kIntentHeader.size()) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    std::memcpy(reset_id->data(), intent.data() + kIntentHeader.size(), reset_id->size());
    *found = true;
    return ESP_OK;
}

esp_err_t CommitResetIntent(const ResetId& reset_id)
{
    ResetIntent intent = {};
    std::memcpy(intent.data(), kIntentHeader.data(), kIntentHeader.size());
    std::memcpy(intent.data() + kIntentHeader.size(), reset_id.data(), reset_id.size());
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(
        nvs_open(kSchemaNamespace, NVS_READWRITE, &nvs.handle),
        kTag, "open reset intent for commit");
    ESP_RETURN_ON_ERROR(
        nvs_set_blob(nvs.handle, kResetIntentKey, intent.data(), intent.size()),
        kTag, "stage reset intent");
    return nvs_commit(nvs.handle);
}

esp_err_t ClearResetIntent()
{
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(
        nvs_open(kSchemaNamespace, NVS_READWRITE, &nvs.handle),
        kTag, "open completed reset intent");
    ESP_RETURN_ON_ERROR(
        nvs_erase_key(nvs.handle, kResetIntentKey), kTag, "clear completed reset intent");
    return nvs_commit(nvs.handle);
}

esp_err_t FindNvsEntryOutsideSchema(nvs_entry_info_t* entry, bool* found)
{
    *found = false;
    nvs_iterator_t iterator = nullptr;
    esp_err_t result = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &iterator);
    while (result == ESP_OK) {
        result = nvs_entry_info(iterator, entry);
        if (result != ESP_OK) {
            break;
        }
        if (std::strcmp(entry->namespace_name, kSchemaNamespace) != 0) {
            *found = true;
            break;
        }
        result = nvs_entry_next(&iterator);
    }
    // Never mutate NVS while an iterator into it remains live.
    nvs_release_iterator(iterator);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

esp_err_t ClearBusinessNvs()
{
    for (;;) {
        nvs_entry_info_t entry = {};
        bool found = false;
        ESP_RETURN_ON_ERROR(
            FindNvsEntryOutsideSchema(&entry, &found), kTag, "enumerate reset namespaces");
        if (!found) {
            // The iterator omits legacy blob entries. Check physical entry
            // counts before treating all business namespaces as cleared.
            nvs_stats_t stats = {};
            ESP_RETURN_ON_ERROR(nvs_get_stats("nvs", &stats), kTag, "check reset NVS stats");
            NvsHandle schema;
            ESP_RETURN_ON_ERROR(
                nvs_open(kSchemaNamespace, NVS_READONLY, &schema.handle),
                kTag, "open retained schema");
            size_t schema_entries = 0;
            ESP_RETURN_ON_ERROR(
                nvs_get_used_entry_count(schema.handle, &schema_entries),
                kTag, "count retained schema entries");
            return stats.used_entries >= stats.namespace_count &&
                stats.used_entries - stats.namespace_count == schema_entries
                ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        NvsHandle nvs;
        ESP_RETURN_ON_ERROR(
            nvs_open(entry.namespace_name, NVS_READWRITE, &nvs.handle),
            kTag, "open reset namespace");
        ESP_RETURN_ON_ERROR(nvs_erase_all(nvs.handle), kTag, "clear reset namespace");
        ESP_RETURN_ON_ERROR(nvs_commit(nvs.handle), kTag, "commit reset namespace");
    }
}

esp_err_t VerifyEmptyMedia()
{
    nvs_iterator_t iterator = nullptr;
    const esp_err_t entry_result = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &iterator);
    nvs_release_iterator(iterator);
    if (entry_result != ESP_ERR_NVS_NOT_FOUND) {
        return entry_result == ESP_OK ? ESP_ERR_INVALID_STATE : entry_result;
    }
    nvs_stats_t stats = {};
    ESP_RETURN_ON_ERROR(nvs_get_stats("nvs", &stats), kTag, "check empty NVS stats");
    if (stats.used_entries != stats.namespace_count) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, kStoragePartitionLabel);
    if (partition == nullptr || partition->size == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    // Mount failure cannot distinguish blank media from damaged data. Check
    // every byte, only on the no-marker/no-NVS-keys initialization path.
    std::array<uint8_t, 1024> bytes;
    for (size_t offset = 0; offset < partition->size;) {
        const size_t size = partition->size - offset < bytes.size()
            ? partition->size - offset : bytes.size();
        ESP_RETURN_ON_ERROR(
            esp_partition_read(partition, offset, bytes.data(), size),
            kTag, "read storage before initialization");
        for (size_t i = 0; i < size; ++i) {
            if (bytes[i] != 0xff) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        offset += size;
    }
    return ESP_OK;
}

esp_err_t ValidateStorageSpiffs()
{
    esp_vfs_spiffs_conf_t config = {};
    config.base_path = kStorageBasePath;
    config.partition_label = kStoragePartitionLabel;
    config.max_files = 1;
    config.format_if_mount_failed = false;

    ESP_RETURN_ON_ERROR(
        esp_vfs_spiffs_register(&config),
        kTag,
        "mount storage SPIFFS for schema validation");

    size_t total = 0;
    size_t used = 0;
    const esp_err_t info_result =
        esp_spiffs_info(kStoragePartitionLabel, &total, &used);
    const esp_err_t unmount_result =
        esp_vfs_spiffs_unregister(kStoragePartitionLabel);
    if (info_result != ESP_OK) {
        return info_result;
    }
    if (unmount_result != ESP_OK) {
        return unmount_result;
    }
    ESP_LOGI(
        kTag,
        "storage SPIFFS validated: total=%u used=%u",
        static_cast<unsigned>(total),
        static_cast<unsigned>(used));
    return ESP_OK;
}

esp_err_t PerformAuthorizedLocalReset(const ResetId& reset_id)
{
    if (esp_spiffs_mounted(kStoragePartitionLabel)) {
        ESP_RETURN_ON_ERROR(
            esp_vfs_spiffs_unregister(kStoragePartitionLabel),
            kTag,
            "unmount storage SPIFFS before reset");
    }

    // Preserve wqn_boot, including the committed authorization, across every
    // destructive step. A missing marker alone never authorizes replay.
    ESP_RETURN_ON_ERROR(ClearBusinessNvs(), kTag, "clear authorized NVS data");
    ESP_RETURN_ON_ERROR(
        esp_spiffs_format(kStoragePartitionLabel),
        kTag,
        "format storage SPIFFS partition");
    ESP_RETURN_ON_ERROR(
        ValidateStorageSpiffs(),
        kTag,
        "validate formatted storage SPIFFS partition");

    ESP_RETURN_ON_ERROR(CommitSchemaMarker(reset_id), kTag, "commit final storage marker");
    return ClearResetIntent();
}

}  // namespace

namespace wqn::runtime {

StorageSchemaBootResult EnsureStorageSchema()
{
    StorageSchemaBootResult boot;
    // This boot-time transaction runs before StorageService exists, but it is
    // still a flash write owner. Keep automatic light sleep out of NVS clearing,
    // SPIFFS format/validation and the final generation-marker commit.
    SleepLease storage_lease = SleepLease::TryAcquire(
        SleepBlocker::kStorage,
        "storage-schema-boot",
        __FILE__,
        __LINE__);
    if (!storage_lease) {
        boot.error = ESP_ERR_INVALID_STATE;
        ESP_LOGE(kTag, "storage schema boot lease acquisition failed");
        return boot;
    }

    const esp_err_t init_result = nvs_flash_init();
    if (init_result != ESP_OK) {
        boot.error = init_result;
        ESP_LOGE(kTag, "NVS schema check init failed: %s", esp_err_to_name(init_result));
        return boot;
    }

    uint32_t generation = 0;
    bool generation_found = false;
    std::array<uint8_t, kResetIdBytes> reset_id = {};
    size_t reset_id_size = 0;
    const esp_err_t read_result = ReadSchemaMarker(
        &generation, &generation_found, &reset_id, &reset_id_size);
    boot.observed_generation = generation;
    if (read_result != ESP_OK) {
        boot.error = read_result;
        ESP_LOGE(kTag, "storage marker unreadable; preserving data: %s",
                 esp_err_to_name(read_result));
        return boot;
    }
    // Even a valid intent from this firmware must not erase a newer schema.
    if (generation_found && generation > kStorageSchemaGeneration) {
        boot.error = ESP_ERR_INVALID_STATE;
        ESP_LOGE(kTag, "future storage generation=%lu; preserving data",
                 static_cast<unsigned long>(generation));
        return boot;
    }
    bool pending = false;
    ResetId pending_id = {};
    const esp_err_t intent_result = ReadResetIntent(&pending, &pending_id);
    if (intent_result != ESP_OK) {
        boot.error = intent_result;
        ESP_LOGE(kTag, "storage reset intent unreadable; preserving data: %s",
                 esp_err_to_name(intent_result));
        return boot;
    }

    if (!pending && IsCurrentSchema(generation_found, generation, reset_id_size)) {
        const esp_err_t spiffs_result = ValidateStorageSpiffs();
        if (spiffs_result != ESP_OK) {
            boot.error = spiffs_result;
            ESP_LOGE(
                kTag,
                "storage SPIFFS validation failed; business startup blocked: %s",
                esp_err_to_name(spiffs_result));
            return boot;
        }
        FormatResetId(reset_id, boot.reset_id, sizeof(boot.reset_id));
        boot.action = StorageSchemaBootAction::kReady;
        boot.error = ESP_OK;
        ESP_LOGI(
            kTag,
            "storage schema ready: generation=%lu reset_id=%s",
            static_cast<unsigned long>(generation),
            boot.reset_id);
        return boot;
    }

    if (!pending) {
        // Generation zero + complete old reset id is the previous firmware's
        // explicit factory-reset request. Old generations 1/2 are not consent.
        const bool legacy_reset = generation_found && generation == 0 &&
            reset_id_size == kResetIdBytes;
        esp_err_t authorization = ESP_ERR_INVALID_STATE;
        if (legacy_reset) {
            authorization = ESP_OK;
        } else if (!generation_found && reset_id_size == 0) {
            authorization = VerifyEmptyMedia();
        }
        if (authorization != ESP_OK) {
            boot.error = authorization;
            ESP_LOGE(kTag, "no authorized storage reset; preserving generation=%lu",
                     static_cast<unsigned long>(generation));
            return boot;
        }
        esp_fill_random(pending_id.data(), pending_id.size());
        const esp_err_t commit_result = CommitResetIntent(pending_id);
        if (commit_result != ESP_OK) {
            boot.error = commit_result;
            return boot;
        }
    }
    ESP_LOGW(kTag, "replaying authorized storage reset");
    const esp_err_t reset_result = PerformAuthorizedLocalReset(pending_id);
    if (reset_result != ESP_OK) {
        boot.error = reset_result;
        ESP_LOGE(
            kTag,
            "storage schema reset failed; business startup blocked: %s",
            esp_err_to_name(reset_result));
        return boot;
    }

    FormatResetId(pending_id, boot.reset_id, sizeof(boot.reset_id));
    boot.action = StorageSchemaBootAction::kRestartRequired;
    boot.error = ESP_OK;
    ESP_LOGW(
        kTag,
        "storage schema committed: generation=%lu reset_id=%s restart required",
        static_cast<unsigned long>(kStorageSchemaGeneration),
        boot.reset_id);
    return boot;
}

esp_err_t InvalidateStorageSchemaForFactoryReset()
{
    uint32_t generation = 0;
    bool found = false;
    ResetId reset_id = {};
    size_t reset_id_size = 0;
    ESP_RETURN_ON_ERROR(
        ReadSchemaMarker(&generation, &found, &reset_id, &reset_id_size),
        kTag, "read schema before factory reset");
    if (!IsCurrentSchema(found, generation, reset_id_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool pending = false;
    ESP_RETURN_ON_ERROR(ReadResetIntent(&pending, &reset_id), kTag, "read existing reset intent");
    if (pending) {
        return ESP_OK;
    }
    esp_fill_random(reset_id.data(), reset_id.size());
    return CommitResetIntent(reset_id);
}

}  // namespace wqn::runtime
