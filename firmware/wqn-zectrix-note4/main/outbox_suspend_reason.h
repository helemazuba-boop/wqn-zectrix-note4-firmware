// Shared outbox suspension vocabulary.
//
// Layering note: this header lives beside the durable-store adapters (not
// under services/) because word/note/problem stores persist the reason byte
// inside suspend-marker records. Services map server error codes onto these
// reasons; stores only carry them opaquely.

#pragma once

#include <cstdint>

namespace wqn {

// Why an outbox head was parked instead of acknowledged, quarantined, or
// retried. Persisted in the suspend-marker record's reserved field; values
// are part of the on-disk format (new reasons append only).
enum class OutboxSuspendReason : uint8_t {
    kNone = 0,
    // REQUEST_ID_REUSED: the idempotency key collides with a different
    // payload. Unilateral deletion is forbidden (audit §16.D); the record
    // waits for diagnosis.
    kIdempotencyConflict = 1,
    // SESSION_ACTOR_MISMATCH: ownership conflict after re-pairing. The
    // server session belongs to another actor; nothing may be dropped.
    kActorOwnership = 2,
    // INVALID_STUDY_OBSERVATION: identity fields corrupt locally. Dropping
    // would strand later same-session sequences in SEQUENCE_GAP.
    kInvalidIdentity = 3,
    // UPGRADE_REQUIRED: firmware too old for the current control contract.
    // Records wait for OTA; never quarantined.
    kProtocolBlocked = 4,
    // Any future unrecognized non-retryable terminal code: conservative
    // default preserves the payload for inspection.
    kUnknownTerminal = 5,
};

inline constexpr const char* OutboxSuspendReasonName(
    OutboxSuspendReason reason)
{
    switch (reason) {
        case OutboxSuspendReason::kIdempotencyConflict:
            return "idempotency-conflict";
        case OutboxSuspendReason::kActorOwnership:
            return "actor-ownership";
        case OutboxSuspendReason::kInvalidIdentity:
            return "invalid-identity";
        case OutboxSuspendReason::kProtocolBlocked:
            return "protocol-blocked";
        case OutboxSuspendReason::kUnknownTerminal:
            return "unknown-terminal";
        case OutboxSuspendReason::kNone:
            break;
    }
    return "none";
}

}  // namespace wqn
