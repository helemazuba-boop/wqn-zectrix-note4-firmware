#pragma once

#include <cstdint>

namespace wqn::services {

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

}  // namespace wqn::services
