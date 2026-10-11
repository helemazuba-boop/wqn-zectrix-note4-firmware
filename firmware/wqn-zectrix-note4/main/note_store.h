#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "device_protocol/note_study.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "outbox_suspend_reason.h"

namespace wqn {

// Session item/snapshot vectors are bounded (<=500 items, <=32 snapshot) but a
// full snapshot of 500 items is ~60 KB; pin them into PSRAM to match word_store
// and keep the ~298 KB internal heap free for rendering.
template <typename T>
struct NoteStorePsramAllocator {
    using value_type = T;
    NoteStorePsramAllocator() noexcept = default;
    template <typename U>
    NoteStorePsramAllocator(const NoteStorePsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        if (count == 0) {
            return nullptr;
        }
        void* memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM);
        if (memory == nullptr) abort();
        return static_cast<T*>(memory);
    }
    void deallocate(T* memory, std::size_t) noexcept { heap_caps_free(memory); }
};

template <typename A, typename B>
bool operator==(const NoteStorePsramAllocator<A>&, const NoteStorePsramAllocator<B>&) noexcept
{
    return true;
}
template <typename A, typename B>
bool operator!=(const NoteStorePsramAllocator<A>&, const NoteStorePsramAllocator<B>&) noexcept
{
    return false;
}

struct StoredNotePackSnapshot {
    char notebook_id[37] = {};
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    char sha256[65] = {};
};

struct StoredNoteSessionItem {
    char item_id[37] = {};
    char notebook_id[37] = {};
    uint64_t ordinal = 0;
    // Last-viewed pin as of session creation; empty means never viewed. Kept so
    // a resumed title list can render "上次看过" without re-fetching.
    char last_opened_at[33] = {};
};

struct StoredNoteSessionData {
    std::string session_id;
    protocol::note_study_v1::Mode mode =
        protocol::note_study_v1::Mode::kSequential;
    protocol::note_study_v1::Ordering ordering =
        protocol::note_study_v1::Ordering::kSequentialNoteV1;
    std::string seed;
    // Note sessions scope exactly one notebook (笔记本 -> 标题 -> 笔记).
    std::string notebook_id;
    bool include_archived = false;
    int optional_count = 500;
    uint64_t next_sequence = 0;
    uint64_t progress_revision = 0;
    std::vector<StoredNotePackSnapshot, NoteStorePsramAllocator<StoredNotePackSnapshot>> snapshot;
    std::vector<StoredNoteSessionItem, NoteStorePsramAllocator<StoredNoteSessionItem>> items;
    std::string cursor;
    bool has_more = false;
};

// The server-issued session plus the exact local selection that may be resumed
// after reset. Content revisions in remote.snapshot stay pinned for the record's
// lifetime. `position` is the highlighted title index (not a linear card cursor).
struct PersistedNoteSession {
    bool active = false;
    bool paused = false;
    uint32_t position = 0;
    StoredNoteSessionData remote;
};

esp_err_t CompactNoteSessionData(
    const protocol::note_study_v1::SessionData& source,
    StoredNoteSessionData* destination);

// A durable `opened` observation (device v1 emits only this action). Carries the
// advanced next_sequence/next_position so load reconciliation can restore the
// cursor if the session save was interrupted after the journal append.
struct DurableNoteObservation {
    std::string request_id;
    std::string session_id;
    uint64_t sequence = 0;
    std::string item_id;
    protocol::note_study_v1::ObservationAction action =
        protocol::note_study_v1::ObservationAction::kOpened;
    protocol::note_study_v1::Mode mode =
        protocol::note_study_v1::Mode::kSequential;
    std::string occurred_at;
    uint64_t next_sequence = 0;
    uint32_t next_position = 0;
};

struct NoteOutboxSnapshot {
    size_t pending_count = 0;
    // Records parked by SuspendPendingNoteObservation: excluded from the
    // upload queue but still resident on device awaiting intervention.
    size_t suspended_count = 0;
    // Pending successors held because an earlier record in their session is
    // suspended. Other sessions remain eligible for upload.
    size_t blocked_count = 0;
    size_t capacity = 0;
};

inline constexpr size_t kNoteObservationOutboxCapacity = 1000;

// A single active note session at a time: entering a different notebook creates
// a new session (the server retires the previous one), so one durable slot.
esp_err_t LoadPersistedNoteSession(PersistedNoteSession* session);
esp_err_t SavePersistedNoteSession(const PersistedNoteSession& session);
esp_err_t ClearPersistedNoteSession();

// Commits the observation first, then the advanced session cursor. Retrying the
// same request_id is idempotent; if the second write is interrupted, load
// reconciliation advances the session from the durable outbox record.
esp_err_t CommitNoteObservation(
    const DurableNoteObservation& observation,
    const PersistedNoteSession& advanced_session);
esp_err_t PeekPendingNoteObservation(DurableNoteObservation* observation);
esp_err_t AcknowledgeNoteObservation(const std::string& request_id);
// Moves one permanently rejected observation to a bounded forensic journal, then
// removes it from the upload queue so a terminal server error cannot wedge the
// durable head.
esp_err_t QuarantinePendingNoteObservation(const std::string& request_id);
// Parks one observation whose server-side disposition forbids unilateral
// deletion. The head is durably marked and skipped by
// PeekPendingNoteObservation so the queue keeps advancing, but the payload
// stays recoverable on device pending human intervention.
esp_err_t SuspendPendingNoteObservation(
    const std::string& request_id,
    OutboxSuspendReason reason);
esp_err_t ReadNoteOutboxSnapshot(NoteOutboxSnapshot* snapshot);
esp_err_t PrepareNoteObservationOutboxForSleep(int64_t deadline_us);

}  // namespace wqn
