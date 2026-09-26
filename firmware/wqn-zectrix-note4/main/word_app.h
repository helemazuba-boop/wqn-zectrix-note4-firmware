#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "word_pack.h"
#include "word_study_store.h"
#include "wqn_api.h"

namespace wqn {

enum class WordInput {
    kUp,
    kDown,
    kConfirm,
    kLongConfirm,
};

enum class WordAppMode : uint8_t {
    kHome,
    kSessionStarting,
    kWordCard,
    // [word-modes-v2] Local completion page of a review session: the due queue
    // and the local replay pool are both empty. Only the review entry has a
    // completion state -- sequential/shuffle/mistakes are open-ended walks.
    kReviewComplete,
};

enum class WordCardPhase : uint8_t {
    kFront,
    kRevealed,
    kPersisting,
};

// How a classification moves the session cursor. kStay re-renders the same
// item (the reveal flip); kAdvance moves on -- which inside a review session
// may be a replay of a word seen earlier.
enum class WordObservationTarget : uint8_t {
    kStay,
    kAdvance,
};

enum class WordHomeSelection : uint8_t {
    kReview,
    kShuffle,
    kMistakes,
};

// Actions on the review completion page.
enum class WordCompleteSelection : uint8_t {
    kSequential,
    kReturn,
};

enum class WordObservationCommitState : uint8_t {
    kIdle,
    kPersisting,
    kLocalCommitted,
    kCloudPending,
    kCloudAcknowledged,
    kFailed,
};

// One word the user failed to recognize during a review session. It is
// re-shown at most kWordReplayMax times, and never sooner than
// kWordReplayMinGap answered cards after its last appearance, so a short
// session never degenerates into drilling the same word.
struct WordReplayEntry {
    uint64_t ordinal = 0;
    uint8_t replays = 0;
    // Eligible once cards_seen reaches this value.
    uint32_t ready_at = 0;
};

// Review-session runtime, in memory only. The FSRS timeline stays
// authoritative: an unrecognized word is already due=now on the server, so
// losing this pool (reboot, scope switch) degrades the local interleaving but
// never the schedule.
struct WordReviewRuntime {
    std::vector<WordReplayEntry> pool;
    uint32_t seed = 0;
    uint32_t cards_seen = 0;
    // The card on screen is a replay of a word seen earlier: answering it
    // returns to the queue position the replay interrupted.
    bool replay_in_flight = false;
    size_t replay_pool_index = 0;
    uint64_t replay_return_ordinal = 0;
    // The card after the current one is a replay chosen while leaving it.
    bool next_is_replay = false;
    size_t next_pool_index = 0;
    uint64_t next_return_ordinal = 0;
    uint16_t reviewed = 0;
    uint16_t unknown = 0;
    uint16_t replayed = 0;
    uint16_t replay_unknown = 0;
    WordCompleteSelection complete_selection = WordCompleteSelection::kSequential;
};

// [word-sequential-chain] 顺序过词库 is today's new words (an intake session)
// followed by the library walk (a sequential session from the saved cursor).
// The device drops the intake words from the walk so they are not shown twice.
struct WordSessionChain {
    bool active = false;
    uint32_t sequential_cursor = 0;
    std::vector<std::string> exclude_ids;
    std::vector<uint64_t> skip_ordinals;
};

struct WordSessionState {
    bool start_requested = false;
    bool start_result_expected = false;
    protocol::word_study_v1::Mode requested_mode =
        protocol::word_study_v1::Mode::kReview;
    // Sequential continuation point (mode=sequential) and daily new-word
    // budget (mode=intake), filled when a start request is armed.
    uint32_t requested_start_index = 0;
    int requested_new_word_limit = 0;
    std::string create_request_id;
    bool page_requested = false;
    bool page_in_flight = false;
    PersistedWordSession persisted;
    WordObservationCommitState commit_state = WordObservationCommitState::kIdle;
    bool observation_effect_ready = false;
    DurableWordObservation pending_observation;
    PersistedWordSession pending_advanced_session;
    // [persist-worker] operation_id of the in-flight persist submit. The worker
    // result is applied only if this still matches: a scope reset
    // (ResetWordSessionsForScopeChange resets the whole struct to 0) or a newer
    // submit invalidates a late result, preventing it from installing a stale
    // or empty advanced session over freshly-reset state.
    uint32_t pending_persist_operation_id = 0;
};

struct WordOutboxState {
    size_t pending_count = 0;
    size_t suspended_count = 0;
    size_t blocked_count = 0;
    size_t capacity = kWordObservationOutboxCapacity;
};

// One mounted deck (manifest title + entry count). The settings dialog and
// the note screen's mixed [词] rows both render from this catalog.
struct WordDeckInfo {
    std::string deck_id;
    std::string title;
    size_t entry_count = 0;
};

struct WordAppState {
    bool initialized = false;
    WordAppMode mode = WordAppMode::kHome;
    WordCardPhase card_phase = WordCardPhase::kFront;
    WordHomeSelection home_selection = WordHomeSelection::kReview;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;

    WordSessionState session;
    WordOutboxState outbox;
    WordReviewRuntime review;
    WordSessionChain chain;

    WordPackIndex pack_index;
    // A cloud refresh never mutates the content snapshot used by an active
    // session. It becomes current only after returning to the word home.
    WordPackIndex pending_pack_index;
    bool pending_pack_index_ready = false;
    // True while `pack_index` is a resumed session's pinned pack snapshot
    // rather than the mounted library index. Only that case needs the library
    // index re-read when the session finishes.
    bool pack_index_pinned = false;
    WqnWordEntry current_word;

    bool cloud_sync_requested = false;
    bool cloud_sync_failed = false;
    bool cloud_loaded_once = false;
    bool review_session_resumable = false;
    bool shuffle_session_resumable = false;
    bool mistakes_session_resumable = false;
    std::string message;

    // Study scope: the NVS-backed default deck (empty = all decks) and the
    // transient override set when a [词] row on the note screen opens a
    // specific deck. scoped wins over default.
    std::string default_deck_id;
    std::string default_deck_title;
    std::string scoped_deck_id;
    std::string scoped_deck_title;
    std::vector<WordDeckInfo> deck_catalog;
};

struct WordAppSnapshot {
    WordAppMode mode = WordAppMode::kHome;
    WordCardPhase card_phase = WordCardPhase::kFront;
    WordObservationCommitState commit_state = WordObservationCommitState::kIdle;
    WordHomeSelection home_selection = WordHomeSelection::kReview;
    WordCompleteSelection complete_selection = WordCompleteSelection::kSequential;
    bool has_card = false;
    bool finished_today = false;
    bool pack_ready = false;
    bool pack_truncated = false;
    bool cloud_sync_failed = false;
    bool review_session_resumable = false;
    bool shuffle_session_resumable = false;
    bool mistakes_session_resumable = false;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;
    uint16_t total_count = 0;
    uint16_t card_position = 0;
    uint16_t card_count = 0;
    // Completion page (kReviewComplete).
    // True only when the queue arrived empty ("今天没有到期的单词"); a queue the
    // user skipped through is finished, not empty.
    bool review_complete_empty = false;
    uint16_t review_complete_reviewed = 0;
    uint16_t review_complete_unknown = 0;
    uint16_t review_complete_replayed = 0;
    uint32_t sequential_cursor = 0;
    uint16_t sequential_total = 0;
    // Last sync's due-word count, shown on the review home card. 0 means "not
    // known" and renders as the pack size instead.
    uint16_t review_due_count = 0;
    size_t pack_count = 0;
    size_t pack_bytes = 0;
    std::string word;
    std::string phonetic;
    std::string meaning;
    std::string example;
    std::string example_translation;
    std::string part_of_speech;
    std::string progress_line;
    std::string status_line;
    std::string hint;
};

// [word-due-hint] The control-plane sync knows how many words are due today;
// the word home is where the user looks for it. Safe to call from any task.
void SetWordReviewDueCount(int count);

esp_err_t InitWordApp(WordAppState* state);
esp_err_t HandleWordAppInput(WordAppState* state, WordInput input);
void ApplyWordPackIndex(WordAppState* state, WordPackIndex index, const std::string& message);
// Loads the mounted deck catalog (manifest titles + entry counts, deleted
// decks skipped). Safe on any task; a missing manifest yields an empty list.
esp_err_t BuildWordDeckCatalog(std::vector<WordDeckInfo>* catalog);
// Installs a catalog and re-resolves the default deck title; a default that
// no longer exists in a non-empty catalog falls back to all decks in memory.
void InstallWordDeckCatalog(WordAppState* state, std::vector<WordDeckInfo> catalog);
// Applies a settings change: remembers id/title and clears both when the id
// is empty (全部词库).
void SetDefaultWordDeck(
    WordAppState* state, const std::string& deck_id, const std::string& title);
// Invalidates the study sessions after the deck scope changed (settings
// default switch or a [词]-row entry): the old session is pinned to the old
// scope, so resuming it would keep studying the previous deck set. Returns
// to the word home; clear_persisted also drops the durable session records.
void ResetWordSessionsForScopeChange(WordAppState* state, bool clear_persisted);
bool TakeWordSessionStartRequest(
    WordAppState* state,
    protocol::word_study_v1::CreateSessionRequest* request);
// The runner thread already compacted (compact_result) and, for active
// sessions, persisted (persist_result) the snapshot; apply only installs it in
// memory so the UI task never runs the snapshot fsync.
bool ApplyWordSessionStartResult(
    WordAppState* state,
    esp_err_t result,
    esp_err_t compact_result,
    esp_err_t persist_result,
    PersistedWordSession persisted);
void CancelWordSessionStartResult(WordAppState* state);
// Discards a server-invalid session (snapshot corrupt / not found / not active)
// so the device stops reusing the same bad session_id, then returns to the word
// home where a fresh session can be created.
void ResetWordSessionForServerInvalid(WordAppState* state);
bool TakeWordCandidatePageRequest(
    WordAppState* state,
    protocol::word_study_v1::CandidatePageRequest* request,
    std::string* session_id);
void RestoreWordCandidatePageRequest(WordAppState* state);
void ApplyWordCandidatePageResult(
    WordAppState* state,
    esp_err_t result,
    protocol::word_study_v1::CandidatePageData page);
bool TakeWordObservationEffect(
    WordAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    DurableWordObservation* observation,
    PersistedWordSession* advanced_session);
void ApplyWordObservationCommitResult(WordAppState* state, esp_err_t result);
void RefreshWordOutboxState(WordAppState* state);
WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state);
std::string WordAppProgressLabel(const WordAppState& state);
std::string WordAppStatusLine(const WordAppState& state);
std::string WordAppSignature(const WordAppState& state);
bool RunWordPageStateSelfTest();

}  // namespace wqn
