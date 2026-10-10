// Server error code taxonomy for the durable observation outboxes.
//
// Implements the seven-class failure classification from the 2026-08-19 sync
// liveness audit (§16.B) on top of the raw `error.code` strings the WQN v3
// API returns. The classifier is a pure function so both the sync service
// and contract fixture self-tests can exercise it without a task context.

#pragma once

#include <string_view>

namespace wqn::services {

enum class ServerErrorClass : uint8_t {
    // Empty code (missing/ambiguous envelope). Callers treat it as transient:
    // the server may have consumed the idempotency key before the response
    // was damaged.
    kUnrecognized,
    // Non-empty but unknown code from a newer server. Conservative default
    // downstream: park the record, never delete.
    kUnknownCode,
    // 401 UNAUTHORIZED: pause outbound work until re-pairing/token refresh.
    kAuthRequired,
    // Transport loss, retryable flag, 5xx/408/425/429 family, and legal
    // SEQUENCE_GAP: exponential backoff keeps attempting.
    kTransientRetry,
    // SEQUENCE_ALREADY_APPLIED: server proves the sequence was consumed by
    // someone. Local quarantine is safe and restores queue progress.
    kSequenceResolved,
    // SESSION_NOT_ACTIVE: session expired/completed/abandoned server-side.
    // Remaining records of the dead session may drain via quarantine.
    kSessionTerminal,
    // ITEM_NOT_VISIBLE / ITEM_NOT_IN_SESSION: business-projection rejection
    // the compensating tombstone can retire.
    kTombstoneRecoverable,
    // 426 UPGRADE_REQUIRED: firmware predates the control contract. Suspend
    // outbound polling until OTA; quarantining here would destroy data.
    kProtocolBlocked,
    // REQUEST_ID_REUSED / SESSION_ACTOR_MISMATCH / INVALID_STUDY_OBSERVATION
    // and any unrecognized terminal code: fail-stop. Records park durably
    // pending human intervention; unilateral deletion is forbidden because
    // the sequence was not consumed and later same-session records would
    // wedge in STUDY_SEQUENCE_GAP (audit §14 Case B).
    kProtocolIntegrity,
};

constexpr ServerErrorClass ClassifyServerErrorCode(std::string_view code)
{
    if (code.empty()) {
        return ServerErrorClass::kUnrecognized;
    }
    if (code == "UNAUTHORIZED") {
        return ServerErrorClass::kAuthRequired;
    }
    if (code == "SEQUENCE_GAP") {
        return ServerErrorClass::kTransientRetry;
    }
    if (code == "SEQUENCE_ALREADY_APPLIED") {
        return ServerErrorClass::kSequenceResolved;
    }
    if (code == "SESSION_NOT_ACTIVE") {
        return ServerErrorClass::kSessionTerminal;
    }
    if (code == "ITEM_NOT_VISIBLE" || code == "ITEM_NOT_IN_SESSION") {
        return ServerErrorClass::kTombstoneRecoverable;
    }
    if (code == "UPGRADE_REQUIRED") {
        return ServerErrorClass::kProtocolBlocked;
    }
    if (code == "REQUEST_ID_REUSED" || code == "SESSION_ACTOR_MISMATCH" ||
        code == "INVALID_STUDY_OBSERVATION") {
        return ServerErrorClass::kProtocolIntegrity;
    }
    return ServerErrorClass::kUnknownCode;
}

}  // namespace wqn::services
