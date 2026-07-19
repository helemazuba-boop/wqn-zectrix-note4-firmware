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

// Runs before any business service starts. A missing/stale generation causes
// an idempotent full local reset: erase default NVS, format the storage SPIFFS
// partition, then atomically commit generation + reset id as the final step.
StorageSchemaBootResult EnsureStorageSchema();

// Marks the current generation stale. The next boot performs the destructive
// reset above; keeping the destructive work in boot context avoids racing live
// NVS/SPIFFS clients during a settings-triggered factory reset.
esp_err_t InvalidateStorageSchemaForFactoryReset();

}  // namespace wqn::runtime
