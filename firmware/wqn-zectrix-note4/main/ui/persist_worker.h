#pragma once

#include <cstdint>

#include "esp_err.h"

#include "note_store.h"
#include "problem_store.h"
#include "word_study_store.h"

// [persist-worker] Dedicated local-write worker. Commit #1 is infrastructure
// only: no call site uses the submit APIs yet (word/note/problem/settings are
// migrated in later commits). It moves durable observation/verdict commits and
// settings saves OFF the UI task and OFF the cloud interactive lane.
//
// Ratified contract (do not weaken without re-review):
//  - The worker ONLY: pops an owned command, runs its storage transaction,
//    publishes a typed terminal result. It NEVER touches AppState or calls any
//    Apply*/reducer -- the UI task does that after consuming the result, so it
//    can also return the right UiUpdate/RefreshSchedule.
//  - Result delivery is an INDEPENDENT per-kind ACK mailbox (NOT the S3 cloud
//    result slot -> no double-writer race; NOT a plain queue -> a UI stall can
//    never lose a terminal result, and the worker never blocks publishing).
//  - SleepLease and per-kind busy have DIFFERENT lifetimes: the lease is held
//    from reservation until the storage write actually ends (released
//    worker-side); the busy stays set until the UI acks. Never cleared together.
//  - foreground storage only bounds the QUEUE wait; once a transaction starts
//    it degrades to portMAX_DELAY. This guarantees the UI/buttons/display keep
//    working, NOT that the worker can never stall or that a lease/next card
//    always frees within a fixed budget.
//
// Submit is TWO PHASE so a failed reservation never costs the caller its staged
// effect (the effect is pulled from UI state, which mutates it):
//   PersistTicket t = TryReservePersist(kind);   // busy + slot + lease, or invalid
//   if (!t.valid()) return;                       // UI state untouched; retry later
//   if (!Take...Effect(&state, ..., &payload)) {  // only now mutate UI state
//       CancelPersistReservation(t);              // nothing to commit
//       return;
//   }
//   EnqueueReserved...(t, std::move(payload));    // cannot fail after reserve

namespace device_ui_internal {

enum class PersistKind : uint8_t {
    kWordObservation = 0,
    kNoteObservation,
    kProblemVerdict,
    kSettingsAutoSync,
    kSettingsVolume,
    kSettingsDefaultDeck,
    kCount,
};

// Handle to a reserved pool slot that already holds the per-kind busy gate and
// a storage SleepLease. Invalid when reservation failed.
struct PersistTicket {
    uint8_t slot_index = 0xFF;
    uint32_t operation_id = 0;
    PersistKind kind = PersistKind::kCount;
    bool valid() const { return slot_index != 0xFF; }
};

// Terminal result pulled from the mailbox. generation + operation_id let the
// ACK prove it matches the exact result just applied, so a stale/duplicate ACK
// can never free a slot that a later command reused.
struct PersistResultReceipt {
    esp_err_t result = ESP_FAIL;
    uint32_t operation_id = 0;
    uint32_t generation = 0;
};

// Starts the worker task + its fixed command pool. Idempotent.
esp_err_t StartPersistWorker();

// --- Submit side (UI task), two phase (see the file header). ---
// Phase 1: reserve busy + a pool slot + a storage SleepLease. Returns an
// invalid ticket if the kind is busy / the pool is full / the lease is
// unavailable -- the caller has NOT touched its UI state yet, so it just
// retries next pump.
PersistTicket TryReservePersist(PersistKind kind);
// Release a reservation whose effect turned out empty (frees lease + slot +
// busy). Safe no-op on an invalid ticket.
void CancelPersistReservation(const PersistTicket& ticket);
// Phase 2: move the payload into the reserved slot and enqueue. Must be paired
// with a valid ticket of the matching kind; cannot fail after a reservation.
void EnqueueReservedWordObservation(
    const PersistTicket& ticket,
    wqn::DurableWordObservation observation,
    wqn::PersistedWordSession advanced_session);
void EnqueueReservedNoteObservation(
    const PersistTicket& ticket,
    wqn::DurableNoteObservation observation,
    wqn::PersistedNoteSession advanced_session);

// --- Consumer side (UI task). ---
// True when an unapplied terminal result is waiting for `kind`; copies it out.
bool TakePersistResultToApply(PersistKind kind, PersistResultReceipt* out);
// Validates generation + operation_id + slot ownership; only a fully matching
// ACK frees the slot and clears busy. Stale/duplicate ACKs are logged and
// ignored (returns false), never mutating state.
bool AckPersistResult(PersistKind kind, uint32_t generation, uint32_t operation_id);
// True while a command for `kind` is accepted-but-not-yet-acked (submit gate +
// duplicate-Confirm guard for settings).
bool IsPersistKindBusy(PersistKind kind);

}  // namespace device_ui_internal
