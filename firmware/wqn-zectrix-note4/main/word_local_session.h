#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "device_protocol/word_study.h"
#include "esp_err.h"
#include "word_pack.h"
#include "word_study_store.h"

namespace wqn {

// ---------------------------------------------------------------------------
// Local (offline) long-sequence word study: candidate / descriptor foundation.
//
// Product shape (see the stage spec): a cached word library is walked offline in
// a full round of >=3507 words.  The library is split into segments of at most
// 500 candidates; the UI keeps a 96-item sliding window and a local worker
// pages candidates in from a per-segment descriptor.  Every one of the ~200 B
// per-item durable observations is still produced by the normal outbox path --
// this module NEVER compresses a round into a cursor-only fact.
//
// Ownership / wiring contract (UI worker + sync worker):
//   * The functions below are PURE (no NVS/SPIFFS/HTTP, no StorageService call).
//     They are safe to call on the persist/cloud worker or a scratch task; the
//     UI worker calls the I/O APIs (Save/Load/Prune, declared here) through the persist worker, never device_ui directly.
//   * A descriptor is immutable per segment: it holds ALL <=500 candidates of
//     that segment, pinned to one exact pack snapshot identity.  It is saved
//     BEFORE the current per-mode session snapshot is published (caller does
//     the publish on the worker).
//   * SyncService registers descriptors before uploading facts; this module
//     performs no HTTP.
// ---------------------------------------------------------------------------

// A self-contained description of one offline segment.  `session` is a normal
// PersistedWordSession (kept on the existing v4 disk format, no new fields):
//   * session.remote.items        = the FULL <=500 candidates of this segment,
//                                  ordinal 0..(count-1) LOCAL to the segment
//                                  (NOT absolute scope ordinals -- see below),
//                                  position == 0, has_more == false.
//   * session.start_index         = ABSOLUTE index of this segment's first word
//                                  inside the whole selected scope (0-based).
//   * session.remote.deck_ids     = the actual selected scope deck set (<=32,
//                                  include_mastered == true), kept in CANONICAL
//                                  TRAVERSAL ORDER (deck_order / min sort_index
//                                  of that deck's words), NOT UUID-lexical order.
//                                  LoadWordPackIndexForSession rebuilds the
//                                  pinned index FROM this array, so it must
//                                  reproduce the exact cross-deck order the
//                                  candidates were cut in; otherwise the next
//                                  segment applies its numeric cursor to a
//                                  different deck order and skips/repeats words
//                                  at a 500 boundary even when the SHA set is
//                                  identical.
//   * session.remote.snapshot     = full pin (deck_id / content_revision /
//                                  pack_revision / sha256) for exactly the
//                                  scope decks, in the SAME traversal order as
//                                  deck_ids.  deck_id is a validated 36-char
//                                  UUID, sha256 is 64 lowercase hex, revisions
//                                  are bounded/positive.
//   * session.remote.session_id   = stable 36-char UUID (segment identity).
//   * session.remote.seed, ordering, mode, candidate policy pinned.
// `total_count` = total word count of the whole selected scope, NOT the segment
// count.  It lets WordSequenceAbsoluteCursor and LocalSequenceSnapshotMatches
// bound the global cursor and lets the caller know when the round is complete.
struct LocalWordSessionDescriptor {
    PersistedWordSession session;
    uint32_t total_count = 0;
};

// Personal progress is separate from shared content packs. A complete bitmap
// follows UUID order, authenticated by the item-set digest and exact pack pins.
// A round gets an immutable copy before its first descriptor is published.
struct LocalWordEligibility {
    std::string round_id;  // empty = latest reusable snapshot
    std::string scope_deck_id;
    std::string actor_binding;  // SHA256 of the pairing token; never the token
    uint32_t deck_scope_generation = 0;
    uint64_t progress_revision = 0;
    std::string item_ids_sha256;
    std::string mastered_bitmap;
    std::vector<StoredWordPackSnapshot, WordStorePsramAllocator<StoredWordPackSnapshot>> snapshot;
};

std::string WordLocalSha256(const std::string& bytes);
bool LocalWordEligibilityMatches(const WordPackIndex& index,
    const LocalWordEligibility& eligibility, const std::string& scope);
esp_err_t SaveLocalWordEligibility(const LocalWordEligibility& eligibility);
esp_err_t LoadLocalWordEligibility(const std::string& round_id, LocalWordEligibility* out);

// Ordinal convention (LOAD-BEARING -- do not "fix" by adding start_index twice):
//   * descriptor.session.remote.items[i].ordinal == i, i in
//     [0, min(500, total_count - start_index)), i.e. 0-based and DENSE within
//     the segment.  This is exactly the continuous-ordinal scheme that
//     ExtendPersistedWordSessionWithPage expects (page items continue at
//     items.back().ordinal + 1), so a 96-window can roll without renumbering.
//   * the ABSOLUTE scope index of the item at window position `position` is
//         start_index + items[position].ordinal
//     and the "one past end" absolute index is
//         start_index + (items.empty() ? 0 : items.back().ordinal + 1).
//     start_index is added ONCE; the item ordinal is already segment-local.
//   WordSequenceAbsoluteCursor() implements exactly that, with overflow guards,
//   and deliberately does NOT use `position` as a global index (the 96-window
//   position resets to 0 after a roll and would otherwise go backwards).

// Strict 36-char UUID validator (8-4-4-4-12 hex, lowercase/uppercase hex, no
// path separators).  Shared with the descriptor storage layer for
// filename-token derivation and path-injection defence.
bool IsValidLocalSessionUuid(const std::string& value);

// Prepare one offline segment descriptor purely in memory.
//
// `index`      : a fully validated WordPackIndex (mounted, has_manifest, NOT
//                truncated, NOT pack_error, with usable pins for every scope
//                deck).  Any of those failures is rejected; no half-built
//                descriptor is produced.
// `deck_id`    : ""            -> scope = ALL distinct decks present in the
//                               index that have a usable pin;
//                36-char UUID  -> scope = that single deck (must be present
//                               with a usable pin).
// `start_index`: absolute scope index of the first word of this segment.
// `session_uuid`: stable 36-char UUID identifying this segment.
// `seed`       : pinned into session.remote.seed (1..64 url-safe chars, EMPTY
//                REJECTED to match the server min-1 policy); for the local
//                sequential walk the ordering is deck-order based, but the seed
//                is still pinned so pages match the online policy.
// `generation` : deck_scope_generation stamped on the session; the SAVE path
//                validates it is not stale.  No I/O here.
//
// Ordering (deterministic for ANY input order of index.entries, ordered or
// shuffled): sort by (deck_order, sort_index, normalized lower-case word,
// word_id) -- i.e. the same DeckOrderLess comparator used by the online
// sequential ordering.  Then slice [start_index, start_index+500).
//
// Errors:
//   ESP_ERR_INVALID_ARG       : null out, invalid session_uuid / deck_id /
//                               seed, bad index flags, missing pin.
//   ESP_ERR_INVALID_SIZE      : scope deck set > kMaxDecks (32) -- NEVER
//                               silently truncated to 32; or empty scope.
//   ESP_ERR_INVALID_RESPONSE  : start_index beyond scope (use NOT_FOUND for
//                               start_index == total_count, see below).
//   ESP_ERR_NOT_FOUND         : start_index == total_count -> full round
//                               complete; NO wrap-around to 0.
esp_err_t PrepareLocalWordSequence(
    const WordPackIndex& index,
    const std::string& deck_id,
    uint32_t start_index,
    const std::string& session_uuid,
    const std::string& seed,
    uint32_t generation,
    LocalWordSessionDescriptor* out,
    protocol::word_study_v1::Mode mode = protocol::word_study_v1::Mode::kSequential,
    const LocalWordEligibility* eligibility = nullptr);

// Pure: build the initial UI session from an immutable descriptor.  Keeps only
// the first `max_initial` candidates (<=96, the UI window cap).  `max_initial`
// 0 means use the default 32; values >96 are rejected.  The result's
// cursor/has_more are set for local paging:
//   * remote.cursor = decimal string of the next LOCAL ordinal to fetch
//     (== number of items already handed to the UI window),
//   * remote.has_more = (kept < segment_count): there are more local pages
//     WITHIN this <=500 segment.  It is deliberately NOT OR-ed with "next
//     segment exists" -- at cursor == 500 the UI page fetch must complete this
//     segment (NOT_FOUND) and the worker chain to the next segment, instead of
//     asking this descriptor for a page that does not exist.
// start_index, snapshot, seed, ordering, session_id, pin are inherited exactly.
esp_err_t BuildLocalWordInitialSession(
    const LocalWordSessionDescriptor& descriptor,
    uint32_t max_initial,
    PersistedWordSession* out);

// Pure: produce one CandidatePageData from the descriptor for local paging.
// `cursor` is the session's current remote.cursor (decimal next-local-ordinal).
// Returns items [offset, offset+limit) with the descriptor's own (segment-local)
// ordinals, snapshot/ordering/seed/candidate_policy_version/progress_revision
// matching the session so the result can be fed straight into
// ExtendPersistedWordSessionWithPage (which trims the answered 96-window and
// appends these items).  limit <= 100 (kMaxCandidatePageItems).
// page.cursor == requested cursor (Extend requires it to equal remote.cursor);
// page.next_cursor advances; page.has_more reflects remaining segment items.
// Errors: INVALID_ARG (null/limit>100/bad cursor), INVALID_STATE (cursor past
// segment), NOT_FOUND (no more items for this segment).
esp_err_t GetLocalWordCandidatePage(
    const LocalWordSessionDescriptor& descriptor,
    const std::string& cursor,
    uint32_t limit,
    protocol::word_study_v1::CandidatePageData* out);

// Pure: absolute, monotonic scope cursor for the current session window.
// Returns start_index + current item.ordinal, or (at the window end)
// start_index + last ordinal + 1.  Guarantees:
//   * never uses `position` as a global index (the 96-window position rolls),
//   * rejects position > items.size(), ordinal > UINT32_MAX-ish overflow, and
//     start_index + ordinal overflow (ESP_ERR_INVALID_STATE / INVALID_SIZE),
//   * returns the value in [start_index, start_index + segment_count].
esp_err_t WordSequenceAbsoluteCursor(
    const PersistedWordSession& session,
    uint64_t* absolute);

// Pure: compare the CURRENT valid index pack-snapshot identity AND traversal
// order against the descriptor's pin, so a caller cannot reuse a numeric
// absolute cursor across a changed pack or a reordered pinned index rebuild.
// Returns true only when BOTH hold:
//   * every pinned deck's (content_revision, pack_revision, sha256) matches a
//     usable pin in the current index, AND
//   * re-deriving the canonical cross-deck traversal order from the current
//     index entries (deck_order / min sort_index / word / word_id) yields the
//     SAME deck sequence and per-deck word counts as the pinned descriptor.
// A same-size / same-SHA reorder is a mismatch (prevents 500-boundary skips).
// A worker may keep walking using the already-pinned index, or restart the
// round from absolute 0 on mismatch.
bool LocalSequenceSnapshotMatches(
    const WordPackIndex& index,
    const LocalWordSessionDescriptor& descriptor);

// ----- I/O APIs (implemented in word_study_store.cpp) -----
// These are the only durable entry points; the UI worker reaches them through
// the persist worker / StorageService, never directly from device_ui.
struct LocalDescriptorRetention {
    // Read-only collection of the pack sha256 stems that must be kept by
    // word_pack's prune, so an old pinned segment's pack is not deleted while
    // its observations still await upload.
    std::vector<std::string> pinned_sha256;
};

esp_err_t SaveLocalWordSessionDescriptor(const LocalWordSessionDescriptor& descriptor);
esp_err_t LoadLocalWordSessionDescriptor(
    const std::string& session_id,
    LocalWordSessionDescriptor* out);
esp_err_t PruneLocalWordSessionDescriptors(
    const std::string& active_session_id);
esp_err_t CollectRetainedLocalSnapshots(LocalDescriptorRetention* out);

}  // namespace wqn
