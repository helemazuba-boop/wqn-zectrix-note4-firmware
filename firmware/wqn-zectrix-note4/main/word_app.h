#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "word_pack.h"
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
    kReviewFront,
    kReviewBack,
    kDictionary,
    kDictionaryDetail,
    kLookupChoice,
    kLookupResult,
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

struct WordAppState {
    bool initialized = false;
    WordAppMode mode = WordAppMode::kHome;
    WordHomeSelection home_selection = WordHomeSelection::kSequential;
    WordLookupSelection lookup_selection = WordLookupSelection::kOnlineSearch;
    bool random_review = false;

    uint16_t daily_target = 20;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;

    WordPackIndex pack_index;
    // A cloud refresh never mutates the content snapshot used by an active
    // review or dictionary session. It becomes current only after returning
    // to the word home screen.
    WordPackIndex pending_pack_index;
    bool pending_pack_index_ready = false;
    std::vector<size_t> review_indices;
    size_t review_position = 0;
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
    std::string pending_search_query;
    std::string pending_ai_query;
    std::string pending_submit_word_id;
    std::string pending_submit_outcome;
    std::string pending_submit_word;
    std::string message;
};

struct WordAppSnapshot {
    WordAppMode mode = WordAppMode::kHome;
    WordHomeSelection home_selection = WordHomeSelection::kSequential;
    WordLookupSelection lookup_selection = WordLookupSelection::kOnlineSearch;
    bool has_card = false;
    bool finished_today = false;
    bool pack_ready = false;
    bool pack_truncated = false;
    bool cloud_sync_failed = false;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;
    uint16_t daily_target = 20;
    uint16_t due_count = 0;
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
void ApplyWordSearchResult(WordAppState* state, const WqnWordSearchResult& result);
void ApplyWordAiLookupResult(WordAppState* state, const WqnWordAiLookupResult& result);
bool TakeWordSearchRequest(WordAppState* state, WqnWordSearchRequest* request);
bool TakeWordAiLookupRequest(WordAppState* state, WqnWordAiLookupRequest* request);
bool TakeWordReviewSubmission(WordAppState* state, WqnWordReviewSubmission* submission, std::string* word);
WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state);
std::string WordAppProgressLabel(const WordAppState& state);
std::string WordAppStatusLine(const WordAppState& state);
std::string WordAppSignature(const WordAppState& state);

}  // namespace wqn
