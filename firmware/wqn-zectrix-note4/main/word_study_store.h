#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "device_protocol/word_study.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "outbox_suspend_reason.h"

namespace wqn {

enum class WordPresentationPhase : uint8_t {
    kFront = 0,
    kBack = 1,
};

template <typename T>
struct WordStorePsramAllocator {
    using value_type = T;
    WordStorePsramAllocator() noexcept = default;
    template <typename U>
    WordStorePsramAllocator(const WordStorePsramAllocator<U>&) noexcept {}
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
bool operator==(const WordStorePsramAllocator<A>&, const WordStorePsramAllocator<B>&) noexcept
{
    return true;
}

template <typename A, typename B>
bool operator!=(const WordStorePsramAllocator<A>&, const WordStorePsramAllocator<B>&) noexcept
{
    return false;
}

struct StoredWordDeckId {
    char value[37] = {};
};

struct StoredWordPackSnapshot {
    char deck_id[37] = {};
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    char sha256[65] = {};
};

struct StoredWordSessionItem {
    char item_id[37] = {};
    char deck_id[37] = {};
    uint64_t ordinal = 0;
};

struct StoredWordSessionData {
    std::string session_id;
    protocol::word_study_v1::Mode mode =
        protocol::word_study_v1::Mode::kSequential;
    protocol::word_study_v1::Purpose purpose =
        protocol::word_study_v1::Purpose::kStudy;
    protocol::word_study_v1::Ordering ordering =
        protocol::word_study_v1::Ordering::kSequential;
    std::string seed;
    bool include_mastered = false;
    int optional_count = 500;
    uint64_t next_sequence = 0;
    uint64_t progress_revision = 0;
    std::vector<StoredWordDeckId, WordStorePsramAllocator<StoredWordDeckId>> deck_ids;
    std::vector<StoredWordPackSnapshot, WordStorePsramAllocator<StoredWordPackSnapshot>> snapshot;
    std::vector<StoredWordSessionItem, WordStorePsramAllocator<StoredWordSessionItem>> items;
    std::string cursor;
    bool has_more = false;
};

// The server-issued session plus the exact local cursor/phase that may be
// resumed after reset. Content revisions in remote.snapshot remain pinned for
// the lifetime of this record.
struct PersistedWordSession {
    bool active = false;
    bool paused = false;
    uint32_t position = 0;
    WordPresentationPhase phase = WordPresentationPhase::kFront;
    // [deck-scope] Generation of the default-deck scope this session was built
    // under. Stamped automatically on save (from GetDeckScopeGeneration) and
    // validated on load: a mismatch means the deck scope changed after this
    // snapshot was written, so the load reports NOT_FOUND instead of resuming
    // a session pinned to the old scope (second line of defense behind the
    // recoverable-marker protocol in storage.cpp).
    uint32_t deck_scope_generation = 0;
    StoredWordSessionData remote;
};

esp_err_t CompactWordSessionData(
    const protocol::word_study_v1::SessionData& source,
    StoredWordSessionData* destination);

struct DurableWordObservation {
    std::string request_id;
    std::string session_id;
    uint64_t sequence = 0;
    std::string item_id;
    protocol::word_study_v1::ObservationAction action =
        protocol::word_study_v1::ObservationAction::kShown;
    protocol::word_study_v1::Mode mode =
        protocol::word_study_v1::Mode::kSequential;
    std::string occurred_at;
    // Absolute candidate ordinal.  The on-disk field kept its original name
    // so v1 outbox records remain readable after the local window rolls.
    uint32_t next_position = 0;
    WordPresentationPhase next_phase = WordPresentationPhase::kFront;
};

struct WordOutboxSnapshot {
    size_t pending_count = 0;
    // Records parked by SuspendPendingWordObservation: excluded from the
    // upload queue but still resident on device awaiting intervention.
    size_t suspended_count = 0;
    size_t capacity = 0;
};

inline constexpr size_t kWordObservationOutboxCapacity = 1000;

// Sequential, random, and dictionary sessions have independent durable
// slots. This is part of the product contract: changing entry mode must not
// destroy the user's paused session in another mode.
esp_err_t LoadPersistedWordSession(
    protocol::word_study_v1::Mode mode,
    PersistedWordSession* session);
esp_err_t SavePersistedWordSession(const PersistedWordSession& session);
// Persists only the mutable paused flag for an existing session (a tiny write),
// instead of rewriting the full candidate snapshot. Use for pause/resume; the
// value is overlaid on load after outbox reconciliation.
esp_err_t SaveWordSessionCursor(const PersistedWordSession& session);
esp_err_t ClearPersistedWordSession(protocol::word_study_v1::Mode mode);

// Commits the observation first, then the advanced session cursor. Retrying
// the same request_id is idempotent. If the second write is interrupted, load
// reconciliation advances the session from the durable outbox record.
esp_err_t CommitWordObservation(
    const DurableWordObservation& observation,
    const PersistedWordSession& advanced_session);
esp_err_t PeekPendingWordObservation(DurableWordObservation* observation);
esp_err_t AcknowledgeWordObservation(const std::string& request_id);
// Moves one permanently rejected observation to the bounded forensic journal
// before removing it from the upload queue. Other sessions and observations
// remain available and no restart is required.
esp_err_t QuarantinePendingWordObservation(const std::string& request_id);
// Parks one observation whose server-side disposition forbids unilateral
// deletion (idempotency conflict, actor ownership conflict, corrupt
// identity, protocol block). The head is durably marked and skipped by
// PeekPendingWordObservation so the queue keeps advancing, but the payload
// stays recoverable on device pending human intervention.
esp_err_t SuspendPendingWordObservation(
    const std::string& request_id,
    OutboxSuspendReason reason);
esp_err_t ReadWordOutboxSnapshot(WordOutboxSnapshot* snapshot);
esp_err_t PrepareWordObservationOutboxForSleep(int64_t deadline_us);

}  // namespace wqn
