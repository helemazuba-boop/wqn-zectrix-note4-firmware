#pragma once

#include <cstdint>

#include "esp_err.h"

namespace wqn::runtime {

constexpr uint32_t kStorageSchemaGeneration = 3;

enum class StorageSchemaBootAction : uint8_t {
    kReady,
    kRestartRequired,
    kRecoveryRequired,
};

struct StorageSchemaBootResult {
    StorageSchemaBootAction action = StorageSchemaBootAction::kRecoveryRequired;
    esp_err_t error = ESP_FAIL;
    uint32_t observed_generation = 0;
    char reset_id[33] = {};
};

// Runs before business services. Only verified empty media, a complete legacy
// factory-reset request, or a durable reset intent permits initialization/reset.
// Unknown/old/future schemas and I/O errors preserve data and require recovery.
// A reset keeps its intent in wqn_boot while clearing other NVS namespaces and
// SPIFFS; final schema keys precede intent removal (not a multi-key transaction).
StorageSchemaBootResult EnsureStorageSchema();

// Records an explicit reset intent for boot; called by the storage owner during
// settings-triggered reset, without destroying live NVS/SPIFFS clients' data.
// A write/commit error does not guarantee that the intent was never persisted.
esp_err_t InvalidateStorageSchemaForFactoryReset();

}  // namespace wqn::runtime
