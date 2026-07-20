#include "word_app.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "wqn_word";
constexpr size_t kDictionaryPreviewLimit = 8;
constexpr size_t kWordHomeSelectionCount = 3;

size_t SelectionIndex(wqn::WordHomeSelection selection)
{
    return static_cast<size_t>(selection);
}

wqn::WordHomeSelection HomeSelectionFromIndex(size_t index)
{
    switch (index % kWordHomeSelectionCount) {
        case 0:
            return wqn::WordHomeSelection::kSequential;
        case 1:
            return wqn::WordHomeSelection::kRandom;
        default:
            return wqn::WordHomeSelection::kDictionary;
    }
}

[[maybe_unused]] std::string HomeSelectionLabel(wqn::WordHomeSelection selection)
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
    const auto& session = state->session.persisted;
    if (!session.active || session.position >= session.remote.items.size()) {
        return ESP_OK;
    }
    const char* item_id = session.remote.items[session.position].item_id;
    const auto entry = std::find_if(
        state->pack_index.entries.begin(),
        state->pack_index.entries.end(),
        [&](const wqn::WordPackIndexEntry& value) {
            return std::strcmp(item_id, value.word_id) == 0;
        });
    if (entry == state->pack_index.entries.end()) {
        return ESP_ERR_NOT_FOUND;
    }
    return wqn::ReadWordPackEntry(*entry, &state->current_word);
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

void PrepareObservation(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::ObservationAction action,
    uint32_t next_position,
    wqn::WordPresentationPhase next_phase)
{
    if (state == nullptr || !state->session.persisted.active ||
        state->session.persisted.position >=
            state->session.persisted.remote.items.size()) {
        return;
    }
    auto& observation = state->session.pending_observation;
    observation = {};
    observation.session_id = state->session.persisted.remote.session_id;
    observation.sequence = state->session.persisted.remote.next_sequence;
    observation.item_id =
        state->session.persisted.remote.items[state->session.persisted.position].item_id;
    observation.action = action;
    observation.mode = state->session.persisted.remote.mode;
    observation.next_position = next_position;
    observation.next_phase = next_phase;
    state->session.commit_state = wqn::WordObservationCommitState::kPersisting;
    state->session.observation_effect_ready = true;
    state->message = "正在保存";
}

uint16_t ClampUint16(size_t value)
{
    return static_cast<uint16_t>(std::min<size_t>(value, UINT16_MAX));
}

void InstallWordPackIndex(
    wqn::WordAppState* state,
    wqn::WordPackIndex index,
    const std::string& message)
{
    const bool has_manifest = index.has_manifest;
    const bool pack_error = index.pack_error;
    const std::string status_message = index.status_message;
    state->pack_index = std::move(index);
    state->cloud_loaded_once = has_manifest;
    state->cloud_sync_failed = pack_error;
    state->cloud_sync_requested = !has_manifest || pack_error;
    state->message = !message.empty() ? message : status_message;
    if (state->message.empty()) {
        state->message = HasPackWords(*state) ? "词库已就绪" : "词库未同步";
    }
    RefreshDictionaryState(state);
}

void ActivatePendingWordPackIndex(wqn::WordAppState* state)
{
    if (state == nullptr || state->mode != wqn::WordAppMode::kHome ||
        state->session.persisted.active ||
        !state->pending_pack_index_ready) {
        return;
    }
    wqn::WordPackIndex pending = std::move(state->pending_pack_index);
    state->pending_pack_index = {};
    state->pending_pack_index_ready = false;
    InstallWordPackIndex(state, std::move(pending), "词库更新已启用");
}

void FinishOrLoadAdvancedReview(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return;
    }
    auto& session = state->session.persisted;
    if (session.active && session.position < session.remote.items.size()) {
        const esp_err_t load_result = LoadCurrentReviewWord(state);
        if (load_result == ESP_OK) {
            state->mode = session.phase == wqn::WordPresentationPhase::kBack
                ? wqn::WordAppMode::kReviewBack
                : wqn::WordAppMode::kReviewFront;
            return;
        }
        state->message = "会话词包不可用";
    }
    session.active = false;
    session.paused = false;
    state->mode = wqn::WordAppMode::kHome;
    state->current_word = wqn::WqnWordEntry{};
    if (state->pending_pack_index_ready) {
        ActivatePendingWordPackIndex(state);
    } else {
        wqn::WordPackIndex current;
        if (wqn::LoadWordPackIndex(&current) == ESP_OK) {
            InstallWordPackIndex(state, std::move(current), "");
        }
    }
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
    state->message = state->current_word.id.empty()
        ? "临时词无法加入错词本"
        : "请在复习中标记不认识";
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
        const std::string index_message = index.status_message;
        ApplyWordPackIndex(state, std::move(index), index_message);
        if (index_result != ESP_OK) {
            ESP_LOGW(kTag, "load local word pack index failed: %s", esp_err_to_name(index_result));
        }
    }

    WordOutboxSnapshot outbox;
    if (ReadWordOutboxSnapshot(&outbox) == ESP_OK) {
        state->outbox.pending_count = outbox.pending_count;
        state->outbox.capacity = outbox.capacity;
    }
    PersistedWordSession persisted;
    const esp_err_t session_result = LoadPersistedWordSession(&persisted);
    if (session_result == ESP_OK && persisted.active &&
        persisted.position < persisted.remote.items.size()) {
        state->session.persisted = std::move(persisted);
        if (!state->session.persisted.paused && LoadCurrentReviewWord(state) == ESP_OK) {
            state->mode = state->session.persisted.phase == WordPresentationPhase::kBack
                ? WordAppMode::kReviewBack
                : WordAppMode::kReviewFront;
            state->message = "已恢复上次会话";
        } else {
            state->mode = WordAppMode::kHome;
            state->message = "上次会话可继续";
        }
    } else if (session_result != ESP_OK && session_result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "load persisted word session failed: %s", esp_err_to_name(session_result));
        state->message = "会话记录损坏";
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
    ActivatePendingWordPackIndex(state);

    switch (state->mode) {
        case WordAppMode::kHome: {
            size_t selected = SelectionIndex(state->home_selection);
            if (input == WordInput::kUp) {
                selected = (selected + kWordHomeSelectionCount - 1) % kWordHomeSelectionCount;
                state->home_selection = HomeSelectionFromIndex(selected);
                return ESP_OK;
            }
            if (input == WordInput::kDown) {
                selected = (selected + 1) % kWordHomeSelectionCount;
                state->home_selection = HomeSelectionFromIndex(selected);
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                // Long-press confirm on the word home is handled by the UI layer
                // (ui_model.cpp) to exit back to the device home screen. Pack
                // sync is triggered automatically when entering the word page.
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
            const auto requested_mode = state->home_selection == WordHomeSelection::kRandom
                ? protocol::word_study_v1::Mode::kRandom
                : protocol::word_study_v1::Mode::kSequential;
            if (state->session.persisted.active &&
                state->session.persisted.paused &&
                state->session.persisted.remote.mode == requested_mode &&
                state->session.persisted.position <
                    state->session.persisted.remote.items.size()) {
                state->session.persisted.paused = false;
                ESP_RETURN_ON_ERROR(
                    SavePersistedWordSession(state->session.persisted),
                    kTag,
                    "resume word session");
                ESP_RETURN_ON_ERROR(LoadCurrentReviewWord(state), kTag, "load resumed word");
                state->mode = state->session.persisted.phase == WordPresentationPhase::kBack
                    ? WordAppMode::kReviewBack
                    : WordAppMode::kReviewFront;
                state->message = "已继续上次会话";
                return ESP_OK;
            }
            if (state->session.requested_mode != requested_mode) {
                state->session.create_request_id.clear();
            }
            state->session.requested_mode = requested_mode;
            state->session.start_requested = true;
            state->mode = WordAppMode::kSessionStarting;
            state->message = "正在准备本轮单词";
            return ESP_OK;
        }

        case WordAppMode::kSessionStarting:
            if (input == WordInput::kLongConfirm) {
                state->session.start_requested = false;
                state->mode = WordAppMode::kHome;
                state->message = "已取消";
            }
            return ESP_OK;

        case WordAppMode::kReviewFront:
            if (state->session.commit_state == WordObservationCommitState::kFailed) {
                if (input == WordInput::kConfirm) {
                    state->session.commit_state = WordObservationCommitState::kPersisting;
                    state->session.observation_effect_ready = true;
                    state->message = "正在重试保存";
                }
                return ESP_OK;
            }
            if (input == WordInput::kConfirm) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kRevealed,
                    state->session.persisted.position,
                    WordPresentationPhase::kBack);
            } else if (input == WordInput::kDown) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kSkipped,
                    state->session.persisted.position + 1,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kLongConfirm) {
                state->session.persisted.paused = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(SavePersistedWordSession(state->session.persisted));
                state->mode = WordAppMode::kHome;
                state->message = "本轮已暂停";
                ActivatePendingWordPackIndex(state);
            }
            return ESP_OK;

        case WordAppMode::kReviewBack:
            if (state->session.commit_state == WordObservationCommitState::kFailed) {
                if (input == WordInput::kConfirm) {
                    state->session.commit_state = WordObservationCommitState::kPersisting;
                    state->session.observation_effect_ready = true;
                    state->message = "正在重试保存";
                }
                return ESP_OK;
            }
            if (input == WordInput::kConfirm) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kKnown,
                    state->session.persisted.position + 1,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kUp) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kUnknown,
                    state->session.persisted.position + 1,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kDown) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kSkipped,
                    state->session.persisted.position + 1,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kLongConfirm) {
                state->session.persisted.paused = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(SavePersistedWordSession(state->session.persisted));
                state->mode = WordAppMode::kHome;
                state->message = "本轮已暂停";
                ActivatePendingWordPackIndex(state);
            }
            return ESP_OK;

        case WordAppMode::kDictionary:
            if (input == WordInput::kLongConfirm) {
                if (!state->dictionary_prefix.empty()) {
                    state->dictionary_prefix.pop_back();
                    RefreshDictionaryState(state);
                    state->dictionary_letter_selected = 0;
                    state->dictionary_match_selected = 0;
                    state->message = state->dictionary_prefix.empty() ? "选择首字母" : state->dictionary_prefix;
                } else {
                    state->mode = WordAppMode::kHome;
                    state->message = "已返回单词主页";
                    ActivatePendingWordPackIndex(state);
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
                state->dictionary_letter_selected = 0;
                state->dictionary_match_selected = 0;
                if (state->dictionary_matches.size() == 1 && state->dictionary_letters.empty()) {
                    if (LoadCurrentDictionaryWord(state) == ESP_OK) {
                        state->mode = WordAppMode::kDictionaryDetail;
                        state->message = "上键标记不认识";
                    } else {
                        state->message = "词条读取失败";
                    }
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
                if (LoadCurrentDictionaryWord(state) == ESP_OK) {
                    state->mode = WordAppMode::kDictionaryDetail;
                    state->message = "上键标记不认识";
                } else {
                    state->message = "词条读取失败";
                }
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

void ApplyWordPackIndex(WordAppState* state, WordPackIndex index, const std::string& message)
{
    if (state == nullptr) {
        return;
    }
    if (state->mode != WordAppMode::kHome || state->session.persisted.active) {
        state->pending_pack_index = std::move(index);
        state->pending_pack_index_ready = true;
        state->cloud_loaded_once = state->pending_pack_index.has_manifest;
        state->cloud_sync_failed = state->pending_pack_index.pack_error;
        state->cloud_sync_requested = !state->pending_pack_index.has_manifest ||
            state->pending_pack_index.pack_error;
        state->message = state->pending_pack_index.pack_error
            ? "词库更新校验失败"
            : "词库已更新，下轮启用";
        return;
    }
    InstallWordPackIndex(state, std::move(index), message);
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

bool TakeWordSessionStartRequest(
    WordAppState* state,
    protocol::word_study_v1::CreateSessionRequest* request)
{
    if (state == nullptr || request == nullptr || !state->session.start_requested) {
        return false;
    }
    if (state->session.create_request_id.empty()) {
        state->session.create_request_id = request->metadata.request_id;
    } else {
        request->metadata.request_id = state->session.create_request_id;
    }
    request->mode = state->session.requested_mode;
    request->scope = {};
    request->optional_count = 0;
    request->seed.clear();
    state->session.start_requested = false;
    return true;
}

void ApplyWordSessionStartResult(
    WordAppState* state,
    esp_err_t result,
    protocol::word_study_v1::SessionData session)
{
    if (state == nullptr) return;
    if (result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = result == ESP_ERR_INVALID_STATE
            ? "请先完成配对"
            : "本轮准备失败，可重试";
        return;
    }
    PersistedWordSession persisted;
    persisted.active = !session.items.empty();
    persisted.paused = false;
    persisted.position = 0;
    persisted.phase = WordPresentationPhase::kFront;
    result = CompactWordSessionData(session, &persisted.remote);
    if (result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = "会话数据过大";
        return;
    }
    if (!persisted.active) {
        state->mode = WordAppMode::kHome;
        state->message = "暂时没有建议复习的单词";
        return;
    }
    result = SavePersistedWordSession(persisted);
    if (result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = "会话未保存，请重试";
        return;
    }
    state->session.persisted = std::move(persisted);
    state->session.commit_state = WordObservationCommitState::kIdle;
    state->session.create_request_id.clear();
    result = LoadCurrentReviewWord(state);
    if (result != ESP_OK) {
        state->session.persisted.active = false;
        ESP_ERROR_CHECK_WITHOUT_ABORT(SavePersistedWordSession(state->session.persisted));
        state->mode = WordAppMode::kHome;
        state->message = "会话词包尚未就绪";
        return;
    }
    state->mode = WordAppMode::kReviewFront;
    state->message = "确认翻面";
}

bool TakeWordObservationEffect(
    WordAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    DurableWordObservation* observation,
    PersistedWordSession* advanced_session)
{
    if (state == nullptr || observation == nullptr || advanced_session == nullptr ||
        !state->session.observation_effect_ready || request_id.empty() ||
        occurred_at.empty()) {
        return false;
    }
    auto& pending = state->session.pending_observation;
    if (pending.request_id.empty()) {
        pending.request_id = request_id;
        pending.occurred_at = occurred_at;
    }
    PersistedWordSession advanced = state->session.persisted;
    advanced.position = pending.next_position;
    advanced.phase = pending.next_phase;
    advanced.remote.next_sequence = pending.sequence + 1;
    if (advanced.position >= advanced.remote.items.size()) {
        advanced.active = false;
        advanced.paused = false;
    }
    state->session.pending_advanced_session = advanced;
    state->session.observation_effect_ready = false;
    *observation = pending;
    *advanced_session = std::move(advanced);
    return true;
}

void ApplyWordObservationCommitResult(WordAppState* state, esp_err_t result)
{
    if (state == nullptr) return;
    if (result != ESP_OK) {
        state->session.commit_state = WordObservationCommitState::kFailed;
        state->message = result == ESP_ERR_NO_MEM
            ? "记录空间已满，确认重试"
            : "未保存，确认重试";
        return;
    }
    const auto action = state->session.pending_observation.action;
    state->session.persisted = std::move(state->session.pending_advanced_session);
    state->session.pending_advanced_session = {};
    state->session.pending_observation = {};
    state->session.commit_state = WordObservationCommitState::kCloudPending;
    if (state->outbox.pending_count < state->outbox.capacity) {
        ++state->outbox.pending_count;
    }
    if (action == protocol::word_study_v1::ObservationAction::kKnown) {
        ++state->reviewed_today;
        ++state->correct_today;
        state->message = "已保存，待同步";
    } else if (action == protocol::word_study_v1::ObservationAction::kUnknown) {
        ++state->reviewed_today;
        state->message = "已保存到遗忘单词，待同步";
    } else if (action == protocol::word_study_v1::ObservationAction::kSkipped) {
        state->message = "已记录跳过，待同步";
    } else {
        state->message = "已保存，待同步";
    }
    FinishOrLoadAdvancedReview(state);
}

void RefreshWordOutboxState(WordAppState* state)
{
    if (state == nullptr) return;
    WordOutboxSnapshot snapshot;
    if (ReadWordOutboxSnapshot(&snapshot) != ESP_OK) return;
    state->outbox.pending_count = snapshot.pending_count;
    state->outbox.capacity = snapshot.capacity;
    if (snapshot.pending_count == 0 &&
        state->session.commit_state == WordObservationCommitState::kCloudPending) {
        state->session.commit_state = WordObservationCommitState::kCloudAcknowledged;
        if (state->mode == WordAppMode::kReviewFront ||
            state->mode == WordAppMode::kReviewBack) {
            state->message = "已同步";
        }
    }
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
    snapshot.daily_target = ClampUint16(state.session.persisted.remote.items.size());
    snapshot.due_count = ClampUint16(state.pack_index.entries.size());
    snapshot.total_count = ClampUint16(state.pack_index.entries.size());
    snapshot.card_count = ClampUint16(state.session.persisted.remote.items.size());
    snapshot.card_position = state.session.persisted.remote.items.empty()
        ? 0
        : ClampUint16(state.session.persisted.position + 1);
    snapshot.finished_today = !state.session.persisted.active &&
        !state.session.persisted.remote.session_id.empty();
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
    if (state.session.persisted.remote.items.empty()) {
        return "";
    }
    const size_t visible_position = std::min(
        static_cast<size_t>(state.session.persisted.position) + 1,
        state.session.persisted.remote.items.size());
    return std::to_string(visible_position) + "/" +
           std::to_string(state.session.persisted.remote.items.size());
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
    if (state.outbox.pending_count > 0) {
        return "待同步 " + std::to_string(state.outbox.pending_count) + " 条";
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
    signature.append(std::to_string(state.session.persisted.position));
    signature.push_back('/');
    signature.append(std::to_string(state.session.persisted.remote.items.size()));
    signature.push_back('/');
    signature.append(std::to_string(state.session.persisted.remote.next_sequence));
    signature.push_back('/');
    signature.append(std::to_string(state.outbox.pending_count));
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
