#pragma once

#include "esp_err.h"

namespace wqn::services {

using StorageTransaction = esp_err_t (*)(void* context);

// Starts the sole task allowed to execute NVS/SPIFFS write transactions.
// Typed command/result envelopes are copied into fixed queues. Transactions
// use caller-owned contexts and complete synchronously, so the context remains
// valid until its matching result is returned.
esp_err_t StartStorageService();
esp_err_t ExecuteStorageTransaction(StorageTransaction transaction, void* context);
bool IsStorageServiceTask();

}  // namespace wqn::services
