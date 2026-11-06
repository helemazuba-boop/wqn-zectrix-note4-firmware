#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "word_pack.h"
#include "word_study_store.h"
#include "wqn_api.h"

#include "word_batch_policy.h"

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
    kCloudPending = 3,
    kCloudAcknowledged,
    kFailed,
    kBuffered,
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
    bool queue_unavailable = false;
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
    // Whole selected-scope denominator and pinned pack identity for one local
    // round. A segment completing at 500 is NOT the true end unless this total
    // is reached. Empty identity means no local descriptor has been pinned.
    uint32_t sequential_total = 0;
    std::string pinned_session_id;
    std::string pinned_deck_id;
    uint32_t pinned_scope_generation = 0;
    std::vector<std::string> exclude_ids;
    std::vector<uint64_t> skip_ordinals;
    // A fully observed segment is armed for the next one only after the RAM
    // buffer drained and its final observation batch ACKed.
    bool segment_finished = false;
    bool next_segment_armed = false;
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
    // Stable per-start UUID and seed for local descriptor identity. Rejected
    // enqueue retries reuse these values rather than minting a new segment.
    std::string local_session_id;
    std::string local_seed;
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
    struct BufferedObservation {
        DurableWordObservation observation;
        int64_t accepted_ms = 0;
    };
    // UI-owned immutable events, INCLUDING the worker's in-flight prefix.
    // No worker result installs an older cursor over RAM-ahead progress.
    std::vector<BufferedObservation> buffered_observations;
    PersistedWordSession buffered_advanced_session;
    size_t batch_in_flight = 0;
    uint32_t batch_operation_id = 0;
    // [persist-worker] Pause/resume cursor intent. The durable overlay is
    // accepted only on the worker's operation_id-bound ACK; a failed ACK keeps
    // the intent and the original screen for an explicit retry.
    bool cursor_intent_armed = false;
    bool cursor_intent_paused = false;
    bool cursor_intent_home = false;
    uint32_t cursor_intent_operation_id = 0;
    int64_t cursor_retry_after_ms = 0;
    bool cursor_awaiting_retry_confirm = false;
    uint32_t cursor_intent_scope_generation = 0;
    PersistedWordSession cursor_intent_session;
    // Worker-backed resume/load or invalid-slot retirement intent.
    bool session_load_armed = false;
    protocol::word_study_v1::Mode session_load_mode =
        protocol::word_study_v1::Mode::kSequential;
    bool session_clear_armed = false;
    std::string session_clear_id;
    bool cancel_start_requested = false;
    protocol::word_study_v1::Mode session_clear_mode =
        protocol::word_study_v1::Mode::kSequential;
    uint32_t session_worker_operation_id = 0;
    int64_t batch_retry_after_ms = 0;
    esp_err_t batch_error = ESP_OK;
    bool batch_flush_requested = false;
    bool batch_finish_pending = false;
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

struct WordCardPrefetchState {
    uint32_t operation_id = 0;
    uint32_t scope_generation = 0;
    std::string session_id;
    WordPackIndexEntry index{};
    WqnWordEntry entry;
    bool ready = false;
    int64_t retry_after_ms = 0;
};

// [deck-autorepair] Independent, self-contained intent for the automatic
// all-decks repair of a default deck that a fresh authoritative catalog
// reports gone. Kept OUT of SettingsAppState on purpose: a manual switch has
// its own pending/op pair and must never be conflated with (or overwritten
// by) this automatic path. All fields are plain UI-task state; the durable
// half is the SAME kSettingsDefaultDeck persist-worker transaction used by a
// manual Confirm, attributed back by operation_id (never by the manual pair).
struct DefaultDeckRepairState {
    bool pending = false;            // intent armed, not yet durable
    uint32_t op_id = 0;             // in-flight worker operation (0 = none)
    int64_t retry_after_ms = 0;     // bounded backoff deadline (UI ticks)
    uint32_t attempts = 0;          // failed submit/ACK cycles for backoff
    uint32_t scope_generation = 0;  // epoch captured when armed
    // [deck-autorepair] Recovery obligation: set the moment a worker
    // operation is ACCEPTED (op_id attributed). A failed
    // ChangeDefaultWordDeckForeground may already have durably changed the
    // default key / session clears / scope generation before failing, so the
    // recoverable marker tail MUST be retried to completion. This obligation
    // survives a failed ACK, a reappearing/valid catalog, a moved scope epoch
    // and Cancel; only a SUCCESS ACK clears it. While it is set, card /
    // new-session / temp-scope / manual mutations stay gated.
    bool must_complete = false;
};

// Gates supplied by the UI pump so the word_app layer never reaches into
// SettingsAppState or persist-worker internals. All default false/0.
struct DefaultDeckRepairGates {
    bool observations_pending = false;
    bool persisting = false;
    bool manual_switch_pending = false;
    bool scope_reset_pending = false;
    uint32_t current_scope_generation = 0;
    int64_t now_ms = 0;
};

enum class DefaultDeckRepairTick {
    kIdle,    // no intent, or cancel (default reappeared / epoch moved)
    kDefer,   // intent held: gate or backoff not clear, retry later
    kSubmit,  // caller must SubmitDefaultDeckChange("") now
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
    WordSessionChain shuffle_chain;

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
    WordCardPrefetchState card_prefetch;

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
    // [word-scope-reset] In-flight scope switch. The durable half (new scope
    // generation, four session clears, walk cursor) runs on the persist worker;
    // nothing is installed optimistically -- the scope, the screen switch and
    // the in-memory reset all wait for the durable ACK, mirroring the
    // default-deck switch's pending pair. On failure the pending pair stays
    // armed so a retry re-scopes without re-picking the deck.
    std::string scope_reset_pending_deck_id;
    std::string scope_reset_pending_deck_title;
    bool scope_reset_pending_valid = false;
    uint32_t scope_reset_save_op_id = 0;
    std::vector<WordDeckInfo> deck_catalog;
    // [deck-autorepair] Pure signal set by InstallWordDeckCatalog ONLY when a
    // freshly installed AUTHORITATIVE catalog (full manifest success, changed
    // or unchanged, non-empty or authoritative-empty) confirms the configured
    // default deck is gone. It must NOT clear default_deck_id in memory (that
    // would diverge from NVS); it only authorises an all-decks worker repair.
    // A boot/unloaded catalog (authoritative=false) leaves it false, so boot
    // never deletes NVS.
    bool default_deck_missing = false;
    // Manual accepted switch owns the scope through durable result installation,
    // including a failed ACK and the retry gap (independent of worker busy).
    bool default_deck_manual_pending = false;
    // [deck-autorepair] The independent auto-repair intent (see above).
    DefaultDeckRepairState default_deck_repair;

    // [scope-count] The library-size counts shown on the word home and used as
    // the sequential progress denominator must describe the SELECTED deck, not
    // the whole mounted pack (which may hold several decks). The effective scope
    // is scoped_deck_id first, else default_deck_id, else empty = all decks.
    // The count is memoized here and recomputed ONLY when the effective scope or
    // the mounted index changes -- never on every snapshot/progress render.
    // `index_generation` is bumped by InstallWordPackIndex so a same-sized pack
    // content change still invalidates the cache. `mutable` lets the const
    // snapshot builder refresh the memo without widening its signature.
    uint32_t index_generation = 1;
    mutable std::string scoped_count_scope_id;
    mutable size_t scoped_count_index_size = 0;
    mutable uint32_t scoped_count_index_generation = 0;
    mutable size_t scoped_count = 0;
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
    bool save_failed = false;
    bool saving = false;
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
    bool review_queue_unavailable = false;
    uint16_t review_complete_reviewed = 0;
    uint16_t review_complete_unknown = 0;
    uint16_t review_complete_replayed = 0;
    uint32_t sequential_cursor = 0;
    uint32_t sequential_total = 0;
    // Unknown and known-empty are distinct; no pack-size substitution.
    bool review_due_known = false;
    uint16_t review_due_count = 0;
    // Last sync's mistakes-pool size, shown on the mistakes home card. -1
    // means "not known" (no sync yet, or an older server) and renders as the
    // "-- 词"; 0 is a known-empty pool and renders as "0 词".
    int16_t mistake_count = -1;
    size_t pack_count = 0;
    size_t pack_bytes = 0;
    std::string word;
    std::string phonetic;
    std::string meaning;
    std::string example;
    std::string example_translation;
    std::string part_of_speech;
    std::string scope_title;
    std::string mode_label;
    std::string feedback;
    std::string progress_line;
    std::string status_line;
    std::string hint;
};

// [word-due-hint] The control-plane sync knows how many words are due today;
// the word home is where the user looks for it. Safe to call from any task.
void SetWordReviewDueCount(int count);

// [word-mistake-hint] Same contract for the mistakes card: the sync service
// reports the pool size, the home card renders it. -1 clears the hint back to
// "unknown". Safe to call from any task.
void SetWordMistakeCount(int count);

esp_err_t InitWordApp(WordAppState* state);
esp_err_t HandleWordAppInput(WordAppState* state, WordInput input);
void ApplyWordPackIndex(WordAppState* state, WordPackIndex index, const std::string& message);
// In-memory activation of an already-loaded pending index after an ACK.
// No storage or manifest read belongs here.
void ActivateLoadedPendingWordPackIndex(WordAppState* state);
void InstallPreparedWordPackIndex(WordAppState* state, WordPackIndex index);
// Loads the mounted deck catalog (manifest titles + entry counts, deleted
// decks skipped). Safe on any task; a missing manifest yields an empty list.
esp_err_t BuildWordDeckCatalog(std::vector<WordDeckInfo>* catalog);
// Installs a catalog and re-resolves the default deck title. `authoritative`
// is true ONLY for a fresh, confirmed full manifest success delivered by the
// pack-sync runner; it authorises the auto-repair signal. A boot/unloaded
// catalog passes false and can never clear NVS. An authoritative EMPTY catalog
// (all decks deleted) also confirms the default is gone.
void InstallWordDeckCatalog(
    WordAppState* state, std::vector<WordDeckInfo> catalog, bool authoritative);
// Applies a settings change: remembers id/title and clears both when the id
// is empty (全部词库).
void SetDefaultWordDeck(
    WordAppState* state, const std::string& deck_id, const std::string& title);
// [deck-autorepair] Real state machine (not a pure predicate). Arm/cancel the
// independent intent, decide the next tick with full gates + backoff, record a
// submit outcome, and attribute the durable worker ACK. All transitions live
// here so they are host-testable; the UI pump only supplies gates and calls
// SubmitDefaultDeckChange when told to.
bool ArmDefaultDeckRepair(WordAppState* state, uint32_t scope_generation);
void CancelDefaultDeckRepair(WordAppState* state);
DefaultDeckRepairTick TickDefaultDeckRepair(
    WordAppState* state, const DefaultDeckRepairGates& gates);
void OnDefaultDeckRepairSubmitted(
    WordAppState* state, uint32_t op_id, int64_t now_ms);
// Returns true when operation_id belonged to this auto repair (caller then
// performs the in-memory install on ESP_OK). A stale/duplicate/foreign op is
// rejected. Success clears the whole obligation. A FAILED accepted transaction
// may be half-applied (live recoverable marker), so it preserves the
// must_complete recovery obligation + backs off for a retry with a fresh op_id;
// a valid catalog / non-authoritative load never cancels that obligation.
bool OnDefaultDeckRepairAck(
    WordAppState* state, uint32_t operation_id, esp_err_t result, int64_t now_ms);
// True while an auto repair is pending, in flight, or owes a failed accepted
// transaction's marker tail (must_complete): card / new-session / temp-scope /
// manual mutations and result installs must not cross that scope until a
// success ACK. Only an unsubmitted intent yields to a new manual selection.
bool DefaultDeckRepairBlocking(const WordAppState& state);
// Invalidates the study sessions after the deck scope changed (settings
// default switch or a [词]-row entry): the old session is pinned to the old
// scope, so resuming it would keep studying the previous deck set. Returns
// to the word home; clear_persisted also drops the durable session records.
void ResetWordSessionsForScopeChange(WordAppState* state, bool clear_persisted);
// [word-scope-reset] In-memory half only: drops the session/review/chain state
// and switches the card phase. The durable half is the worker's
// kWordSessionReset transaction, and this must not run before its ACK.
void ResetWordSessionsInMemory(WordAppState* state);
bool TakeWordSessionStartRequest(
    WordAppState* state,
    protocol::word_study_v1::CreateSessionRequest* request);
// Re-arm only a rejected enqueue, retaining its request ID and parameters.
// This is not a retry of an accepted/in-flight cloud request.
void RestoreWordSessionStartRequest(WordAppState* state);
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
    PersistedWordSession* snapshot,
    std::string* session_id);
void RestoreWordCandidatePageRequest(WordAppState* state);
// [ui-gates] `runner_snapshot` is the snapshot the runner extended and
// persisted; when the in-memory session has advanced past it the page is merged
// in memory instead and nothing is written here.
void ApplyWordCandidatePageResult(
    WordAppState* state,
    esp_err_t result,
    esp_err_t compact_result,
    esp_err_t persist_result,
    const PersistedWordSession& runner_snapshot,
    protocol::word_study_v1::CandidatePageData page);
bool TakeWordObservationEffect(
    WordAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    DurableWordObservation* observation,
    PersistedWordSession* advanced_session);
void ApplyWordObservationCommitResult(WordAppState* state, esp_err_t result);
// [persist-worker] Typed pause/resume cursor effect. Take is called only after
// reservation; the ACK installs paused/resumable/home. Failure keeps the exact
// intent and identity for the next retry.
bool TakeWordSessionCursorEffect(
    WordAppState* state,
    uint32_t operation_id,
    PersistedWordSession* session);
bool ApplyWordSessionCursorResult(
    WordAppState* state,
    uint32_t operation_id,
    esp_err_t result);
bool HasBufferedWordObservations(const WordAppState& state);
bool BufferWordObservationEffect(WordAppState* state, const std::string& request_id,
    const std::string& occurred_at, int64_t now_ms);
bool TakeWordObservationBatch(WordAppState* state, uint32_t operation_id, int64_t now_ms,
    std::vector<DurableWordObservation>* observations, PersistedWordSession* advanced_session);
bool ApplyWordObservationBatchResult(WordAppState* state, esp_err_t result,
    uint32_t operation_id, int64_t now_ms);
bool SameWordCardPrefetchIndex(const WordPackIndexEntry& left, const WordPackIndexEntry& right);
bool GetWordCardPrefetchEntry(const WordAppState& state, int64_t now_ms, WordPackIndexEntry* entry);
bool TakeWordCardPrefetchEntry(WordAppState* state, uint32_t operation_id, int64_t now_ms,
    WordPackIndexEntry* entry);
bool ApplyWordCardPrefetchResult(WordAppState* state, uint32_t operation_id, esp_err_t result,
    WqnWordEntry entry, int64_t now_ms);
WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state);
std::string WordAppProgressLabel(const WordAppState& state);
std::string WordAppStatusLine(const WordAppState& state);
std::string WordAppSignature(const WordAppState& state);
bool RunWordPageStateSelfTest();

}  // namespace wqn
