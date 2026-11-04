#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "runtime/storage_schema.h"

namespace wqn::services {

enum class StorageRecoveryOperation : uint8_t { kInspect, kAuthorizeReset };

struct StorageRecoveryResult {
    uint32_t operation_id = 0;
    StorageRecoveryOperation operation = StorageRecoveryOperation::kInspect;
    esp_err_t error = ESP_FAIL;
    runtime::StorageRecoverySnapshot snapshot;
    runtime::StorageRecoveryResetResult reset;
};

// Boot recovery only. Starts the sole storage owner without business storage
// initialization, cloud, audio or normal UI. The coordinator only waits; all
// schema I/O runs on StorageService's internal stack with static request data.
esp_err_t StartStorageRecoveryService(TaskHandle_t ui_task);
bool SubmitStorageRecoveryInspect(uint32_t* operation_id);
bool SubmitStorageRecoveryReset(
    const runtime::StorageRecoverySnapshot& expected, uint32_t* operation_id);

// One UI consumer. Busy remains held through terminal result delivery until
// that consumer ACKs its exact operation; notifications carry no payload.
bool ReadStorageRecoveryResult(StorageRecoveryResult* result);
bool AcknowledgeStorageRecoveryResult(uint32_t operation_id);

}  // namespace wqn::services
