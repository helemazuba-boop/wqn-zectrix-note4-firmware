#pragma once

#include <array>
#include <cstddef>
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

enum class StorageRecoveryResetPolicy : uint8_t {
    kUnavailable,
    kAllowed,
    kAlreadyPending,
    kNewerFirmwareRequired,
};

// Metadata only. Never exports credentials, business entries or a partition
// image. Reads do not initialize, format, migrate or authorize a reset.
struct StorageRecoverySnapshot {
    esp_err_t error = ESP_FAIL;
    uint32_t observed_generation = 0;
    bool generation_found = false;
    size_t reset_id_size = 0;
    std::array<uint8_t, 16> reset_id{};
    std::array<uint8_t, 16> pending_reset_id{};
    StorageRecoveryResetPolicy reset_policy = StorageRecoveryResetPolicy::kUnavailable;
};

esp_err_t InspectStorageRecovery(StorageRecoverySnapshot* snapshot);

enum class StorageRecoveryResetOutcome : uint8_t {
    kRejected,
    kRecorded,
    kUncertain,
};

struct StorageRecoveryResetResult {
    esp_err_t error = ESP_FAIL;
    StorageRecoveryResetOutcome outcome = StorageRecoveryResetOutcome::kRejected;
};

// Recovery-owner-only, after explicit local confirmation and before business
// services. Revalidates the metadata the user confirmed. Records intent only;
// no live format/erase/restart. A failed intent write may still have persisted.
StorageRecoveryResetResult AuthorizeStorageRecoveryReset(
    const StorageRecoverySnapshot& expected);

}  // namespace wqn::runtime
