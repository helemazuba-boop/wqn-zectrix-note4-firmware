#pragma once

#include <cstdint>

#include "esp_err.h"

namespace wqn::services {

using StorageTransaction = esp_err_t (*)(void* context);
using StorageContextCleanup = void (*)(void* context);

// Starts the sole task allowed to execute NVS/SPIFFS write transactions.
// Typed command/result envelopes are copied into fixed queues. Transactions
// use caller-owned contexts and complete synchronously, so the context remains
// valid until its matching result is returned.
esp_err_t StartStorageService();
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

// Consumes an independently owned context on every path when cleanup != null.
// A finite absolute esp_timer deadline covers queue admission and observation.
// Timeout may detach a RUNNING transaction; its context/completion remain alive
// until the owner finishes. The transaction must own every referenced payload
// and resource hold; owner must be a static string. No inline owner-task call.
// This bounds the caller wait, not SPIFFS/NVS execution or remote side effects.
esp_err_t ExecuteOwnedStorageTransactionUntil(
    StorageTransaction transaction, void* context, StorageContextCleanup cleanup,
    const char* owner, int64_t deadline_us);
bool IsStorageServiceTask();

}  // namespace wqn::services
