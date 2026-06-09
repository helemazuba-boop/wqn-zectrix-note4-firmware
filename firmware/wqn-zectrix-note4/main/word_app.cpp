#include "word_app.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <string>

#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "wqn_word";
constexpr uint16_t kDefaultDailyTarget = 20;
constexpr size_t kDictionaryPreviewLimit = 8;

size_t SelectionIndex(wqn::WordHomeSelection selection)
{
    return static_cast<size_t>(selection);
}

wqn::WordHomeSelection HomeSelectionFromIndex(size_t index)
{
    switch (index % 3) {
        case 0:
            return wqn::WordHomeSelection::kSequential;
        case 1:
            return wqn::WordHomeSelection::kRandom;
        default:
            return wqn::WordHomeSelection::kDictionary;
    }
}

std::string HomeSelectionLabel(wqn::WordHomeSelection selection)
{
    switch (selection) {
        case wqn::WordHomeSelection::kSequential:
            return "顺序复习";
        case wqn::WordHomeSelection::kRandom:
            return "随机复习";
        case wqn::WordHomeSelection::kDictionary:
            return "词典";
    }
    return "顺序复习";
}

bool HasPackWords(const wqn::WordAppState& state)
{
    return !state.pack_index.entries.empty();
}

void RefreshDictionaryState(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return;
    }
    state->dictionary_letters = wqn::WordPackNextLetters(state->pack_index, state->dictionary_prefix);
    if (state->dictionary_letter_selected >= state->dictionary_letters.size()) {
        state->dictionary_letter_selected = 0;
    }
    wqn::FindWordPackPrefixMatches(state->pack_index, state->dictionary_prefix, kDictionaryPreviewLimit, &state->dictionary_matches);
    if (state->dictionary_match_selected >= state->dictionary_matches.size()) {
        state->dictionary_match_selected = 0;
    }
}

esp_err_t LoadCurrentReviewWord(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    state->current_word = wqn::WqnWordEntry{};
    if (state->review_indices.empty() || state->review_position >= state->review_indices.size()) {
        return ESP_OK;
    }
    const size_t index = state->review_indices[state->review_position];
    if (index >= state->pack_index.entries.size()) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::ReadWordPackEntry(state->pack_index.entries[index], &state->current_word);
}

esp_err_t LoadCurrentDictionaryWord(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    state->current_word = wqn::WqnWordEntry{};
    if (state->dictionary_matches.empty() || state->dictionary_match_selected >= state->dictionary_matches.size()) {
        return ESP_OK;
    }
    const size_t index = state->dictionary_matches[state->dictionary_match_selected];
    if (index >= state->pack_index.entries.size()) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::ReadWordPackEntry(state->pack_index.entries[index], &state->current_word);
}

void BuildReviewQueue(wqn::WordAppState* state, bool random)
{
    if (state == nullptr) {
        return;
    }
    state->review_indices.clear();
    state->review_position = 0;
    state->random_review = random;
    const size_t limit = std::min<size_t>(state->pack_index.entries.size(), state->daily_target);
    state->review_indices.reserve(limit);
    for (size_t i = 0; i < state->pack_index.entries.size() && state->review_indices.size() < limit; ++i) {
        state->review_indices.push_back(i);
    }
    if (random && state->review_indices.size() > 1) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        for (size_t i = state->review_indices.size() - 1; i > 0; --i) {
            const size_t j = static_cast<size_t>(std::rand()) % (i + 1);
            std::swap(state->review_indices[i], state->review_indices[j]);
        }
    }
}

void QueueReviewSubmission(wqn::WordAppState* state, const wqn::WqnWordEntry& word, const char* outcome)
{
    if (state == nullptr || outcome == nullptr || outcome[0] == '\0' || word.id.empty()) {
        return;
    }
    state->pending_submit_word_id = word.id;
    state->pending_submit_outcome = outcome;
    state->pending_submit_word = word.word;
}

void AdvanceReview(wqn::WordAppState* state)
{
    if (state == nullptr || state->review_indices.empty()) {
        return;
    }
    if (state->review_position + 1 < state->review_indices.size()) {
        ++state->review_position;
        ESP_ERROR_CHECK_WITHOUT_ABORT(LoadCurrentReviewWord(state));
        state->mode = wqn::WordAppMode::kReviewFront;
        return;
    }
    state->mode = wqn::WordAppMode::kHome;
    state->review_indices.clear();
    state->review_position = 0;
    state->current_word = wqn::WqnWordEntry{};
    state->message = "本轮复习完成";
}

void RequestOnlineLookup(wqn::WordAppState* state)
{
    if (state == nullptr || state->dictionary_prefix.empty()) {
        return;
    }
    state->pending_search_query = state->dictionary_prefix;
    state->search_pending = true;
    state->message = "正在在线搜索";
}

void RequestAiLookup(wqn::WordAppState* state)
{
    if (state == nullptr || state->dictionary_prefix.empty()) {
        return;
    }
    state->pending_ai_query = state->dictionary_prefix;
    state->ai_lookup_pending = true;
    state->message = "正在询问 AI";
}

void MarkCurrentAsUnknown(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return;
    }
    QueueReviewSubmission(state, state->current_word, "unknown");
    state->message = state->current_word.id.empty() ? "临时词无法加入错词本" : "已标记不认识";
}

uint16_t ClampUint16(size_t value)
{
    return static_cast<uint16_t>(std::min<size_t>(value, UINT16_MAX));
}

}  // namespace

namespace wqn {

esp_err_t InitWordApp(WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->initialized) {
        return ESP_OK;
    }

    state->daily_target = kDefaultDailyTarget;
    state->mode = WordAppMode::kHome;
    state->home_selection = WordHomeSelection::kSequential;
    state->lookup_selection = WordLookupSelection::kOnlineSearch;
    state->message = "词库同步中";

    const esp_err_t storage_result = InitWordPackStorage();
    if (storage_result != ESP_OK) {
        state->message = "词库分区不可用";
    } else {
        WordPackIndex index;
        const esp_err_t index_result = LoadWordPackIndex(&index);
        ApplyWordPackIndex(state, index, index.status_message);
        if (index_result != ESP_OK) {
            ESP_LOGW(kTag, "load local word pack index failed: %s", esp_err_to_name(index_result));
        }
    }

    state->initialized = true;
    state->cloud_sync_requested = !state->cloud_loaded_once || state->pack_index.pack_error;
    return ESP_OK;
}

esp_err_t HandleWordAppInput(WordAppState* state, WordInput input)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->initialized) {
        ESP_RETURN_ON_ERROR(InitWordApp(state), kTag, "init word app");
    }

    switch (state->mode) {
        case WordAppMode::kHome: {
            size_t selected = SelectionIndex(state->home_selection);
            if (input == WordInput::kUp) {
                selected = (selected + 2) % 3;
                state->home_selection = HomeSelectionFromIndex(selected);
                state->message = HomeSelectionLabel(state->home_selection);
                return ESP_OK;
            }
            if (input == WordInput::kDown) {
                selected = (selected + 1) % 3;
                state->home_selection = HomeSelectionFromIndex(selected);
                state->message = HomeSelectionLabel(state->home_selection);
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                state->cloud_sync_requested = true;
                state->message = "词库同步中";
                return ESP_OK;
            }
            if (input != WordInput::kConfirm) {
                return ESP_OK;
            }
            if (state->home_selection == WordHomeSelection::kDictionary) {
                state->mode = WordAppMode::kDictionary;
                state->dictionary_prefix.clear();
                RefreshDictionaryState(state);
                state->message = HasPackWords(*state) ? "选择首字母" : "词库未同步";
                return ESP_OK;
            }
            if (!HasPackWords(*state)) {
                state->message = state->pack_index.status_message.empty() ? "词库未同步" : state->pack_index.status_message;
                state->cloud_sync_requested = true;
                return ESP_OK;
            }
            BuildReviewQueue(state, state->home_selection == WordHomeSelection::kRandom);
            ESP_RETURN_ON_ERROR(LoadCurrentReviewWord(state), kTag, "load current review word");
            state->mode = WordAppMode::kReviewFront;
            state->message = "确认翻面";
            return ESP_OK;
        }

        case WordAppMode::kReviewFront:
            if (input == WordInput::kConfirm) {
                state->mode = WordAppMode::kReviewBack;
                state->message = "确认认识，上键不认识，下键跳过";
            } else if (input == WordInput::kDown) {
                AdvanceReview(state);
                state->message = "已跳过";
            } else if (input == WordInput::kLongConfirm) {
                state->mode = WordAppMode::kHome;
                state->message = "已返回单词主页";
            }
            return ESP_OK;

        case WordAppMode::kReviewBack:
            if (input == WordInput::kConfirm) {
                QueueReviewSubmission(state, state->current_word, "known");
                ++state->reviewed_today;
                ++state->correct_today;
                state->message = "已记录认识";
                AdvanceReview(state);
            } else if (input == WordInput::kUp) {
                QueueReviewSubmission(state, state->current_word, "unknown");
                ++state->reviewed_today;
                state->message = "已加入遗忘的单词";
                AdvanceReview(state);
            } else if (input == WordInput::kDown) {
                AdvanceReview(state);
                state->message = "已跳过";
            } else if (input == WordInput::kLongConfirm) {
                state->mode = WordAppMode::kHome;
                state->message = "已返回单词主页";
            }
            return ESP_OK;

        case WordAppMode::kDictionary:
            if (input == WordInput::kLongConfirm) {
                if (!state->dictionary_prefix.empty()) {
                    state->dictionary_prefix.pop_back();
                    RefreshDictionaryState(state);
                    state->message = state->dictionary_prefix.empty() ? "选择首字母" : state->dictionary_prefix;
                } else {
                    state->mode = WordAppMode::kHome;
                    state->message = "已返回单词主页";
                }
                return ESP_OK;
            }
            if (input == WordInput::kUp) {
                if (!state->dictionary_letters.empty()) {
                    state->dictionary_letter_selected =
                        (state->dictionary_letter_selected + state->dictionary_letters.size() - 1) % state->dictionary_letters.size();
                }
                return ESP_OK;
            }
            if (input == WordInput::kDown) {
                if (!state->dictionary_letters.empty()) {
                    state->dictionary_letter_selected = (state->dictionary_letter_selected + 1) % state->dictionary_letters.size();
                }
                return ESP_OK;
            }
            if (input != WordInput::kConfirm) {
                return ESP_OK;
            }
            if (!state->dictionary_letters.empty()) {
                state->dictionary_prefix.push_back(state->dictionary_letters[state->dictionary_letter_selected]);
                RefreshDictionaryState(state);
                if (state->dictionary_matches.size() == 1 && state->dictionary_letters.empty()) {
                    ESP_RETURN_ON_ERROR(LoadCurrentDictionaryWord(state), kTag, "load dictionary detail");
                    state->mode = WordAppMode::kDictionaryDetail;
                    state->message = "上键标记不认识";
                } else if (state->dictionary_matches.empty()) {
                    state->mode = WordAppMode::kLookupChoice;
                    state->lookup_selection = WordLookupSelection::kOnlineSearch;
                    state->message = "本地未命中";
                } else {
                    state->message = state->dictionary_prefix;
                }
                return ESP_OK;
            }
            if (!state->dictionary_matches.empty()) {
                ESP_RETURN_ON_ERROR(LoadCurrentDictionaryWord(state), kTag, "load dictionary detail");
                state->mode = WordAppMode::kDictionaryDetail;
                state->message = "上键标记不认识";
                return ESP_OK;
            }
            state->mode = WordAppMode::kLookupChoice;
            state->lookup_selection = WordLookupSelection::kOnlineSearch;
            state->message = "本地未命中";
            return ESP_OK;

        case WordAppMode::kDictionaryDetail:
            if (input == WordInput::kUp) {
                MarkCurrentAsUnknown(state);
            } else if (input == WordInput::kConfirm || input == WordInput::kLongConfirm) {
                state->mode = WordAppMode::kDictionary;
                state->message = state->dictionary_prefix.empty() ? "选择首字母" : state->dictionary_prefix;
            }
            return ESP_OK;

        case WordAppMode::kLookupChoice:
            if (input == WordInput::kUp || input == WordInput::kDown) {
                state->lookup_selection = state->lookup_selection == WordLookupSelection::kOnlineSearch ? WordLookupSelection::kAiLookup
                                                                                                      : WordLookupSelection::kOnlineSearch;
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                state->mode = WordAppMode::kDictionary;
                state->message = state->dictionary_prefix.empty() ? "选择首字母" : state->dictionary_prefix;
                return ESP_OK;
            }
            if (input == WordInput::kConfirm) {
                if (state->lookup_selection == WordLookupSelection::kOnlineSearch) {
                    RequestOnlineLookup(state);
                } else {
                    RequestAiLookup(state);
                }
            }
            return ESP_OK;

        case WordAppMode::kLookupResult:
            if (input == WordInput::kUp) {
                MarkCurrentAsUnknown(state);
            } else if (input == WordInput::kConfirm || input == WordInput::kLongConfirm) {
                state->mode = WordAppMode::kDictionary;
                state->message = state->dictionary_prefix.empty() ? "选择首字母" : state->dictionary_prefix;
            }
            return ESP_OK;
    }

    return ESP_OK;
}

void ApplyWordPackIndex(WordAppState* state, const WordPackIndex& index, const std::string& message)
{
    if (state == nullptr) {
        return;
    }
    state->pack_index = index;
    state->cloud_loaded_once = index.has_manifest;
    state->cloud_sync_failed = index.pack_error;
    state->cloud_sync_requested = !index.has_manifest || index.pack_error;
    state->message = !message.empty() ? message : index.status_message;
    if (state->message.empty()) {
        state->message = HasPackWords(*state) ? "词库已就绪" : "词库未同步";
    }
    RefreshDictionaryState(state);
}

void ApplyWordSearchResult(WordAppState* state, const WqnWordSearchResult& result)
{
    if (state == nullptr) {
        return;
    }
    state->online_results = result.words;
    state->online_result_selected = 0;
    if (!state->online_results.empty()) {
        state->current_word = state->online_results.front();
        state->mode = WordAppMode::kLookupResult;
        state->message = "在线搜索结果";
    } else {
        state->mode = WordAppMode::kLookupChoice;
        state->lookup_selection = WordLookupSelection::kAiLookup;
        state->message = "未找到，确认询问 AI";
    }
}

void ApplyWordAiLookupResult(WordAppState* state, const WqnWordAiLookupResult& result)
{
    if (state == nullptr) {
        return;
    }
    state->lookup_word = result.word;
    state->current_word = result.word;
    state->mode = WordAppMode::kLookupResult;
    state->message = "AI 临时释义";
}

bool TakeWordSearchRequest(WordAppState* state, WqnWordSearchRequest* request)
{
    if (state == nullptr || request == nullptr || !state->search_pending) {
        return false;
    }
    request->query = state->pending_search_query;
    request->limit = 8;
    state->search_pending = false;
    state->pending_search_query.clear();
    return true;
}

bool TakeWordAiLookupRequest(WordAppState* state, WqnWordAiLookupRequest* request)
{
    if (state == nullptr || request == nullptr || !state->ai_lookup_pending) {
        return false;
    }
    request->query = state->pending_ai_query;
    state->ai_lookup_pending = false;
    state->pending_ai_query.clear();
    return true;
}

bool TakeWordReviewSubmission(WordAppState* state, WqnWordReviewSubmission* submission, std::string* word)
{
    if (state == nullptr || submission == nullptr) {
        return false;
    }
    if (state->pending_submit_word_id.empty() || state->pending_submit_outcome.empty()) {
        return false;
    }
    submission->word_id = state->pending_submit_word_id;
    submission->outcome = state->pending_submit_outcome;
    submission->mode = state->random_review ? "random" : "sequential";
    if (word != nullptr) {
        *word = state->pending_submit_word;
    }
    state->pending_submit_word_id.clear();
    state->pending_submit_outcome.clear();
    state->pending_submit_word.clear();
    return true;
}

WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state)
{
    WordAppSnapshot snapshot;
    snapshot.mode = state.mode;
    snapshot.home_selection = state.home_selection;
    snapshot.lookup_selection = state.lookup_selection;
    snapshot.pack_ready = HasPackWords(state);
    snapshot.pack_truncated = state.pack_index.truncated;
    snapshot.cloud_sync_failed = state.cloud_sync_failed;
    snapshot.reviewed_today = state.reviewed_today;
    snapshot.correct_today = state.correct_today;
    snapshot.daily_target = state.daily_target;
    snapshot.due_count = ClampUint16(state.pack_index.entries.size());
    snapshot.total_count = ClampUint16(state.pack_index.entries.size());
    snapshot.card_count = ClampUint16(state.review_indices.size());
    snapshot.card_position = state.review_indices.empty() ? 0 : ClampUint16(state.review_position + 1);
    snapshot.finished_today = state.review_indices.empty() && (state.mode == WordAppMode::kReviewFront || state.mode == WordAppMode::kReviewBack);
    snapshot.pack_count = state.pack_index.pack_count;
    snapshot.pack_bytes = state.pack_index.pack_bytes;
    snapshot.dictionary_prefix = state.dictionary_prefix;
    snapshot.dictionary_letters = state.dictionary_letters;
    snapshot.dictionary_letter_selected = state.dictionary_letter_selected;
    snapshot.dictionary_match_selected = state.dictionary_match_selected;
    snapshot.online_result_selected = state.online_result_selected;
    snapshot.progress_line = WordAppProgressLabel(state);
    snapshot.status_line = WordAppStatusLine(state);
    snapshot.hint = state.message.empty() ? "确认选择，长按确认返回" : state.message;

    const WqnWordEntry& word = state.current_word;
    if (!word.word.empty()) {
        snapshot.has_card = true;
        snapshot.word = word.word;
        snapshot.phonetic = word.phonetic;
        snapshot.meaning = word.meaning;
        snapshot.example = word.example;
        snapshot.example_translation = word.example_translation;
        snapshot.part_of_speech = word.part_of_speech;
    }

    for (size_t i = 0; i < state.dictionary_matches.size() && i < kDictionaryPreviewLimit; ++i) {
        const size_t index = state.dictionary_matches[i];
        if (index < state.pack_index.entries.size()) {
            snapshot.dictionary_preview_words.push_back(state.pack_index.entries[index].word);
        }
    }
    for (const WqnWordEntry& entry : state.online_results) {
        snapshot.online_words.push_back(entry.word);
    }
    return snapshot;
}

std::string WordAppProgressLabel(const WordAppState& state)
{
    if (state.daily_target == 0) {
        return "--%";
    }
    const int percent = std::min(100, static_cast<int>(state.reviewed_today) * 100 / state.daily_target);
    return std::to_string(percent) + "%";
}

std::string WordAppStatusLine(const WordAppState& state)
{
    if (!state.pack_index.mounted) {
        return "词库分区不可用";
    }
    if (state.cloud_sync_failed) {
        return "词库同步异常";
    }
    if (!HasPackWords(state)) {
        return state.pack_index.status_message.empty() ? "词库未同步" : state.pack_index.status_message;
    }
    return "本地词库 " + std::to_string(state.pack_index.entries.size()) + " 词";
}

std::string WordAppSignature(const WordAppState& state)
{
    std::string signature;
    signature.reserve(180);
    signature.append(std::to_string(static_cast<int>(state.mode)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.home_selection)));
    signature.push_back('/');
    signature.append(std::to_string(state.review_position));
    signature.push_back('/');
    signature.append(std::to_string(state.review_indices.size()));
    signature.push_back('/');
    signature.append(state.current_word.id);
    signature.push_back('/');
    signature.append(state.current_word.word);
    signature.push_back('/');
    signature.append(state.dictionary_prefix);
    signature.push_back('/');
    signature.append(std::to_string(state.dictionary_letter_selected));
    signature.push_back('/');
    signature.append(std::to_string(state.dictionary_match_selected));
    signature.push_back('/');
    signature.append(std::to_string(state.pack_index.entries.size()));
    signature.push_back('/');
    signature.append(state.message);
    return signature;
}

}  // namespace wqn
