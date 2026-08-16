#pragma once

#include <cstdint>

#include "esp_err.h"
#include "device_protocol/v3.h"
#include "freertos/FreeRTOS.h"

namespace wqn::services {

enum class SyncContentDomain : uint8_t {
    kWordPacks,
    kNotePacks,
    kProblemPacks,
};

enum class SyncContentPhase : uint8_t {
    kClean,
    kPending,
    kFetching,
    kInstalling,
    kBackoff,
    kBlocked,
};

struct SyncContentSnapshot {
    uint64_t desired_revision = 0;
    uint64_t applied_revision = 0;
    SyncContentPhase phase = SyncContentPhase::kClean;
    uint8_t retry_attempt = 0;
    int64_t next_retry_ms = 0;
    char snapshot_id[65] = {};
    char last_error[64] = {};
};

struct SyncContentTicket {
    SyncContentDomain domain = SyncContentDomain::kWordPacks;
    uint32_t generation = 0;
    uint64_t target_revision = 0;
    explicit operator bool() const { return generation != 0; }
};

enum class SyncOutboxPhase : uint8_t {
    kDrained,
    kPending,
    kYielded,
    kBlocked,
};

struct SyncOutboxSnapshot {
    SyncOutboxPhase phase = SyncOutboxPhase::kDrained;
    uint8_t retry_attempt = 0;
    int64_t next_retry_ms = 0;
    char last_error[48] = {};
};

struct SyncSnapshot {
    bool task_running = false;
    bool last_round_success = false;
    uint32_t interval_minutes = 0;
    uint32_t success_count = 0;
    uint32_t partial_count = 0;
    uint32_t failure_count = 0;
    int64_t last_started_ms = 0;
    int64_t last_finished_ms = 0;
    char status[64] = {};
    char claim_code[9] = {};
    uint64_t claim_expires_at_ms = 0;
    uint32_t state_sequence = 0;
    uint64_t todo_revision = 0;
    SyncContentSnapshot word_packs = {};
    SyncContentSnapshot note_packs = {};
    SyncContentSnapshot problem_packs = {};
    SyncOutboxSnapshot word_outbox = {};
    SyncOutboxSnapshot note_outbox = {};
    SyncOutboxSnapshot problem_outbox = {};
};

enum class SyncEventStatus : uint8_t {
    kSucceeded,
    kPartial,
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
    // Live-only Todo target. Todo deliberately has no offline pack journal;
    // the UI uses this monotonic target to schedule an interactive refresh.
    uint64_t todo_revision = 0;
};

// Returns the newest terminal/control-plane status event. The mailbox is
// overwrite-safe: callers compare sequence and never depend on a bounded
// queue preserving every intermediate status.
void GetLatestSyncEvent(SyncEvent* event);

esp_err_t StartSyncService();
// Boot admission for ConnectivityService. Scheduled timer wakes keep WiFi off
// unless a periodic sync is due, an outbox is durable, or content convergence
// was interrupted. Non-timer boots keep normal interactive connectivity.
bool ShouldStartConnectivityAtBoot();
void RequestSyncNow();
// Token save/clear is a distinct full-sync/claim reason, not a user manual
// request. Storage publishes it after the credential mutation commits.
void NotifySyncCredentialsChanged();
// Connectivity is only a readiness signal. It wakes the scheduler but does
// not itself create a full-sync reason.
void NotifySyncConnectivityAvailable();
// Called after the async settings write is durably acknowledged. Re-arms the
// absolute RTC-retained periodic deadline without forcing an immediate sync.
void NotifyAutoSyncIntervalChanged(uint32_t minutes);
// Read-only power policy. Returns 0 when sync needs no timer, otherwise the
// seconds until its next durable periodic/retry/outbox/content obligation.
uint32_t SecondsUntilNextSyncWake();
// Arms a durable content convergence intent. The coordinator coalesces this
// with control-sync results; callers never need to relay a completion event.
void RequestContentRefresh(SyncContentDomain domain);
// Bulk-worker dispatch handshake. Claim moves Pending/Backoff -> Fetching;
// completion is called by the bulk worker before it publishes its UI result,
// so durable applied state never depends on the UI event loop.
SyncContentTicket TryClaimContentRefresh(SyncContentDomain domain);
void CancelContentRefreshClaim(const SyncContentTicket& ticket);
esp_err_t BeginContentInstall(const SyncContentTicket& ticket);
void CompleteContentRefresh(
    const SyncContentTicket& ticket,
    esp_err_t result,
    const char* snapshot_id = nullptr,
    const char* error = nullptr);
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
// Requests an upload of pending problem verdicts (same quiet-window trigger;
// all three durable queues drain in one outbox-only round).
void RequestProblemOutboxUpload();
void GetSyncSnapshot(SyncSnapshot* snapshot);
bool HasUsableStoredToken();

// Returns metadata sharing the same per-boot identity as the control-plane
// bootstrap/sync requests. Callers own the fresh request_id.
wqn::protocol::v3::RequestMetadata MakeDeviceRequestMetadata();

}  // namespace wqn::services
