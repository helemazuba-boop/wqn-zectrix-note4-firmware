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
esp_err_t ExecuteStorageTransactionNamed(
    StorageTransaction transaction,
    void* context,
    const char* owner);
// Interactive persistence uses a separate bounded queue. The storage owner
// still executes one transaction at a time, but always drains foreground work
// before starting the next background transaction.
esp_err_t ExecuteForegroundStorageTransaction(
    StorageTransaction transaction,
    void* context,
    const char* owner);
bool IsStorageServiceTask();

}  // namespace wqn::services
