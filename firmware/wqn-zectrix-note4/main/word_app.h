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
    kDictionaryPicker,
    kWordCard,
};

enum class WordCardPhase : uint8_t {
    kFront,
    kRevealed,
    kPersisting,
};

enum class WordCardSource : uint8_t {
    kStudy,
    kDictionary,
};

enum class WordDictionaryStage : uint8_t {
    kLetters,
    kLookupChoice,
};

enum class WordHomeSelection : uint8_t {
    kSequential,
    kRandom,
    kDictionary,
};

enum class WordLookupSelection : uint8_t {
    kOnlineSearch,
    kAiLookup,
};

enum class WordObservationCommitState : uint8_t {
    kIdle,
    kPersisting,
    kLocalCommitted,
    kCloudPending,
    kCloudAcknowledged,
    kFailed,
};

struct WordSessionState {
    bool start_requested = false;
    bool start_result_expected = false;
    protocol::word_study_v1::Mode requested_mode =
        protocol::word_study_v1::Mode::kSequential;
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
    WordCardSource card_source = WordCardSource::kStudy;
    WordDictionaryStage dictionary_stage = WordDictionaryStage::kLetters;
    WordHomeSelection home_selection = WordHomeSelection::kSequential;
    WordLookupSelection lookup_selection = WordLookupSelection::kOnlineSearch;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;

    WordSessionState session;
    WordOutboxState outbox;

    WordPackIndex pack_index;
    // A cloud refresh never mutates the content snapshot used by an active
    // review or dictionary session. It becomes current only after returning
    // to the word home screen.
    WordPackIndex pending_pack_index;
    bool pending_pack_index_ready = false;
    WqnWordEntry current_word;

    std::string dictionary_prefix;
    std::vector<char> dictionary_letters;
    size_t dictionary_letter_selected = 0;
    std::vector<size_t> dictionary_matches;
    size_t dictionary_match_selected = 0;

    std::vector<WqnWordEntry> online_results;
    size_t online_result_selected = 0;
    WqnWordEntry lookup_word;

    bool cloud_sync_requested = false;
    bool cloud_sync_failed = false;
    bool cloud_loaded_once = false;
    bool search_pending = false;
    bool ai_lookup_pending = false;
    bool lookup_result_expected = false;
    std::string pending_search_query;
    std::string pending_ai_query;
    std::string active_lookup_query;
    bool sequential_session_resumable = false;
    bool random_session_resumable = false;
    std::string message;

    // Study scope: the NVS-backed default deck (empty = all decks) and the
    // transient override set when a [词] row on the note screen opens a
    // specific deck. scoped wins over default; dictionary stays cross-deck.
    std::string default_deck_id;
    std::string default_deck_title;
    std::string scoped_deck_id;
    std::string scoped_deck_title;
    std::vector<WordDeckInfo> deck_catalog;
};

struct WordAppSnapshot {
    WordAppMode mode = WordAppMode::kHome;
    WordCardPhase card_phase = WordCardPhase::kFront;
    WordCardSource card_source = WordCardSource::kStudy;
    WordDictionaryStage dictionary_stage = WordDictionaryStage::kLetters;
    WordHomeSelection home_selection = WordHomeSelection::kSequential;
    WordLookupSelection lookup_selection = WordLookupSelection::kOnlineSearch;
    bool has_card = false;
    bool finished_today = false;
    bool pack_ready = false;
    bool pack_truncated = false;
    bool cloud_sync_failed = false;
    bool sequential_session_resumable = false;
    bool random_session_resumable = false;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;
    uint16_t total_count = 0;
    uint16_t card_position = 0;
    uint16_t card_count = 0;
    size_t pack_count = 0;
    size_t pack_bytes = 0;
    std::string word;
    std::string phonetic;
    std::string meaning;
    std::string example;
    std::string example_translation;
    std::string part_of_speech;
    std::string dictionary_prefix;
    std::vector<char> dictionary_letters;
    size_t dictionary_letter_selected = 0;
    std::vector<std::string> dictionary_preview_words;
    size_t dictionary_match_selected = 0;
    std::vector<std::string> online_words;
    size_t online_result_selected = 0;
    std::string progress_line;
    std::string status_line;
    std::string hint;
};

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
// to the word home; clear_persisted also drops the durable
// sequential/random records (the dictionary session is cross-deck and
// survives).
void ResetWordSessionsForScopeChange(WordAppState* state, bool clear_persisted);
bool ApplyWordSearchResult(
    WordAppState* state,
    const std::string& query,
    const WqnWordSearchResult& result);
bool ApplyWordAiLookupResult(
    WordAppState* state,
    const std::string& query,
    const WqnWordAiLookupResult& result);
bool ApplyWordLookupFailure(
    WordAppState* state,
    const std::string& query,
    const std::string& message);
void CancelWordLookupResult(WordAppState* state);
bool TakeWordSearchRequest(WordAppState* state, WqnWordSearchRequest* request);
bool TakeWordAiLookupRequest(WordAppState* state, WqnWordAiLookupRequest* request);
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
