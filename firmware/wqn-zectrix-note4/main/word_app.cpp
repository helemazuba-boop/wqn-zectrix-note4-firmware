#include "word_app.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "wqn_word";
constexpr size_t kDictionaryPreviewLimit = 8;
constexpr size_t kWordHomeSelectionCount = 3;
constexpr size_t kCandidatePrefetchThreshold =
    wqn::protocol::word_study_v1::kInitialCandidatePageSize;
constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes[] = {
    wqn::protocol::word_study_v1::Mode::kSequential,
    wqn::protocol::word_study_v1::Mode::kRandom,
    wqn::protocol::word_study_v1::Mode::kDictionary,
};

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
            return "顺序";
        case wqn::WordHomeSelection::kRandom:
            return "随机";
        case wqn::WordHomeSelection::kDictionary:
            return "词典";
    }
    return "顺序";
}

bool HasPackWords(const wqn::WordAppState& state)
{
    return !state.pack_index.entries.empty();
}

wqn::WordCardPhase CardPhaseFromSession(
    const wqn::PersistedWordSession& session)
{
    return session.phase == wqn::WordPresentationPhase::kBack
        ? wqn::WordCardPhase::kRevealed
        : wqn::WordCardPhase::kFront;
}

void ShowStudyCard(wqn::WordAppState* state)
{
    if (state == nullptr) return;
    state->mode = wqn::WordAppMode::kWordCard;
    state->card_source = wqn::WordCardSource::kStudy;
    state->card_phase = CardPhaseFromSession(state->session.persisted);
}

void ShowDictionaryCard(wqn::WordAppState* state)
{
    if (state == nullptr) return;
    state->mode = wqn::WordAppMode::kWordCard;
    state->card_source = wqn::WordCardSource::kDictionary;
    state->card_phase = wqn::WordCardPhase::kRevealed;
}

void SetStudySessionResumable(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::Mode mode,
    bool resumable)
{
    if (state == nullptr) return;
    if (mode == wqn::protocol::word_study_v1::Mode::kSequential) {
        state->sequential_session_resumable = resumable;
    } else if (mode == wqn::protocol::word_study_v1::Mode::kRandom) {
        state->random_session_resumable = resumable;
    }
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
    const uint64_t current_ordinal =
        state->session.persisted.remote.items[
            state->session.persisted.position].ordinal;
    const bool advances = next_position > state->session.persisted.position;
    const uint64_t next_ordinal = current_ordinal + (advances ? 1U : 0U);
    if (next_ordinal > UINT32_MAX) {
        state->message = "会话游标超限";
        return;
    }
    observation.next_position = static_cast<uint32_t>(next_ordinal);
    observation.next_phase = next_phase;
    state->session.commit_state = wqn::WordObservationCommitState::kPersisting;
    state->card_phase = wqn::WordCardPhase::kPersisting;
    state->session.observation_effect_ready = true;
    state->message = "正在保存";
}

void PrepareDictionaryObservation(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::ObservationAction action)
{
    if (state == nullptr) return;
    if (state->current_word.id.empty()) {
        state->message = "临时词无法写入学习记录";
        return;
    }
    const auto& session = state->session.persisted;
    if (!session.active || session.remote.session_id.empty() ||
        session.remote.mode !=
            wqn::protocol::word_study_v1::Mode::kDictionary) {
        state->message = "记录尚未就绪，请稍后重试";
        return;
    }
    auto& observation = state->session.pending_observation;
    observation = {};
    observation.session_id = session.remote.session_id;
    observation.sequence = session.remote.next_sequence;
    observation.item_id = state->current_word.id;
    observation.action = action;
    observation.mode = wqn::protocol::word_study_v1::Mode::kDictionary;
    observation.next_position = session.position;
    observation.next_phase = wqn::WordPresentationPhase::kBack;
    state->session.commit_state =
        wqn::WordObservationCommitState::kPersisting;
    state->card_phase = wqn::WordCardPhase::kPersisting;
    state->session.observation_effect_ready = true;
    state->message = "正在保存";
}

size_t RemainingCandidateItems(const wqn::PersistedWordSession& session)
{
    return session.position < session.remote.items.size()
        ? session.remote.items.size() - session.position
        : 0;
}

void RequestCandidatePageIfNeeded(wqn::WordAppState* state)
{
    if (state == nullptr || !state->session.persisted.active ||
        state->session.persisted.paused ||
        !state->session.persisted.remote.has_more ||
        state->session.page_in_flight || state->session.page_requested) {
        return;
    }
    if (RemainingCandidateItems(state->session.persisted) <=
        kCandidatePrefetchThreshold) {
        state->session.page_requested = true;
    }
}

bool SetSessionCursorOrdinal(
    wqn::PersistedWordSession* session,
    uint32_t ordinal)
{
    if (session == nullptr) return false;
    const auto match = std::find_if(
        session->remote.items.begin(), session->remote.items.end(),
        [&](const auto& item) { return item.ordinal == ordinal; });
    if (match != session->remote.items.end()) {
        session->position = static_cast<uint32_t>(
            match - session->remote.items.begin());
        return true;
    }
    const uint64_t end_ordinal = session->remote.items.empty()
        ? 0
        : session->remote.items.back().ordinal + 1;
    if (ordinal == end_ordinal) {
        session->position = static_cast<uint32_t>(session->remote.items.size());
        return true;
    }
    return false;
}

bool SnapshotMatches(
    const wqn::StoredWordSessionData& session,
    const wqn::protocol::word_study_v1::CandidatePageData& page)
{
    if (session.snapshot.size() != page.snapshot.size()) return false;
    for (size_t index = 0; index < session.snapshot.size(); ++index) {
        const auto& stored = session.snapshot[index];
        const auto& remote = page.snapshot[index];
        if (remote.deck_id != stored.deck_id ||
            remote.content_revision != stored.content_revision ||
            remote.pack_revision != stored.pack_revision ||
            remote.sha256 != stored.sha256) {
            return false;
        }
    }
    return true;
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
        (state->session.persisted.active && !state->session.persisted.paused) ||
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
            ShowStudyCard(state);
            return;
        }
        state->message = "会话词包不可用";
    }
    if (session.active && session.position == session.remote.items.size() &&
        session.remote.has_more) {
        state->mode = wqn::WordAppMode::kSessionStarting;
        state->session.page_requested = true;
        state->message = "正在加载后续单词";
        return;
    }
    session.active = false;
    session.paused = false;
    SetStudySessionResumable(state, session.remote.mode, false);
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
    state->message = "本轮浏览完成";
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

bool LookupResultMatches(
    const wqn::WordAppState& state,
    const std::string& query)
{
    return state.mode == wqn::WordAppMode::kDictionaryPicker &&
        state.dictionary_stage == wqn::WordDictionaryStage::kLookupChoice &&
        state.lookup_result_expected && !query.empty() &&
        query == state.active_lookup_query;
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
    state->card_phase = WordCardPhase::kFront;
    state->card_source = WordCardSource::kStudy;
    state->dictionary_stage = WordDictionaryStage::kLetters;
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
    bool found_paused_session = false;
    bool found_corrupt_session = false;
    for (const auto mode : kPersistedSessionModes) {
        PersistedWordSession persisted;
        const esp_err_t session_result = LoadPersistedWordSession(mode, &persisted);
        const bool resumable = persisted.active &&
            (persisted.position < persisted.remote.items.size() ||
             (persisted.position == persisted.remote.items.size() &&
              persisted.remote.has_more));
        if (session_result == ESP_OK && resumable &&
            persisted.remote.mode ==
                protocol::word_study_v1::Mode::kDictionary) {
            // Dictionary is an arbitrary lookup context, not a card cursor to
            // auto-resume. Keep its server session for explicit observations,
            // but always return to the neutral word home after reboot.
            if (!persisted.paused) {
                persisted.paused = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    SavePersistedWordSession(persisted));
            }
            continue;
        }
        if (session_result == ESP_OK && resumable) {
            if (persisted.remote.mode ==
                protocol::word_study_v1::Mode::kSequential) {
                state->sequential_session_resumable = true;
            } else if (persisted.remote.mode ==
                       protocol::word_study_v1::Mode::kRandom) {
                state->random_session_resumable = true;
            }
        }
        if (session_result == ESP_OK && resumable) {
            // At most one session should be unpaused. Prefer it so a reset
            // restores the exact card and presentation phase.
            if (!persisted.paused) {
                state->session.persisted = std::move(persisted);
                if (state->session.persisted.position ==
                    state->session.persisted.remote.items.size()) {
                    state->mode = WordAppMode::kSessionStarting;
                    state->session.page_requested = true;
                    state->message = "正在恢复后续单词";
                } else if (LoadCurrentReviewWord(state) == ESP_OK) {
                    ShowStudyCard(state);
                    state->message = "已恢复上次会话";
                } else {
                    state->mode = WordAppMode::kHome;
                    state->message = "上次会话可继续";
                }
                break;
            }
            if (!found_paused_session) {
                state->session.persisted = std::move(persisted);
                found_paused_session = true;
            }
        } else if (session_result != ESP_OK && session_result != ESP_ERR_NOT_FOUND) {
            found_corrupt_session = true;
            ESP_LOGW(
                kTag,
                "load persisted word session failed: mode=%u error=%s",
                static_cast<unsigned>(mode),
                esp_err_to_name(session_result));
        }
    }
    RequestCandidatePageIfNeeded(state);
    if (state->mode == WordAppMode::kHome && found_paused_session) {
        state->message = "上次会话可继续";
    } else if (state->mode == WordAppMode::kHome && found_corrupt_session) {
        state->message = "会话记录损坏";
    }

    ESP_LOGI(
        kTag,
        "word runtime restored: mode=%u session=%s position=%u phase=%u pending=%u",
        static_cast<unsigned>(state->mode),
        state->session.persisted.remote.session_id.empty() ? "none" :
            state->session.persisted.remote.session_id.c_str(),
        static_cast<unsigned>(state->session.persisted.position),
        static_cast<unsigned>(state->session.persisted.phase),
        static_cast<unsigned>(state->outbox.pending_count));

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
                CancelWordLookupResult(state);
                state->mode = WordAppMode::kDictionaryPicker;
                state->dictionary_stage = WordDictionaryStage::kLetters;
                state->dictionary_prefix.clear();
                RefreshDictionaryState(state);
                PersistedWordSession dictionary_session;
                const bool can_record =
                    LoadPersistedWordSession(
                        protocol::word_study_v1::Mode::kDictionary,
                        &dictionary_session) == ESP_OK &&
                    dictionary_session.active;
                if (can_record) {
                    dictionary_session.paused = false;
                    state->session.persisted = std::move(dictionary_session);
                    state->session.commit_state =
                        WordObservationCommitState::kIdle;
                } else if (HasPackWords(*state)) {
                    if (state->session.requested_mode !=
                        protocol::word_study_v1::Mode::kDictionary) {
                        state->session.create_request_id.clear();
                    }
                    state->session.requested_mode =
                        protocol::word_study_v1::Mode::kDictionary;
                    state->session.start_requested = true;
                }
                state->message = !HasPackWords(*state)
                    ? "词库未同步"
                    : (can_record ? "选择首字母" : "选择首字母，记录准备中");
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
            PersistedWordSession stored_session;
            if (LoadPersistedWordSession(requested_mode, &stored_session) == ESP_OK &&
                stored_session.active && stored_session.paused &&
                (stored_session.position < stored_session.remote.items.size() ||
                 (stored_session.position == stored_session.remote.items.size() &&
                stored_session.remote.has_more))) {
                state->session.persisted = std::move(stored_session);
                if (!WordPackIndexMatchesSession(
                        state->pack_index,
                        state->session.persisted)) {
                    WordPackIndex pinned_index;
                    const esp_err_t pinned_result = LoadWordPackIndexForSession(
                        state->session.persisted, &pinned_index);
                    if (pinned_result != ESP_OK || pinned_index.pack_error) {
                        state->message = "会话词包不可用";
                        return ESP_OK;
                    }
                    InstallWordPackIndex(
                        state, std::move(pinned_index), "已载入会话词包");
                } else {
                    ESP_LOGI(
                        kTag,
                        "reuse in-memory word pack index for pinned session");
                }
            }
            if (state->session.persisted.active &&
                state->session.persisted.paused &&
                state->session.persisted.remote.mode == requested_mode &&
                (state->session.persisted.position <
                     state->session.persisted.remote.items.size() ||
                 (state->session.persisted.position ==
                      state->session.persisted.remote.items.size() &&
                  state->session.persisted.remote.has_more))) {
                state->session.persisted.paused = false;
                SetStudySessionResumable(
                    state, state->session.persisted.remote.mode, false);
                ESP_RETURN_ON_ERROR(
                    SavePersistedWordSession(state->session.persisted),
                    kTag,
                    "resume word session");
                if (state->session.persisted.position ==
                    state->session.persisted.remote.items.size()) {
                    state->mode = WordAppMode::kSessionStarting;
                    state->session.page_requested = true;
                    state->message = "正在加载后续单词";
                } else {
                    ESP_RETURN_ON_ERROR(LoadCurrentReviewWord(state), kTag, "load resumed word");
                    ShowStudyCard(state);
                    state->message = "已继续上次会话";
                    RequestCandidatePageIfNeeded(state);
                }
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
            if (input == WordInput::kConfirm &&
                state->session.persisted.active &&
                state->session.persisted.position ==
                    state->session.persisted.remote.items.size() &&
                state->session.persisted.remote.has_more) {
                state->session.page_requested = true;
                state->message = "正在重试加载";
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                state->session.start_requested = false;
                CancelWordSessionStartResult(state);
                if (state->session.persisted.active) {
                    state->session.persisted.paused = true;
                    SetStudySessionResumable(
                        state, state->session.persisted.remote.mode, true);
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        SavePersistedWordSession(state->session.persisted));
                }
                state->mode = WordAppMode::kHome;
                state->message = state->session.persisted.active
                    ? "本轮已暂停"
                    : "已取消";
            }
            return ESP_OK;

        case WordAppMode::kWordCard:
            if (state->card_phase == WordCardPhase::kPersisting) {
                return ESP_OK;
            }
            if (state->session.commit_state == WordObservationCommitState::kFailed) {
                if (input == WordInput::kConfirm) {
                    state->session.commit_state = WordObservationCommitState::kPersisting;
                    state->card_phase = WordCardPhase::kPersisting;
                    state->session.observation_effect_ready = true;
                    state->message = "正在重试保存";
                }
                return ESP_OK;
            }
            if (state->card_source == WordCardSource::kDictionary) {
                // Merely opening the card is read-only. The same revealed-card
                // controls as study create an explicit durable observation.
                if (input == WordInput::kConfirm) {
                    PrepareDictionaryObservation(
                        state,
                        protocol::word_study_v1::ObservationAction::kKnown);
                } else if (input == WordInput::kUp) {
                    PrepareDictionaryObservation(
                        state,
                        protocol::word_study_v1::ObservationAction::kUnknown);
                } else if (input == WordInput::kDown) {
                    PrepareDictionaryObservation(
                        state,
                        protocol::word_study_v1::ObservationAction::kSkipped);
                } else if (input == WordInput::kLongConfirm) {
                    state->mode = WordAppMode::kDictionaryPicker;
                    state->dictionary_stage = WordDictionaryStage::kLetters;
                    state->message = state->dictionary_prefix.empty()
                        ? "选择首字母"
                        : state->dictionary_prefix;
                }
                return ESP_OK;
            }
            if (state->card_phase == WordCardPhase::kFront) {
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
                    SetStudySessionResumable(
                        state, state->session.persisted.remote.mode, true);
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        SavePersistedWordSession(state->session.persisted));
                    state->mode = WordAppMode::kHome;
                    state->message = "本轮已暂停";
                    ActivatePendingWordPackIndex(state);
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
                SetStudySessionResumable(
                    state, state->session.persisted.remote.mode, true);
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    SavePersistedWordSession(state->session.persisted));
                state->mode = WordAppMode::kHome;
                state->message = "本轮已暂停";
                ActivatePendingWordPackIndex(state);
            }
            return ESP_OK;

        case WordAppMode::kDictionaryPicker:
            if (state->dictionary_stage == WordDictionaryStage::kLookupChoice) {
                if (input == WordInput::kUp || input == WordInput::kDown) {
                    state->lookup_selection =
                        state->lookup_selection == WordLookupSelection::kOnlineSearch
                        ? WordLookupSelection::kAiLookup
                        : WordLookupSelection::kOnlineSearch;
                    return ESP_OK;
                }
                if (input == WordInput::kLongConfirm) {
                    CancelWordLookupResult(state);
                    state->dictionary_stage = WordDictionaryStage::kLetters;
                    state->message = state->dictionary_prefix.empty()
                        ? "选择首字母"
                        : state->dictionary_prefix;
                    return ESP_OK;
                }
                if (input == WordInput::kConfirm) {
                    if (state->lookup_selection ==
                        WordLookupSelection::kOnlineSearch) {
                        RequestOnlineLookup(state);
                    } else {
                        RequestAiLookup(state);
                    }
                }
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                if (!state->dictionary_prefix.empty()) {
                    state->dictionary_prefix.pop_back();
                    RefreshDictionaryState(state);
                    state->dictionary_letter_selected = 0;
                    state->dictionary_match_selected = 0;
                    state->message = state->dictionary_prefix.empty() ? "选择首字母" : state->dictionary_prefix;
                } else {
                    CancelWordSessionStartResult(state);
                    if (state->session.persisted.active &&
                        state->session.persisted.remote.mode ==
                            protocol::word_study_v1::Mode::kDictionary) {
                        state->session.persisted.paused = true;
                        ESP_ERROR_CHECK_WITHOUT_ABORT(
                            SavePersistedWordSession(
                                state->session.persisted));
                    }
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
                        ShowDictionaryCard(state);
                        state->message = "词典浏览，不自动改变进度";
                    } else {
                        state->message = "词条读取失败";
                    }
                } else if (state->dictionary_matches.empty()) {
                    state->dictionary_stage = WordDictionaryStage::kLookupChoice;
                    state->lookup_selection = WordLookupSelection::kOnlineSearch;
                    state->message = "本地未命中";
                } else {
                    state->message = state->dictionary_prefix;
                }
                return ESP_OK;
            }
            if (!state->dictionary_matches.empty()) {
                if (LoadCurrentDictionaryWord(state) == ESP_OK) {
                    ShowDictionaryCard(state);
                    state->message = "词典浏览，不自动改变进度";
                } else {
                    state->message = "词条读取失败";
                }
                return ESP_OK;
            }
            state->dictionary_stage = WordDictionaryStage::kLookupChoice;
            state->lookup_selection = WordLookupSelection::kOnlineSearch;
            state->message = "本地未命中";
            return ESP_OK;
    }

    return ESP_OK;
}

void ApplyWordPackIndex(WordAppState* state, WordPackIndex index, const std::string& message)
{
    if (state == nullptr) {
        return;
    }
    if (state->mode != WordAppMode::kHome ||
        (state->session.persisted.active && !state->session.persisted.paused)) {
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

bool ApplyWordSearchResult(
    WordAppState* state,
    const std::string& query,
    const WqnWordSearchResult& result)
{
    if (state == nullptr || !LookupResultMatches(*state, query)) return false;
    CancelWordLookupResult(state);
    state->online_results = result.words;
    state->online_result_selected = 0;
    if (!state->online_results.empty()) {
        state->current_word = state->online_results.front();
        ShowDictionaryCard(state);
        state->message = "在线搜索结果";
    } else {
        state->mode = WordAppMode::kDictionaryPicker;
        state->dictionary_stage = WordDictionaryStage::kLookupChoice;
        state->lookup_selection = WordLookupSelection::kAiLookup;
        state->message = "未找到，确认询问 AI";
    }
    return true;
}

bool ApplyWordAiLookupResult(
    WordAppState* state,
    const std::string& query,
    const WqnWordAiLookupResult& result)
{
    if (state == nullptr || !LookupResultMatches(*state, query)) return false;
    CancelWordLookupResult(state);
    state->lookup_word = result.word;
    state->current_word = result.word;
    ShowDictionaryCard(state);
    state->message = "AI 临时释义";
    return true;
}

bool ApplyWordLookupFailure(
    WordAppState* state,
    const std::string& query,
    const std::string& message)
{
    if (state == nullptr || !LookupResultMatches(*state, query)) return false;
    CancelWordLookupResult(state);
    state->message = message;
    return true;
}

void CancelWordLookupResult(WordAppState* state)
{
    if (state == nullptr) return;
    state->search_pending = false;
    state->ai_lookup_pending = false;
    state->lookup_result_expected = false;
    state->pending_search_query.clear();
    state->pending_ai_query.clear();
    state->active_lookup_query.clear();
}

bool TakeWordSearchRequest(WordAppState* state, WqnWordSearchRequest* request)
{
    if (state == nullptr || request == nullptr || !state->search_pending ||
        state->mode != WordAppMode::kDictionaryPicker ||
        state->dictionary_stage != WordDictionaryStage::kLookupChoice) {
        return false;
    }
    request->query = state->pending_search_query;
    request->limit = 8;
    state->search_pending = false;
    state->lookup_result_expected = true;
    state->active_lookup_query = request->query;
    state->pending_search_query.clear();
    return true;
}

bool TakeWordAiLookupRequest(WordAppState* state, WqnWordAiLookupRequest* request)
{
    if (state == nullptr || request == nullptr || !state->ai_lookup_pending ||
        state->mode != WordAppMode::kDictionaryPicker ||
        state->dictionary_stage != WordDictionaryStage::kLookupChoice) {
        return false;
    }
    request->query = state->pending_ai_query;
    state->ai_lookup_pending = false;
    state->lookup_result_expected = true;
    state->active_lookup_query = request->query;
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
    request->optional_count = 500;
    request->seed.clear();
    state->session.start_requested = false;
    state->session.start_result_expected = true;
    return true;
}

bool ApplyWordSessionStartResult(
    WordAppState* state,
    esp_err_t result,
    protocol::word_study_v1::SessionData session)
{
    if (state == nullptr || !state->session.start_result_expected) return false;
    state->session.start_result_expected = false;
    const bool dictionary_request = state->session.requested_mode ==
        protocol::word_study_v1::Mode::kDictionary;
    if (result != ESP_OK) {
        if (!dictionary_request) {
            state->mode = WordAppMode::kHome;
        }
        state->message = result == ESP_ERR_INVALID_STATE
            ? "请先完成配对"
            : (dictionary_request
                  ? "词典可浏览，记录准备失败"
                  : "本轮准备失败，可重试");
        return true;
    }
    PersistedWordSession persisted;
    const bool dictionary_session = session.mode ==
        protocol::word_study_v1::Mode::kDictionary;
    persisted.active = !session.items.empty();
    persisted.paused = false;
    persisted.position = 0;
    persisted.phase = WordPresentationPhase::kFront;
    result = CompactWordSessionData(session, &persisted.remote);
    if (result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = "会话数据过大";
        return true;
    }
    if (!persisted.active) {
        state->session.create_request_id.clear();
        if (dictionary_session) {
            state->message = "词典可浏览，学习记录暂不可用";
        } else {
            state->mode = WordAppMode::kHome;
            state->message = "当前范围没有可浏览的单词";
        }
        return true;
    }
    result = SavePersistedWordSession(persisted);
    if (result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = "会话未保存，请重试";
        return true;
    }
    state->session.persisted = std::move(persisted);
    SetStudySessionResumable(
        state, state->session.persisted.remote.mode, false);
    state->session.commit_state = WordObservationCommitState::kIdle;
    state->session.page_in_flight = false;
    state->session.page_requested = false;
    state->session.create_request_id.clear();
    if (dictionary_session) {
        state->session.persisted.paused = false;
        if (state->mode == WordAppMode::kWordCard &&
            state->card_source == WordCardSource::kDictionary) {
            state->card_phase = WordCardPhase::kRevealed;
        } else {
            state->mode = WordAppMode::kDictionaryPicker;
            state->dictionary_stage = WordDictionaryStage::kLetters;
        }
        state->message = "词典记录已就绪";
        return true;
    }
    result = LoadCurrentReviewWord(state);
    if (result != ESP_OK) {
        state->session.persisted.active = false;
        SetStudySessionResumable(
            state, state->session.persisted.remote.mode, false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(SavePersistedWordSession(state->session.persisted));
        state->mode = WordAppMode::kHome;
        state->message = "会话词包尚未就绪";
        return true;
    }
    ShowStudyCard(state);
    state->message = "确认翻面";
    RequestCandidatePageIfNeeded(state);
    return true;
}

void CancelWordSessionStartResult(WordAppState* state)
{
    if (state == nullptr) return;
    state->session.start_requested = false;
    state->session.start_result_expected = false;
    state->session.create_request_id.clear();
}

bool TakeWordCandidatePageRequest(
    WordAppState* state,
    protocol::word_study_v1::CandidatePageRequest* request,
    std::string* session_id)
{
    if (state == nullptr || request == nullptr || session_id == nullptr ||
        !state->session.page_requested || state->session.page_in_flight ||
        !state->session.persisted.active ||
        state->session.persisted.paused ||
        !state->session.persisted.remote.has_more ||
        state->session.persisted.remote.cursor.empty()) {
        return false;
    }
    request->cursor = state->session.persisted.remote.cursor;
    request->limit = static_cast<int>(
        protocol::word_study_v1::kCandidatePrefetchPageSize);
    *session_id = state->session.persisted.remote.session_id;
    state->session.page_requested = false;
    state->session.page_in_flight = true;
    return true;
}

void RestoreWordCandidatePageRequest(WordAppState* state)
{
    if (state == nullptr) return;
    state->session.page_in_flight = false;
    if (state->session.persisted.active &&
        !state->session.persisted.paused &&
        state->session.persisted.remote.has_more) {
        state->session.page_requested = true;
    }
}

void ApplyWordCandidatePageResult(
    WordAppState* state,
    esp_err_t result,
    protocol::word_study_v1::CandidatePageData page)
{
    if (state == nullptr) return;
    state->session.page_in_flight = false;
    auto& persisted = state->session.persisted;
    auto& remote = persisted.remote;
    if (!persisted.active || persisted.paused) {
        // The user left or paused while this bounded prefetch was in flight.
        // Its result belongs to the old interaction context and must not
        // replace the home message or revive the session UI.
        return;
    }
    if (result != ESP_OK) {
        state->session.page_requested = false;
        state->message = "后续单词加载失败，继续时重试";
        return;
    }
    if (page.session_id != remote.session_id || page.ordering != remote.ordering ||
        page.candidate_policy_version !=
            protocol::word_study_v1::CandidatePolicyVersionName(remote.ordering) ||
        page.seed != remote.seed || page.progress_revision != remote.progress_revision ||
        page.cursor != remote.cursor || !SnapshotMatches(remote, page)) {
        state->message = "后续单词快照不一致";
        return;
    }

    if (persisted.position > remote.items.size()) {
        state->message = "会话游标损坏";
        return;
    }
    PersistedWordSession updated = persisted;
    auto& updated_remote = updated.remote;
    if (updated.position > 0) {
        updated_remote.items.erase(
            updated_remote.items.begin(),
            updated_remote.items.begin() + updated.position);
        updated.position = 0;
    }
    if (updated_remote.items.size() + page.items.size() >
        protocol::word_study_v1::kCandidateWindowSize) {
        state->message = "候选窗口超限";
        return;
    }
    uint64_t expected_ordinal = updated_remote.items.empty()
        ? (page.items.empty() ? 0 : page.items.front().ordinal)
        : updated_remote.items.back().ordinal + 1;
    for (const auto& source : page.items) {
        if (source.ordinal != expected_ordinal || source.item_id.size() != 36 ||
            source.deck_id.size() != 36) {
            state->message = "候选页顺序无效";
            return;
        }
        StoredWordSessionItem item;
        std::snprintf(item.item_id, sizeof(item.item_id), "%s", source.item_id.c_str());
        std::snprintf(item.deck_id, sizeof(item.deck_id), "%s", source.deck_id.c_str());
        item.ordinal = source.ordinal;
        updated_remote.items.push_back(item);
        ++expected_ordinal;
    }
    updated_remote.cursor = page.next_cursor;
    updated_remote.has_more = page.has_more;
    result = SavePersistedWordSession(updated);
    if (result != ESP_OK) {
        state->message = "后续单词未保存";
        return;
    }
    persisted = std::move(updated);
    auto& committed_remote = persisted.remote;
    if (persisted.position < committed_remote.items.size()) {
        result = LoadCurrentReviewWord(state);
        if (result != ESP_OK) {
            state->message = "会话词包不可用";
            return;
        }
        ShowStudyCard(state);
        state->message = "后续单词已就绪";
    } else if (!committed_remote.has_more) {
        persisted.active = false;
        SetStudySessionResumable(state, persisted.remote.mode, false);
        state->mode = WordAppMode::kHome;
        state->message = "本轮浏览完成";
        ESP_ERROR_CHECK_WITHOUT_ABORT(SavePersistedWordSession(persisted));
    }
    RequestCandidatePageIfNeeded(state);
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
    const bool dictionary_observation =
        pending.mode == protocol::word_study_v1::Mode::kDictionary;
    if (pending.session_id != advanced.remote.session_id ||
        pending.sequence != advanced.remote.next_sequence ||
        (dictionary_observation &&
         advanced.remote.mode !=
             protocol::word_study_v1::Mode::kDictionary) ||
        (!dictionary_observation &&
         !SetSessionCursorOrdinal(&advanced, pending.next_position))) {
        state->session.observation_effect_ready = false;
        state->session.commit_state = WordObservationCommitState::kFailed;
        state->card_phase = CardPhaseFromSession(state->session.persisted);
        state->message = "会话游标无效";
        return false;
    }
    advanced.phase = pending.next_phase;
    advanced.remote.next_sequence = pending.sequence + 1;
    if (!dictionary_observation &&
        advanced.position >= advanced.remote.items.size() &&
        !advanced.remote.has_more) {
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
        state->card_phase = CardPhaseFromSession(state->session.persisted);
        state->message = result == ESP_ERR_NO_MEM
            ? "记录空间已满，确认重试"
            : "未保存，确认重试";
        return;
    }
    const auto action = state->session.pending_observation.action;
    const auto observation_mode = state->session.pending_observation.mode;
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
    if (observation_mode ==
        protocol::word_study_v1::Mode::kDictionary) {
        state->mode = WordAppMode::kDictionaryPicker;
        state->dictionary_stage = WordDictionaryStage::kLetters;
        state->card_source = WordCardSource::kDictionary;
        state->card_phase = WordCardPhase::kRevealed;
        state->current_word = {};
        return;
    }
    RequestCandidatePageIfNeeded(state);
    if (action == protocol::word_study_v1::ObservationAction::kRevealed &&
        state->session.persisted.active &&
        state->session.persisted.position <
            state->session.persisted.remote.items.size()) {
        // Revealing the back does not advance the item. current_word already
        // owns the exact pinned content, so reopening and reparsing the same
        // JSONL record only delays the flip.
        ShowStudyCard(state);
        return;
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
        if (state->mode == WordAppMode::kWordCard &&
            state->card_source == WordCardSource::kStudy) {
            state->message = "已同步";
        }
    }
}

WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state)
{
    WordAppSnapshot snapshot;
    snapshot.mode = state.mode;
    snapshot.card_phase = state.card_phase;
    snapshot.card_source = state.card_source;
    snapshot.dictionary_stage = state.dictionary_stage;
    snapshot.home_selection = state.home_selection;
    snapshot.lookup_selection = state.lookup_selection;
    snapshot.pack_ready = HasPackWords(state);
    snapshot.pack_truncated = state.pack_index.truncated;
    snapshot.cloud_sync_failed = state.cloud_sync_failed;
    snapshot.sequential_session_resumable =
        state.sequential_session_resumable;
    snapshot.random_session_resumable = state.random_session_resumable;
    snapshot.reviewed_today = state.reviewed_today;
    snapshot.correct_today = state.correct_today;
    snapshot.total_count = ClampUint16(state.pack_index.entries.size());
    const bool study_cursor_visible =
        (state.mode == WordAppMode::kWordCard &&
         state.card_source == WordCardSource::kStudy) ||
        state.mode == WordAppMode::kSessionStarting;
    snapshot.card_count = study_cursor_visible
        ? ClampUint16(state.session.persisted.remote.items.size())
        : 0;
    snapshot.card_position = !study_cursor_visible ||
            state.session.persisted.remote.items.empty()
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
    if ((state.mode != WordAppMode::kWordCard ||
         state.card_source != WordCardSource::kStudy) &&
        state.mode != WordAppMode::kSessionStarting) {
        return "";
    }
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
    signature.append(std::to_string(static_cast<int>(state.card_phase)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.card_source)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.dictionary_stage)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.home_selection)));
    signature.push_back('/');
    signature.append(state.sequential_session_resumable ? "1" : "0");
    signature.append(state.random_session_resumable ? "1" : "0");
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

namespace {

// WordAppState is intentionally large (currently 1408 bytes). The boot
// contract test needs several independent states, so keeping them as ordinary
// locals would exceed app_main's fixed 8 KiB stack even though the test runs
// sequentially. Allocate each bounded fixture in PSRAM and release it when the
// self-test returns; production runtime state ownership is unchanged.
class WordPageFixtureState {
public:
    WordPageFixtureState()
    {
        storage_ = heap_caps_malloc(
            sizeof(WordAppState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage_ != nullptr) {
            state_ = new (storage_) WordAppState();
        }
    }

    ~WordPageFixtureState()
    {
        if (state_ != nullptr) {
            state_->~WordAppState();
        }
        heap_caps_free(storage_);
    }

    WordPageFixtureState(const WordPageFixtureState&) = delete;
    WordPageFixtureState& operator=(const WordPageFixtureState&) = delete;

    explicit operator bool() const { return state_ != nullptr; }
    WordAppState* operator->() { return state_; }
    WordAppState& get() { return *state_; }

private:
    void* storage_ = nullptr;
    WordAppState* state_ = nullptr;
};

}  // namespace

bool RunWordPageStateSelfTest()
{
    auto require = [](bool condition, const char* label) {
        if (!condition) {
            ESP_LOGE(kTag, "word page self-test failed: %s", label);
        }
        return condition;
    };

    WordPageFixtureState home;
    if (!home) return require(false, "allocate home fixture");
    home->initialized = true;
    home->mode = WordAppMode::kHome;
    for (size_t index = 0; index < 500; ++index) {
        const WordInput input = (index % 2 == 0)
            ? WordInput::kDown
            : WordInput::kUp;
        if (HandleWordAppInput(&home.get(), input) != ESP_OK) return false;
    }
    if (!require(home->mode == WordAppMode::kHome,
                 "home mixed navigation remains home")) {
        return false;
    }

    WordPageFixtureState study;
    if (!study) return require(false, "allocate study fixture");
    study->initialized = true;
    study->mode = WordAppMode::kWordCard;
    study->card_source = WordCardSource::kStudy;
    study->card_phase = WordCardPhase::kFront;
    study->session.persisted.active = true;
    study->session.persisted.phase = WordPresentationPhase::kFront;
    study->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000001";
    study->session.persisted.remote.next_sequence = 7;
    StoredWordSessionItem item;
    std::snprintf(
        item.item_id,
        sizeof(item.item_id),
        "%s",
        "00000000-0000-4000-8000-000000000002");
    std::snprintf(
        item.deck_id,
        sizeof(item.deck_id),
        "%s",
        "00000000-0000-4000-8000-000000000003");
    item.ordinal = 11;
    study->session.persisted.remote.items.push_back(item);

    if (HandleWordAppInput(&study.get(), WordInput::kConfirm) != ESP_OK ||
        !require(study->mode == WordAppMode::kWordCard,
                 "front and revealed share WordCard") ||
        !require(study->card_phase == WordCardPhase::kPersisting,
                 "front confirm enters persisting") ||
        !require(study->session.pending_observation.action ==
                     protocol::word_study_v1::ObservationAction::kRevealed,
                 "front confirm records revealed")) {
        return false;
    }

    const DurableWordObservation pending =
        study->session.pending_observation;
    for (size_t index = 0; index < 500; ++index) {
        const WordInput input = index % 3 == 0
            ? WordInput::kConfirm
            : (index % 3 == 1 ? WordInput::kDown
                              : WordInput::kLongConfirm);
        if (HandleWordAppInput(&study.get(), input) != ESP_OK) return false;
    }
    if (!require(study->card_phase == WordCardPhase::kPersisting,
                 "persisting blocks mixed input") ||
        !require(study->session.pending_observation.sequence == pending.sequence &&
                     study->session.pending_observation.item_id == pending.item_id &&
                     study->session.pending_observation.action == pending.action,
                 "persisting keeps one observation")) {
        return false;
    }

    WordPageFixtureState mixed;
    if (!mixed) return require(false, "allocate mixed fixture");
    mixed->initialized = true;
    mixed->mode = WordAppMode::kWordCard;
    mixed->card_source = WordCardSource::kStudy;
    mixed->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000010";
    mixed->session.persisted.remote.mode =
        protocol::word_study_v1::Mode::kSequential;
    constexpr size_t kMixedItemCount = 64;
    mixed->session.persisted.remote.items.reserve(kMixedItemCount);
    for (size_t index = 0; index < kMixedItemCount; ++index) {
        StoredWordSessionItem mixed_item;
        std::snprintf(
            mixed_item.item_id,
            sizeof(mixed_item.item_id),
            "00000000-0000-4000-8000-%012u",
            static_cast<unsigned>(index + 100));
        std::snprintf(
            mixed_item.deck_id,
            sizeof(mixed_item.deck_id),
            "%s",
            "00000000-0000-4000-8000-000000000020");
        mixed_item.ordinal = index;
        mixed->session.persisted.remote.items.push_back(mixed_item);
    }
    for (size_t index = 0; index < 500; ++index) {
        const size_t item_index = index % kMixedItemCount;
        mixed->session.persisted.active = true;
        mixed->session.persisted.paused = false;
        mixed->session.persisted.position = item_index;
        mixed->session.persisted.phase = WordPresentationPhase::kBack;
        mixed->session.persisted.remote.next_sequence = index;
        mixed->card_phase = WordCardPhase::kRevealed;
        mixed->session.commit_state = WordObservationCommitState::kIdle;
        const WordInput input = index % 3 == 0
            ? WordInput::kConfirm
            : (index % 3 == 1 ? WordInput::kUp : WordInput::kDown);
        const auto expected_action = index % 3 == 0
            ? protocol::word_study_v1::ObservationAction::kKnown
            : (index % 3 == 1
                   ? protocol::word_study_v1::ObservationAction::kUnknown
                   : protocol::word_study_v1::ObservationAction::kSkipped);
        if (HandleWordAppInput(&mixed.get(), input) != ESP_OK) return false;
        char request_id[40] = {};
        std::snprintf(
            request_id,
            sizeof(request_id),
            "req_word_mix_%016u",
            static_cast<unsigned>(index));
        DurableWordObservation observation;
        PersistedWordSession advanced;
        if (!TakeWordObservationEffect(
                &mixed.get(),
                request_id,
                "2026-07-20T12:00:00Z",
                &observation,
                &advanced) ||
            observation.sequence != index ||
            observation.action != expected_action ||
            observation.item_id !=
                mixed->session.persisted.remote.items[item_index].item_id ||
            advanced.position != item_index + 1 ||
            advanced.remote.next_sequence != index + 1) {
            return require(false, "500 mixed actions preserve attribution");
        }
        mixed->session.pending_observation = {};
        mixed->session.pending_advanced_session = {};
    }

    WordPageFixtureState revealed;
    if (!revealed) return require(false, "allocate revealed fixture");
    revealed->initialized = true;
    revealed->mode = WordAppMode::kWordCard;
    revealed->card_source = WordCardSource::kStudy;
    revealed->card_phase = WordCardPhase::kRevealed;
    revealed->session.persisted = study->session.persisted;
    revealed->session.persisted.phase = WordPresentationPhase::kBack;
    revealed->session.commit_state = WordObservationCommitState::kIdle;
    if (HandleWordAppInput(&revealed.get(), WordInput::kUp) != ESP_OK ||
        !require(revealed->session.pending_observation.action ==
                     protocol::word_study_v1::ObservationAction::kUnknown,
                 "revealed up records unknown") ||
        !require(revealed->card_phase == WordCardPhase::kPersisting,
                 "classification persists before advance")) {
        return false;
    }

    WordPageFixtureState dictionary;
    if (!dictionary) return require(false, "allocate dictionary fixture");
    dictionary->initialized = true;
    dictionary->mode = WordAppMode::kWordCard;
    dictionary->card_source = WordCardSource::kDictionary;
    dictionary->card_phase = WordCardPhase::kRevealed;
    dictionary->current_word.id = item.item_id;
    dictionary->current_word.word = "baseline";
    if (HandleWordAppInput(&dictionary.get(), WordInput::kLongConfirm) != ESP_OK ||
        !require(!dictionary->session.observation_effect_ready,
                 "dictionary view does not create progress") ||
        !require(dictionary->mode == WordAppMode::kDictionaryPicker,
                 "dictionary card returns to picker")) {
        return false;
    }

    dictionary->mode = WordAppMode::kWordCard;
    dictionary->card_phase = WordCardPhase::kRevealed;
    dictionary->session.persisted.active = true;
    dictionary->session.persisted.remote.mode =
        protocol::word_study_v1::Mode::kDictionary;
    dictionary->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000004";
    dictionary->session.persisted.remote.next_sequence = 3;
    if (HandleWordAppInput(&dictionary.get(), WordInput::kUp) != ESP_OK ||
        !require(dictionary->session.pending_observation.action ==
                     protocol::word_study_v1::ObservationAction::kUnknown,
                 "dictionary shares revealed-card controls") ||
        !require(dictionary->card_phase == WordCardPhase::kPersisting,
                 "dictionary classification persists first")) {
        return false;
    }
    DurableWordObservation dictionary_observation;
    PersistedWordSession dictionary_advanced;
    if (!require(TakeWordObservationEffect(
                     &dictionary.get(),
                     "req_word_dictionary_0001",
                     "2026-07-20T12:00:00Z",
                     &dictionary_observation,
                     &dictionary_advanced),
                 "dictionary observation enters durable effect") ||
        !require(dictionary_advanced.position == 0 &&
                     dictionary_advanced.remote.next_sequence == 4,
                 "dictionary observation advances sequence only")) {
        return false;
    }

    WordPageFixtureState stale;
    if (!stale) return require(false, "allocate stale-result fixture");
    stale->initialized = true;
    stale->mode = WordAppMode::kHome;
    stale->home_selection = WordHomeSelection::kRandom;
    protocol::word_study_v1::SessionData stale_session;
    if (!require(!ApplyWordSessionStartResult(
                     &stale.get(), ESP_OK, std::move(stale_session)),
                 "cancelled session result is ignored") ||
        !require(stale->mode == WordAppMode::kHome &&
                     stale->home_selection == WordHomeSelection::kRandom,
                 "stale session result preserves selection")) {
        return false;
    }

    stale->mode = WordAppMode::kDictionaryPicker;
    stale->dictionary_stage = WordDictionaryStage::kLookupChoice;
    stale->lookup_result_expected = true;
    stale->active_lookup_query = "alpha";
    CancelWordLookupResult(&stale.get());
    WqnWordSearchResult stale_lookup;
    stale_lookup.prefix = "alpha";
    if (!require(!ApplyWordSearchResult(
                     &stale.get(), "alpha", stale_lookup),
                 "cancelled lookup result is ignored") ||
        !require(stale->mode == WordAppMode::kDictionaryPicker,
                 "stale lookup result preserves picker")) {
        return false;
    }

    const WordAppSnapshot front_snapshot = BuildWordAppSnapshot(study.get());
    dictionary->mode = WordAppMode::kDictionaryPicker;
    dictionary->card_phase = WordCardPhase::kRevealed;
    const WordAppSnapshot dictionary_snapshot = BuildWordAppSnapshot(dictionary.get());
    return require(front_snapshot.mode == WordAppMode::kWordCard,
                   "study snapshot uses WordCard") &&
        require(dictionary_snapshot.mode == WordAppMode::kDictionaryPicker,
                "dictionary snapshot uses picker after return");
}

}  // namespace wqn
