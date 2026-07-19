#include "runtime/storage_schema.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
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
constexpr char kStoragePartitionLabel[] = "storage";
constexpr char kStorageBasePath[] = "/storage";
constexpr size_t kResetIdBytes = 16;

static_assert(sizeof(kGenerationKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");
static_assert(sizeof(kResetIdKey) <= 16, "NVS key must fit ESP-IDF's 15-character limit");

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

esp_err_t DeinitNvsIfNeeded()
{
    const esp_err_t result = nvs_flash_deinit();
    return result == ESP_ERR_INVALID_STATE || result == ESP_ERR_NVS_NOT_INITIALIZED
        ? ESP_OK
        : result;
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

esp_err_t PerformFullLocalReset(std::array<uint8_t, kResetIdBytes>* reset_id)
{
    if (reset_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (esp_spiffs_mounted(kStoragePartitionLabel)) {
        ESP_RETURN_ON_ERROR(
            esp_vfs_spiffs_unregister(kStoragePartitionLabel),
            kTag,
            "unmount storage SPIFFS before reset");
    }

    ESP_RETURN_ON_ERROR(DeinitNvsIfNeeded(), kTag, "deinit NVS before reset");
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "erase default NVS partition");
    ESP_RETURN_ON_ERROR(nvs_flash_init(), kTag, "reinitialize default NVS partition");

    // The schema marker deliberately remains absent until SPIFFS formatting
    // succeeds. A reset or brownout anywhere above/below repeats the sequence.
    ESP_RETURN_ON_ERROR(
        esp_spiffs_format(kStoragePartitionLabel),
        kTag,
        "format storage SPIFFS partition");
    ESP_RETURN_ON_ERROR(
        ValidateStorageSpiffs(),
        kTag,
        "validate formatted storage SPIFFS partition");

    esp_fill_random(reset_id->data(), reset_id->size());
    return CommitSchemaMarker(*reset_id);
}

}  // namespace

namespace wqn::runtime {

StorageSchemaBootResult EnsureStorageSchema()
{
    StorageSchemaBootResult boot;
    // This boot-time transaction runs before StorageService exists, but it is
    // still a flash write owner. Keep automatic light sleep out of NVS erase,
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

    esp_err_t init_result = nvs_flash_init();
    const bool nvs_requires_reset =
        init_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        init_result == ESP_ERR_NVS_NEW_VERSION_FOUND;
    if (init_result != ESP_OK && !nvs_requires_reset) {
        boot.error = init_result;
        ESP_LOGE(kTag, "NVS schema check init failed: %s", esp_err_to_name(init_result));
        return boot;
    }

    uint32_t generation = 0;
    bool generation_found = false;
    std::array<uint8_t, kResetIdBytes> reset_id = {};
    size_t reset_id_size = 0;
    if (!nvs_requires_reset) {
        const esp_err_t read_result = ReadSchemaMarker(
            &generation,
            &generation_found,
            &reset_id,
            &reset_id_size);
        if (read_result != ESP_OK) {
            // Marker corruption is itself an untrusted generation. Local data
            // is explicitly disposable for generation 3, so route it through
            // the same idempotent reset instead of starting a dead-end mode.
            ESP_LOGW(
                kTag,
                "storage schema marker unreadable; forcing reset: %s",
                esp_err_to_name(read_result));
            generation = 0;
            generation_found = false;
            reset_id_size = 0;
        }
    }
    boot.observed_generation = generation;

    if (!nvs_requires_reset &&
        IsCurrentSchema(generation_found, generation, reset_id_size)) {
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

    ESP_LOGW(
        kTag,
        "storage schema reset required: found=%d observed=%lu target=%lu reset_id_bytes=%u nvs_init=%s",
        generation_found ? 1 : 0,
        static_cast<unsigned long>(generation),
        static_cast<unsigned long>(kStorageSchemaGeneration),
        static_cast<unsigned>(reset_id_size),
        esp_err_to_name(init_result));

    reset_id.fill(0);
    const esp_err_t reset_result = PerformFullLocalReset(&reset_id);
    if (reset_result != ESP_OK) {
        boot.error = reset_result;
        ESP_LOGE(
            kTag,
            "storage schema reset failed; business startup blocked: %s",
            esp_err_to_name(reset_result));
        return boot;
    }

    FormatResetId(reset_id, boot.reset_id, sizeof(boot.reset_id));
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
    NvsHandle nvs;
    ESP_RETURN_ON_ERROR(
        nvs_open(kSchemaNamespace, NVS_READWRITE, &nvs.handle),
        kTag,
        "open storage schema marker for invalidation");
    ESP_RETURN_ON_ERROR(
        nvs_set_u32(nvs.handle, kGenerationKey, 0),
        kTag,
        "invalidate storage schema generation");
    return nvs_commit(nvs.handle);
}

}  // namespace wqn::runtime
