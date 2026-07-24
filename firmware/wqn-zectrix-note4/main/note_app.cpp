#include "note_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "note_app";
// Prefetch the next candidate page when the title selection nears the tail.
constexpr size_t kCandidatePrefetchThreshold = 8;

using wqn::protocol::note_study_v1::Mode;
using wqn::protocol::note_study_v1::ObservationAction;
using wqn::protocol::note_study_v1::Ordering;

// Locates the pack-index notebook row for a notebook id (its entry range holds
// the note titles/offsets). Returns npos when the notebook is not mounted.
size_t FindNotebookRow(const wqn::NoteAppState& state, const std::string& notebook_id)
{
    for (size_t index = 0; index < state.pack_index.notebooks.size(); ++index) {
        if (state.pack_index.notebooks[index].notebook_id == notebook_id) {
            return index;
        }
    }
    return static_cast<size_t>(-1);
}

// Finds the mounted pack entry for a note id, scanning only its notebook's
// contiguous entry range. Returns nullptr when the content is not mounted.
const wqn::NotePackIndexEntry* FindPackEntry(
    const wqn::NoteAppState& state,
    const std::string& notebook_id,
    const std::string& note_id)
{
    // note_order indexes entries sorted by note_id (a globally-unique UUID), so
    // this is O(log N) instead of scanning the notebook's entries per lookup --
    // BuildNoteAppSnapshot calls it once per candidate item.
    const auto& index = state.pack_index;
    const auto it = std::lower_bound(
        index.note_order.begin(), index.note_order.end(), note_id,
        [&index](uint32_t entry_index, const std::string& target) {
            return std::strcmp(index.entries[entry_index].note_id, target.c_str()) < 0;
        });
    if (it == index.note_order.end()) return nullptr;
    const wqn::NotePackIndexEntry& entry = index.entries[*it];
    if (note_id != entry.note_id) return nullptr;
    // note_id is unique; guard only against a caller passing a mismatched pair.
    if (!notebook_id.empty() && notebook_id != entry.notebook_id) return nullptr;
    return &entry;
}

// Best-effort body load for the selected title. Only touches storage when a
// matching pack entry exists, so the reducer stays testable with an empty index.
void LoadCurrentNoteBody(wqn::NoteAppState* state)
{
    state->current_note = wqn::WqnNoteEntry{};
    state->current_note_loaded = false;
    const auto& items = state->session.persisted.remote.items;
    if (state->note_list_selected >= items.size()) return;
    const wqn::StoredNoteSessionItem& item = items[state->note_list_selected];
    const wqn::NotePackIndexEntry* entry =
        FindPackEntry(*state, item.notebook_id, item.item_id);
    if (entry == nullptr) return;
    if (wqn::ReadNotePackEntry(*entry, &state->current_note) == ESP_OK) {
        state->current_note_loaded = true;
    }
}

size_t RemainingCandidateItems(const wqn::PersistedNoteSession& session, size_t selected)
{
    return session.remote.items.size() > selected
        ? session.remote.items.size() - selected
        : 0;
}

void RequestNoteCandidatePageIfNeeded(wqn::NoteAppState* state)
{
    if (state == nullptr || !state->session.persisted.active ||
        state->session.persisted.paused ||
        !state->session.persisted.remote.has_more ||
        state->session.page_in_flight || state->session.page_requested ||
        state->session.commit_state == wqn::NoteObservationCommitState::kPersisting) {
        return;
    }
    if (RemainingCandidateItems(state->session.persisted, state->note_list_selected) <=
        kCandidatePrefetchThreshold) {
        state->session.page_requested = true;
    }
}

bool SnapshotMatches(
    const wqn::StoredNoteSessionData& session,
    const wqn::protocol::note_study_v1::CandidatePageData& page)
{
    if (session.snapshot.size() != page.snapshot.size()) return false;
    for (size_t index = 0; index < session.snapshot.size(); ++index) {
        const auto& stored = session.snapshot[index];
        const auto& remote = page.snapshot[index];
        if (remote.notebook_id != stored.notebook_id ||
            remote.content_revision != stored.content_revision ||
            remote.pack_revision != stored.pack_revision ||
            remote.sha256 != stored.sha256) {
            return false;
        }
    }
    return true;
}

void InstallNotePackIndex(
    wqn::NoteAppState* state,
    wqn::NotePackIndex index,
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
        state->message = state->pack_index.notebooks.empty() ? "笔记未同步" : "笔记已就绪";
    }
    if (state->notebook_selected >= state->pack_index.notebooks.size()) {
        state->notebook_selected = 0;
    }
}

void ActivatePendingNotePackIndex(wqn::NoteAppState* state)
{
    if (state == nullptr || state->mode != wqn::NoteAppMode::kNotebookList ||
        (state->session.persisted.active && !state->session.persisted.paused) ||
        !state->pending_pack_index_ready) {
        return;
    }
    wqn::NotePackIndex pending = std::move(state->pending_pack_index);
    state->pending_pack_index = {};
    state->pending_pack_index_ready = false;
    InstallNotePackIndex(state, std::move(pending), "笔记更新已启用");
}

void HandleNotebookListInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    const size_t count = state->pack_index.notebooks.size();
    switch (input) {
        case wqn::NoteInput::kUp:
            if (state->notebook_selected > 0) --state->notebook_selected;
            break;
        case wqn::NoteInput::kDown:
            if (count > 0 && state->notebook_selected + 1 < count) ++state->notebook_selected;
            break;
        case wqn::NoteInput::kConfirm: {
            if (count == 0 || state->notebook_selected >= count) {
                state->message = "暂无笔记本，稍后同步";
                break;
            }
            const wqn::NotePackNotebook& notebook =
                state->pack_index.notebooks[state->notebook_selected];
            state->session.start_requested = true;
            state->session.start_result_expected = false;
            state->session.create_request_id.clear();
            state->session.requested_notebook_id = notebook.notebook_id;
            state->mode = wqn::NoteAppMode::kSessionStarting;
            state->message = "正在打开笔记本";
            break;
        }
        case wqn::NoteInput::kLongConfirm:
            // Leaving the note screen is owned by the UI runtime; ignore here.
            break;
    }
}

void HandleSessionStartingInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    if (input == wqn::NoteInput::kLongConfirm) {
        wqn::CancelNoteSessionStartResult(state);
        state->mode = wqn::NoteAppMode::kNotebookList;
        state->message = "已取消";
    }
}

void ArmOpenObservation(wqn::NoteAppState* state)
{
    auto& session = state->session;
    const auto& items = session.persisted.remote.items;
    const wqn::StoredNoteSessionItem& item = items[state->note_list_selected];
    wqn::DurableNoteObservation pending;
    pending.session_id = session.persisted.remote.session_id;
    pending.sequence = session.persisted.remote.next_sequence;
    pending.item_id = item.item_id;
    pending.action = ObservationAction::kOpened;
    pending.mode = session.persisted.remote.mode;
    pending.next_sequence = session.persisted.remote.next_sequence + 1;
    pending.next_position = static_cast<uint32_t>(state->note_list_selected);
    session.pending_observation = std::move(pending);
    session.observation_effect_ready = true;
    session.commit_state = wqn::NoteObservationCommitState::kPersisting;
}

void HandleNoteListInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    const auto& items = state->session.persisted.remote.items;
    switch (input) {
        case wqn::NoteInput::kUp:
            if (state->note_list_selected > 0) --state->note_list_selected;
            break;
        case wqn::NoteInput::kDown:
            if (!items.empty() && state->note_list_selected + 1 < items.size()) {
                ++state->note_list_selected;
            }
            RequestNoteCandidatePageIfNeeded(state);
            break;
        case wqn::NoteInput::kConfirm: {
            if (items.empty() || state->note_list_selected >= items.size()) {
                state->message = "该笔记本暂无笔记";
                break;
            }
            if (state->session.commit_state ==
                wqn::NoteObservationCommitState::kPersisting) {
                // A previous open is still committing; ignore the brief window.
                break;
            }
            // Record the explicit gate-0 `opened` action, then show the note.
            ArmOpenObservation(state);
            LoadCurrentNoteBody(state);
            state->note_scroll_offset_lines = 0;
            state->mode = wqn::NoteAppMode::kNoteView;
            state->message = state->current_note_loaded ? "阅读中" : "内容未同步";
            break;
        }
        case wqn::NoteInput::kLongConfirm:
            state->current_note = wqn::WqnNoteEntry{};
            state->current_note_loaded = false;
            state->mode = wqn::NoteAppMode::kNotebookList;
            state->message.clear();
            break;
    }
}

void HandleNoteViewInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    switch (input) {
        case wqn::NoteInput::kUp:
            if (state->note_scroll_offset_lines > 0) --state->note_scroll_offset_lines;
            break;
        case wqn::NoteInput::kDown:
        case wqn::NoteInput::kConfirm:
            // The renderer clamps window_top to the wrapped line count; a soft
            // cap here keeps the offset from drifting far past the body.
            if (state->note_scroll_offset_lines < 4096) ++state->note_scroll_offset_lines;
            break;
        case wqn::NoteInput::kLongConfirm:
            state->current_note = wqn::WqnNoteEntry{};
            state->current_note_loaded = false;
            state->note_scroll_offset_lines = 0;
            state->mode = wqn::NoteAppMode::kNoteList;
            state->message.clear();
            break;
    }
}

}  // namespace

namespace wqn {

esp_err_t InitNoteApp(NoteAppState* state)
{
    if (state == nullptr) return ESP_ERR_INVALID_ARG;
    if (state->initialized) return ESP_OK;

    state->mode = NoteAppMode::kNotebookList;
    state->message = "笔记同步中";

    const esp_err_t storage_result = InitNotePackStorage();
    if (storage_result != ESP_OK) {
        state->message = "笔记分区不可用";
    } else {
        NotePackIndex index;
        const esp_err_t index_result = LoadNotePackIndex(&index);
        const std::string index_message = index.status_message;
        ApplyNotePackIndex(state, std::move(index), index_message);
        if (index_result != ESP_OK) {
            ESP_LOGW(kTag, "load local note pack index failed: %s", esp_err_to_name(index_result));
        }
    }

    NoteOutboxSnapshot outbox;
    if (ReadNoteOutboxSnapshot(&outbox) == ESP_OK) {
        state->outbox.pending_count = outbox.pending_count;
        state->outbox.capacity = outbox.capacity;
    }

    PersistedNoteSession persisted;
    const esp_err_t session_result = LoadPersistedNoteSession(&persisted);
    if (session_result == ESP_OK && persisted.active && !persisted.paused &&
        !persisted.remote.items.empty()) {
        state->session.persisted = std::move(persisted);
        state->note_list_selected = std::min<size_t>(
            state->session.persisted.position,
            state->session.persisted.remote.items.size() - 1);
        const size_t row = FindNotebookRow(*state, state->session.persisted.remote.notebook_id);
        if (row != static_cast<size_t>(-1)) state->notebook_selected = row;
        state->mode = NoteAppMode::kNoteList;
        state->message = "已恢复上次浏览";
        RequestNoteCandidatePageIfNeeded(state);
    } else if (session_result != ESP_OK && session_result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "load persisted note session failed: %s", esp_err_to_name(session_result));
    }

    state->initialized = true;
    state->cloud_sync_requested = !state->cloud_loaded_once || state->pack_index.pack_error;
    return ESP_OK;
}

esp_err_t HandleNoteAppInput(NoteAppState* state, NoteInput input)
{
    if (state == nullptr) return ESP_ERR_INVALID_ARG;
    if (!state->initialized) {
        ESP_RETURN_ON_ERROR(InitNoteApp(state), kTag, "init note app");
    }
    ActivatePendingNotePackIndex(state);
    switch (state->mode) {
        case NoteAppMode::kNotebookList:
            HandleNotebookListInput(state, input);
            break;
        case NoteAppMode::kSessionStarting:
            HandleSessionStartingInput(state, input);
            break;
        case NoteAppMode::kNoteList:
            HandleNoteListInput(state, input);
            break;
        case NoteAppMode::kNoteView:
            HandleNoteViewInput(state, input);
            break;
    }
    return ESP_OK;
}

void ApplyNotePackIndex(NoteAppState* state, NotePackIndex index, const std::string& message)
{
    if (state == nullptr) return;
    // A refresh while browsing stages for the next return to the notebook list.
    if (state->mode != NoteAppMode::kNotebookList ||
        (state->session.persisted.active && !state->session.persisted.paused)) {
        state->pending_pack_index = std::move(index);
        state->pending_pack_index_ready = true;
        return;
    }
    InstallNotePackIndex(state, std::move(index), message);
}

bool TakeNoteSessionStartRequest(
    NoteAppState* state,
    protocol::note_study_v1::CreateSessionRequest* request)
{
    if (state == nullptr || request == nullptr || !state->session.start_requested ||
        state->session.requested_notebook_id.empty()) {
        return false;
    }
    if (state->session.create_request_id.empty()) {
        state->session.create_request_id = request->metadata.request_id;
    } else {
        request->metadata.request_id = state->session.create_request_id;
    }
    // Device v1 only browses by least-recently-viewed (mode=recent).
    request->mode = Mode::kRecent;
    request->scope = {};
    request->scope.notebook_ids.push_back(state->session.requested_notebook_id);
    request->scope.include_archived = false;
    request->optional_count = 500;
    request->seed.clear();
    state->session.start_requested = false;
    state->session.start_result_expected = true;
    return true;
}

bool ApplyNoteSessionStartResult(
    NoteAppState* state,
    esp_err_t result,
    protocol::note_study_v1::SessionData session)
{
    if (state == nullptr || !state->session.start_result_expected) return false;
    state->session.start_result_expected = false;
    if (result != ESP_OK) {
        state->mode = NoteAppMode::kNotebookList;
        state->message = result == ESP_ERR_INVALID_STATE
            ? "请先完成配对"
            : "打开失败，可重试";
        return true;
    }
    PersistedNoteSession persisted;
    persisted.active = !session.items.empty();
    persisted.paused = false;
    persisted.position = 0;
    result = CompactNoteSessionData(session, &persisted.remote);
    if (result != ESP_OK) {
        state->mode = NoteAppMode::kNotebookList;
        state->message = "会话数据过大";
        return true;
    }
    result = SavePersistedNoteSession(persisted);
    if (result != ESP_OK) {
        state->mode = NoteAppMode::kNotebookList;
        state->message = "会话未保存，请重试";
        return true;
    }
    state->session.persisted = std::move(persisted);
    state->session.commit_state = NoteObservationCommitState::kIdle;
    state->session.page_in_flight = false;
    state->session.page_requested = false;
    state->session.create_request_id.clear();
    state->note_list_selected = 0;
    state->mode = NoteAppMode::kNoteList;
    state->message = state->session.persisted.remote.items.empty()
        ? "该笔记本暂无笔记"
        : "选择要看的笔记";
    RequestNoteCandidatePageIfNeeded(state);
    return true;
}

void CancelNoteSessionStartResult(NoteAppState* state)
{
    if (state == nullptr) return;
    state->session.start_requested = false;
    state->session.start_result_expected = false;
    state->session.create_request_id.clear();
}

void ResetNoteSessionForServerInvalid(NoteAppState* state)
{
    if (state == nullptr) return;
    // The server rejected this session as unusable; reusing the same session_id
    // cannot repair it. Drop the durable record and return to the notebook list
    // so re-opening creates a fresh session. The pack index stays mounted.
    ESP_ERROR_CHECK_WITHOUT_ABORT(ClearPersistedNoteSession());
    state->session = NoteSessionState{};
    state->note_list_selected = 0;
    state->note_scroll_offset_lines = 0;
    state->current_note = WqnNoteEntry{};
    state->current_note_loaded = false;
    state->mode = NoteAppMode::kNotebookList;
    state->message = "会话已重置，请重新打开";
}

bool TakeNoteCandidatePageRequest(
    NoteAppState* state,
    protocol::note_study_v1::CandidatePageRequest* request,
    std::string* session_id)
{
    if (state == nullptr || request == nullptr || session_id == nullptr ||
        !state->session.page_requested || state->session.page_in_flight ||
        !state->session.persisted.active || state->session.persisted.paused ||
        !state->session.persisted.remote.has_more ||
        state->session.persisted.remote.cursor.empty()) {
        return false;
    }
    request->cursor = state->session.persisted.remote.cursor;
    request->limit = static_cast<int>(protocol::note_study_v1::kCandidatePrefetchPageSize);
    *session_id = state->session.persisted.remote.session_id;
    state->session.page_requested = false;
    state->session.page_in_flight = true;
    return true;
}

void RestoreNoteCandidatePageRequest(NoteAppState* state)
{
    if (state == nullptr) return;
    state->session.page_in_flight = false;
    if (state->session.persisted.active && !state->session.persisted.paused &&
        state->session.persisted.remote.has_more) {
        state->session.page_requested = true;
    }
}

void ApplyNoteCandidatePageResult(
    NoteAppState* state,
    esp_err_t result,
    protocol::note_study_v1::CandidatePageData page)
{
    if (state == nullptr) return;
    state->session.page_in_flight = false;
    auto& persisted = state->session.persisted;
    auto& remote = persisted.remote;
    if (!persisted.active || persisted.paused) return;
    if (result != ESP_OK) {
        state->session.page_requested = false;
        state->message = "后续笔记加载失败，继续时重试";
        return;
    }
    if (page.session_id != remote.session_id || page.ordering != remote.ordering ||
        page.candidate_policy_version !=
            protocol::note_study_v1::CandidatePolicyVersionName(remote.ordering) ||
        page.seed != remote.seed || page.progress_revision != remote.progress_revision ||
        page.cursor != remote.cursor || !SnapshotMatches(remote, page)) {
        state->message = "后续笔记快照不一致";
        return;
    }
    // Note browsing is a free list (no card cursor), so pages append without
    // trimming; the total stays bounded by the session candidate cap.
    uint64_t expected_ordinal =
        remote.items.empty() ? (page.items.empty() ? 0 : page.items.front().ordinal)
                             : remote.items.back().ordinal + 1;
    if (remote.items.size() + page.items.size() >
        protocol::note_study_v1::kMaxSessionItems) {
        state->message = "候选窗口超限";
        return;
    }
    PersistedNoteSession updated = persisted;
    for (const auto& source : page.items) {
        if (source.ordinal != expected_ordinal || source.item_id.size() != 36 ||
            source.notebook_id.size() != 36) {
            state->message = "候选页顺序无效";
            return;
        }
        StoredNoteSessionItem item;
        std::snprintf(item.item_id, sizeof(item.item_id), "%s", source.item_id.c_str());
        std::snprintf(item.notebook_id, sizeof(item.notebook_id), "%s", source.notebook_id.c_str());
        item.ordinal = source.ordinal;
        std::snprintf(
            item.last_opened_at, sizeof(item.last_opened_at), "%s", source.last_opened_at.c_str());
        updated.remote.items.push_back(item);
        ++expected_ordinal;
    }
    updated.remote.cursor = page.next_cursor;
    updated.remote.has_more = page.has_more;
    if (SavePersistedNoteSession(updated) != ESP_OK) {
        state->message = "后续笔记未保存";
        return;
    }
    persisted = std::move(updated);
    state->message = "后续笔记已就绪";
    RequestNoteCandidatePageIfNeeded(state);
}

bool TakeNoteObservationEffect(
    NoteAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    DurableNoteObservation* observation,
    PersistedNoteSession* advanced_session)
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
    PersistedNoteSession advanced = state->session.persisted;
    if (pending.session_id != advanced.remote.session_id ||
        pending.sequence != advanced.remote.next_sequence ||
        pending.next_position > advanced.remote.items.size()) {
        state->session.observation_effect_ready = false;
        state->session.commit_state = NoteObservationCommitState::kFailed;
        state->message = "会话游标无效";
        return false;
    }
    advanced.remote.next_sequence = pending.sequence + 1;
    advanced.position = pending.next_position;
    state->session.pending_advanced_session = advanced;
    state->session.observation_effect_ready = false;
    *observation = pending;
    *advanced_session = std::move(advanced);
    return true;
}

void ApplyNoteObservationCommitResult(NoteAppState* state, esp_err_t result)
{
    if (state == nullptr) return;
    if (result != ESP_OK) {
        state->session.commit_state = NoteObservationCommitState::kFailed;
        state->message = result == ESP_ERR_NO_MEM ? "记录空间已满" : "记录未保存";
        return;
    }
    state->session.persisted = std::move(state->session.pending_advanced_session);
    state->session.pending_advanced_session = {};
    state->session.pending_observation = {};
    state->session.commit_state = NoteObservationCommitState::kCloudPending;
    if (state->outbox.pending_count < state->outbox.capacity) {
        ++state->outbox.pending_count;
    }
}

void RefreshNoteOutboxState(NoteAppState* state)
{
    if (state == nullptr) return;
    NoteOutboxSnapshot snapshot;
    if (ReadNoteOutboxSnapshot(&snapshot) != ESP_OK) return;
    state->outbox.pending_count = snapshot.pending_count;
    state->outbox.capacity = snapshot.capacity;
    if (snapshot.pending_count == 0 &&
        state->session.commit_state == NoteObservationCommitState::kCloudPending) {
        state->session.commit_state = NoteObservationCommitState::kCloudAcknowledged;
    }
}

NoteAppSnapshot BuildNoteAppSnapshot(const NoteAppState& state)
{
    NoteAppSnapshot snapshot;
    snapshot.mode = state.mode;
    snapshot.notebook_selected = state.notebook_selected;
    snapshot.note_list_selected = state.note_list_selected;
    snapshot.note_scroll_offset_lines = state.note_scroll_offset_lines;
    snapshot.cloud_sync_failed = state.cloud_sync_failed;
    snapshot.notebook_count = state.pack_index.notebooks.size();
    snapshot.notebooks.reserve(state.pack_index.notebooks.size());
    for (const NotePackNotebook& notebook : state.pack_index.notebooks) {
        NoteNotebookRow row;
        row.title = notebook.title;
        row.note_count = notebook.entry_count;
        row.has_pack = notebook.has_pack;
        snapshot.notebooks.push_back(std::move(row));
    }

    if (state.mode == NoteAppMode::kNoteList || state.mode == NoteAppMode::kNoteView) {
        const std::string& notebook_id = state.session.persisted.remote.notebook_id;
        const size_t row = FindNotebookRow(state, notebook_id);
        if (row != static_cast<size_t>(-1)) {
            snapshot.notebook_title = state.pack_index.notebooks[row].title;
        }
        const auto& items = state.session.persisted.remote.items;
        snapshot.titles.reserve(items.size());
        for (const StoredNoteSessionItem& item : items) {
            NoteTitleRow title_row;
            const NotePackIndexEntry* entry = FindPackEntry(state, item.notebook_id, item.item_id);
            title_row.title = entry != nullptr ? entry->title : std::string("(未同步)");
            title_row.last_opened_at = item.last_opened_at;
            snapshot.titles.push_back(std::move(title_row));
        }
    }
    if (state.mode == NoteAppMode::kNoteView) {
        snapshot.has_body = state.current_note_loaded;
        snapshot.note_title = state.current_note.title;
        snapshot.note_body = state.current_note.content;
        snapshot.note_id = state.current_note.note_id;
    }
    snapshot.status_line = NoteAppStatusLine(state);
    switch (state.mode) {
        case NoteAppMode::kNotebookList:
            snapshot.hint = "确认进入 · 长按返回";
            break;
        case NoteAppMode::kSessionStarting:
            snapshot.hint = "长按取消";
            break;
        case NoteAppMode::kNoteList:
            snapshot.hint = "确认打开 · 长按返回";
            break;
        case NoteAppMode::kNoteView:
            snapshot.hint = "上下滚动 · 长按返回";
            break;
    }
    return snapshot;
}

std::string NoteAppStatusLine(const NoteAppState& state)
{
    if (!state.message.empty()) return state.message;
    switch (state.mode) {
        case NoteAppMode::kNotebookList:
            return state.pack_index.notebooks.empty() ? "笔记未同步" : "选择笔记本";
        case NoteAppMode::kSessionStarting:
            return "正在打开笔记本";
        case NoteAppMode::kNoteList:
            return "选择要看的笔记";
        case NoteAppMode::kNoteView:
            return state.current_note_loaded ? "阅读中" : "内容未同步";
    }
    return "";
}

std::string NoteAppSignature(const NoteAppState& state)
{
    // Compact identity for the render layer to detect meaningful frame changes.
    char buffer[96] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u:%u:%u:%u:%u:%u",
        static_cast<unsigned>(state.mode),
        static_cast<unsigned>(state.notebook_selected),
        static_cast<unsigned>(state.note_list_selected),
        static_cast<unsigned>(state.note_scroll_offset_lines),
        static_cast<unsigned>(state.pack_index.notebooks.size()),
        static_cast<unsigned>(state.session.commit_state));
    return std::string(buffer);
}

namespace {

struct NotePageFixture {
    NoteAppState* state = nullptr;
    NotePageFixture() : state(new (std::nothrow) NoteAppState()) {}
    ~NotePageFixture() { delete state; }
    explicit operator bool() const { return state != nullptr; }
    NoteAppState& get() { return *state; }
};

StoredNoteSessionItem MakeItem(unsigned index)
{
    StoredNoteSessionItem item;
    std::snprintf(item.item_id, sizeof(item.item_id), "00000000-0000-4000-8000-%012u", index + 100);
    std::snprintf(item.notebook_id, sizeof(item.notebook_id), "%s", "11111111-1111-4111-8111-111111111111");
    item.ordinal = index;
    return item;
}

}  // namespace

bool RunNotePageStateSelfTest()
{
    auto require = [](bool condition, const char* label) {
        if (!condition) ESP_LOGE(kTag, "note page self-test failed: %s", label);
        return condition;
    };

    // Notebook-list navigation never escapes its bounds with an empty index.
    NotePageFixture home;
    if (!home) return require(false, "allocate home fixture");
    home.get().initialized = true;
    home.get().mode = NoteAppMode::kNotebookList;
    for (size_t index = 0; index < 500; ++index) {
        const NoteInput input = (index % 2 == 0) ? NoteInput::kDown : NoteInput::kUp;
        if (HandleNoteAppInput(&home.get(), input) != ESP_OK) return false;
    }
    if (!require(home.get().mode == NoteAppMode::kNotebookList, "empty notebook list stays put") ||
        !require(home.get().notebook_selected == 0, "empty list selection pinned")) {
        return false;
    }

    // Opening a title arms exactly one durable `opened` and enters the reader.
    NotePageFixture browse;
    if (!browse) return require(false, "allocate browse fixture");
    NoteAppState& b = browse.get();
    b.initialized = true;
    b.mode = NoteAppMode::kNoteList;
    b.session.persisted.active = true;
    b.session.persisted.remote.session_id = "00000000-0000-4000-8000-000000000001";
    b.session.persisted.remote.mode = Mode::kRecent;
    b.session.persisted.remote.next_sequence = 5;
    constexpr size_t kItemCount = 40;
    b.session.persisted.remote.items.reserve(kItemCount);
    for (size_t index = 0; index < kItemCount; ++index) {
        b.session.persisted.remote.items.push_back(MakeItem(static_cast<unsigned>(index)));
    }
    if (HandleNoteAppInput(&b, NoteInput::kConfirm) != ESP_OK ||
        !require(b.mode == NoteAppMode::kNoteView, "confirm opens the reader") ||
        !require(b.session.commit_state == NoteObservationCommitState::kPersisting,
                 "open arms a durable commit") ||
        !require(b.session.observation_effect_ready, "open arms observation effect") ||
        !require(b.session.pending_observation.action == ObservationAction::kOpened,
                 "open records opened") ||
        !require(b.session.pending_observation.sequence == 5, "open uses next_sequence")) {
        return false;
    }

    const DurableNoteObservation pending = b.session.pending_observation;
    for (size_t index = 0; index < 500; ++index) {
        const NoteInput input = index % 4 == 0
            ? NoteInput::kConfirm
            : (index % 4 == 1 ? NoteInput::kDown
                              : (index % 4 == 2 ? NoteInput::kUp : NoteInput::kLongConfirm));
        if (HandleNoteAppInput(&b, input) != ESP_OK) return false;
    }
    if (!require(b.session.pending_observation.sequence == pending.sequence &&
                     b.session.pending_observation.item_id == pending.item_id,
                 "persisting keeps one observation")) {
        return false;
    }

    // The effect handoff produces a valid advanced session and reconciles it.
    NotePageFixture effect;
    if (!effect) return require(false, "allocate effect fixture");
    NoteAppState& e = effect.get();
    e.initialized = true;
    e.mode = NoteAppMode::kNoteList;
    e.session.persisted.active = true;
    e.session.persisted.remote.session_id = "00000000-0000-4000-8000-000000000002";
    e.session.persisted.remote.mode = Mode::kRecent;
    e.session.persisted.remote.next_sequence = 0;
    e.session.persisted.remote.items.push_back(MakeItem(0));
    if (HandleNoteAppInput(&e, NoteInput::kConfirm) != ESP_OK) return false;
    DurableNoteObservation observation;
    PersistedNoteSession advanced;
    if (!require(
            TakeNoteObservationEffect(
                &e, "req_note_selftest_0001", "2026-07-20T00:00:00.000Z", &observation, &advanced),
            "take observation effect") ||
        !require(advanced.remote.next_sequence == 1, "advanced sequence increments") ||
        !require(observation.action == ObservationAction::kOpened, "effect action opened")) {
        return false;
    }
    ApplyNoteObservationCommitResult(&e, ESP_OK);
    if (!require(e.session.commit_state == NoteObservationCommitState::kCloudPending,
                 "commit result moves to cloud pending") ||
        !require(e.session.persisted.remote.next_sequence == 1, "commit advances session")) {
        return false;
    }
    return true;
}

}  // namespace wqn
