#pragma once
#include "runtime/storage_evidence.h"
namespace wqn::services {
enum class StorageRecoveryExportKind : uint8_t { kLogical, kRaw };
struct StorageRecoveryExportResult {
    esp_err_t error = ESP_FAIL, source_error = ESP_FAIL, transfer_error = ESP_FAIL;
    uint32_t persisted_data_bytes = 0, persisted_frames = 0;
    bool source_complete = false, commit_acknowledged = false;
    std::array<uint8_t, 16> capture_id{};
    std::array<uint8_t, 32> manifest_sha256{};
};
// Boot-recovery owner only. Caller must already validate fresh device consent.
esp_err_t RunStorageRecoveryExportOnOwner(StorageRecoveryExportKind kind,
    bool (*cancelled)(void*), void* context, int64_t deadline_us,
    void (*progress)(uint32_t, void*), StorageRecoveryExportResult* result);
}
