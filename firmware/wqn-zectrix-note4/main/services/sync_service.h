#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace wqn::services {

struct SyncSnapshot {
    bool task_running = false;
    bool last_round_success = false;
    uint32_t interval_minutes = 0;
    uint32_t success_count = 0;
    uint32_t failure_count = 0;
    int64_t last_started_ms = 0;
    int64_t last_finished_ms = 0;
    char status[64] = {};
    char claim_code[9] = {};
    uint64_t claim_expires_at_ms = 0;
};

enum class SyncEventStatus : uint8_t {
    kSucceeded,
    kFailed,
    kAwaitingClaim,
};

struct SyncEvent {
    SyncEventStatus status = SyncEventStatus::kFailed;
    uint32_t sequence = 0;
    int64_t finished_ms = 0;
    char claim_code[9] = {};
    uint64_t claim_expires_at_ms = 0;
};

using SyncEventSink = void (*)(const SyncEvent& event);

// Installs the consumer for immutable sync-domain events. The callback runs on
// SyncService's task and must only copy or enqueue the fixed-size event.
void SetSyncEventSink(SyncEventSink sink);

esp_err_t StartSyncService();
void RequestSyncNow();
void GetSyncSnapshot(SyncSnapshot* snapshot);
TickType_t GetConfiguredSyncDelayTicks();
bool HasUsableStoredToken();

}  // namespace wqn::services
