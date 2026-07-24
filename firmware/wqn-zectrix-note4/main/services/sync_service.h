#pragma once

#include <cstdint>

#include "esp_err.h"
#include "device_protocol/v3.h"
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

enum class SyncEventScope : uint8_t {
    kFull,
    kWordOutbox,
};

struct SyncEvent {
    SyncEventStatus status = SyncEventStatus::kFailed;
    SyncEventScope scope = SyncEventScope::kFull;
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
// Marks the beginning of an interactive word action. An in-flight outbox
// batch finishes its current idempotent item, then yields until the user has
// been quiet again.
void NoteWordInteraction();
// Coalesces repeated word observations and uploads them after a short quiet
// period. This never requests the full control/problem/content sync round.
void RequestWordOutboxUpload();
// Uploads durable `opened` note observations after the same quiet period. Note
// and word share one outbox round; this is the trigger for the note-open path.
void RequestNoteOutboxUpload();
void GetSyncSnapshot(SyncSnapshot* snapshot);
TickType_t GetConfiguredSyncDelayTicks();
bool HasUsableStoredToken();

// Returns metadata sharing the same per-boot identity as the control-plane
// bootstrap/sync requests. Callers own the fresh request_id.
wqn::protocol::v3::RequestMetadata MakeDeviceRequestMetadata();

}  // namespace wqn::services
