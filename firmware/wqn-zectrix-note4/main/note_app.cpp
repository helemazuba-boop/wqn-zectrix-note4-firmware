#include "note_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display_service.h"

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

// Visible rows on the notebook/title lists; mirrors page_note.cpp geometry
// ((kHintY - 4 - kContentTop) / kListRowH = (284 - 4 - 32) / 30 = 8).
constexpr size_t kNoteListVisibleRows = 8;

// Edge-triggered viewport: the window moves only when the selection walks off
// an edge, and then jumps so the selection lands on the opposite edge. The
// previous centered window (start = selected - visible/2) shifted the whole
// window one row on EVERY step past row visible/2, repainting all 8 rows as a
// heavy full-frame partial per step -- the 4th<->5th row stutter and the long
// BUSY stretches. With this rule, in-window steps flip just two rows (fast
// local partial) and sustained scrolling costs one whole-window repaint per 8
// steps in either direction.
void UpdateListViewport(size_t selected, size_t count, size_t* start)
{
    constexpr size_t visible = kNoteListVisibleRows;
    if (count <= visible) {
        *start = 0;
        return;
    }
    const size_t max_start = count - visible;
    if (*start > max_start) *start = max_start;
    if (selected < *start) {
        // Walked off the top: park the selection on the bottom row so the next
        // 7 up-steps stay in-window.
        *start = selected + 1 >= visible ? selected + 1 - visible : 0;
    } else if (selected >= *start + visible) {
        // Walked off the bottom: park the selection on the top row.
        *start = std::min(selected, max_start);
    }
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

// [transfer-progress] Dispatch identity for the runner-side progress mailbox.
// UI task only; 0 is reserved for "no dispatch".
uint32_t NextTransferProgressGeneration()
{
    static uint32_t counter = 0;
    if (++counter == 0) {
        ++counter;
    }
    return counter;
}

// [body-fetch] Arms the targeted single-notebook pack fetch for an opened
// note whose pack is missing on disk; the pump dispatches it on the
// interactive lane. Clears a previous terminal error so Confirm retries.
void ArmNoteBodyFetch(wqn::NoteAppState* state, const char* notebook_id)
{
    const size_t length = notebook_id != nullptr ? std::strlen(notebook_id) : 0;
    if (length != 36) return;
    state->body_fetch_notebook_id.assign(notebook_id, length);
    state->body_fetch_request = true;
    state->body_fetch_error = false;
    // [progress-fix] Show the empty bar the moment the request is armed (the
    // pump may sit behind a busy domain for a while; the user pressed a
    // button NOW and the bar is the acknowledgement).
    state->transfer_progress_bucket = 0;
    state->transfer_progress_repaint_us = esp_timer_get_time();
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
    if (entry == nullptr) {
        // [note-image-diag] The session references a note the mounted index
        // cannot resolve -- typically a stale index after packs changed on disk.
        // An EMPTY index is not an anomaly (self-test / pre-mount), keep quiet.
        if (!state->pack_index.entries.empty()) {
            ESP_LOGW(
                "note_app", "note open unresolved: id=%.8s not in pack index",
                item.item_id);
        }
        return;
    }
    const esp_err_t read_result = wqn::ReadNotePackEntry(*entry, &state->current_note);
    if (read_result == ESP_OK) {
        state->current_note_loaded = true;
        // [note-image-diag] Settles "UP does nothing": images only exist for
        // the viewer if the pack line carried ids.
        ESP_LOGI(
            "note_app", "note opened: id=%.8s images=%u body_bytes=%u",
            state->current_note.note_id.c_str(),
            static_cast<unsigned>(state->current_note.image_ids.size()),
            static_cast<unsigned>(state->current_note.content.size()));
        // Precompute the wrapped line count with the SAME width as RenderNoteBody
        // (ui/page_note.cpp: kContentW - 14 = kEpdWidth - 30 = 370 px) so the
        // scroll handler can clamp Down to the last page.
        state->note_body_total_lines = static_cast<uint32_t>(
            wqn::WrapUtf8TextToWidth(state->current_note.content, 370, 4096).size());
    } else {
        ESP_LOGW(
            "note_app", "note open read failed: id=%.8s err=%s",
            item.item_id, esp_err_to_name(read_result));
        state->note_body_total_lines = 0;
    }
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
    // Eagerly pull the rest of the notebook's candidates (one page per pump,
    // chained via ApplyNoteCandidatePageResult) so the title list can always
    // scroll to the very end. The old threshold-based prefetch only started when
    // the selection was within a few rows of the loaded end, so scrolling faster
    // than the network round-trip left the list stuck before the last note
    // ("无法翻到底"). Bounded by the server's has_more and kMaxSessionItems.
    state->session.page_requested = true;
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
    // The list mixes notebooks with the [题] problem-set rows and the [词]
    // word-deck rows appended after them; navigation spans all three
    // segments, Confirm dispatches on the row kind.
    const size_t notebook_count = state->pack_index.notebooks.size();
    const size_t problem_count = state->problem_sets.size();
    const size_t count = notebook_count + problem_count + state->word_decks.size();
    switch (input) {
        case wqn::NoteInput::kUp:
            if (state->notebook_selected > 0) {
                --state->notebook_selected;
                state->message.clear();
            } else {
                state->message = "已到顶部";
            }
            UpdateListViewport(state->notebook_selected, count, &state->notebook_window_start);
            break;
        case wqn::NoteInput::kDown:
            if (count > 0 && state->notebook_selected + 1 < count) {
                ++state->notebook_selected;
                state->message.clear();
            } else {
                state->message = "已到底部";
            }
            UpdateListViewport(state->notebook_selected, count, &state->notebook_window_start);
            break;
        case wqn::NoteInput::kConfirm: {
            if (count == 0 || state->notebook_selected >= count) {
                state->message = "暂无笔记本，稍后同步";
                break;
            }
            if (state->notebook_selected >= notebook_count + problem_count) {
                const wqn::NoteWordDeckRow& row = state->word_decks
                    [state->notebook_selected - notebook_count - problem_count];
                state->word_deck_open_requested = true;
                state->requested_word_deck_id = row.deck_id;
                state->message.clear();
                break;
            }
            if (state->notebook_selected >= notebook_count) {
                const wqn::NoteProblemSetRow& row =
                    state->problem_sets[state->notebook_selected - notebook_count];
                state->problem_set_open_requested = true;
                state->requested_problem_set_id = row.set_id;
                state->message.clear();
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

void HandleNoteListInput(wqn::NoteAppState* state, wqn::NoteInput input);
void ResetNoteImageViewer(wqn::NoteAppState* state);

void HandleNoteListInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    const auto& items = state->session.persisted.remote.items;
    switch (input) {
        case wqn::NoteInput::kUp:
            if (state->note_list_selected > 0) {
                --state->note_list_selected;
                state->message.clear();
            } else {
                state->message = "已到顶部";
            }
            UpdateListViewport(state->note_list_selected, items.size(),
                               &state->note_list_window_start);
            break;
        case wqn::NoteInput::kDown:
            if (!items.empty() && state->note_list_selected + 1 < items.size()) {
                ++state->note_list_selected;
                state->message.clear();
            } else {
                // [note-scroll-diag] Down could not advance -- either the real
                // bottom (has_more=0) or waiting on a prefetch (has_more=1).
                ESP_LOGW(
                    kTag,
                    "note list bottom: selected=%u candidates=%u has_more=%d",
                    static_cast<unsigned>(state->note_list_selected),
                    static_cast<unsigned>(items.size()),
                    state->session.persisted.remote.has_more ? 1 : 0);
                state->message = state->session.persisted.remote.has_more
                    ? "正在加载更多…"
                    : "已到底部";
            }
            UpdateListViewport(state->note_list_selected, items.size(),
                               &state->note_list_window_start);
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
            LoadCurrentNoteBody(state);
            if (state->current_note_loaded) {
                // Record the explicit gate-0 `opened` action, then show it.
                ArmOpenObservation(state);
                state->note_scroll_offset_lines = 0;
                ResetNoteImageViewer(state);
                state->mode = wqn::NoteAppMode::kNoteView;
                state->message = "阅读中";
                break;
            }
            // [status-bar-progress] Content not on disk: STAY ON THE LIST so
            // the user keeps browsing while the targeted single-notebook sync
            // runs behind the status-bar progress strip. The open intent is
            // recorded and honored when the pack lands (item_id must still be
            // selected); the opened-observation is deferred until the note is
            // actually shown.
            ArmNoteBodyFetch(
                state, items[state->note_list_selected].notebook_id);
            state->body_fetch_pending_open = true;
            state->body_fetch_pending_item_id =
                items[state->note_list_selected].item_id;
            state->message = "正在同步内容";
            break;
        }
        case wqn::NoteInput::kLongConfirm:
            state->current_note = wqn::WqnNoteEntry{};
            state->current_note_loaded = false;
            // Leaving the list abandons the auto-open intent; an in-flight
            // fetch still lands in the index for the next visit.
            state->body_fetch_pending_open = false;
            state->body_fetch_request = false;
            state->body_fetch_error = false;
            state->mode = wqn::NoteAppMode::kNotebookList;
            state->message.clear();
            break;
    }
}

void HandleNoteViewInput(wqn::NoteAppState* state, wqn::NoteInput input);

// Clears the image viewer whenever the underlying note changes or the body
// view is left for good; the loaded payload is kept while merely toggling
// between body and image so re-entry is instant.
void ResetNoteImageViewer(wqn::NoteAppState* state)
{
    state->image_index = 0;
    state->image_request = false;
    state->image_in_flight = false;
    state->image_error = false;
    state->image_expected_id.clear();
    state->image_dispatched_id.clear();
    state->image_loaded_id.clear();
    state->image_wqni.reset();
    state->image_dispatch_us = 0;
}

// Points the viewer at current_note.image_ids[image_index]: cache hit on the
// in-memory payload is immediate; otherwise the pump hands the id to the note
// cloud task (SPIFFS cache first, then download).
void RequestCurrentNoteImage(wqn::NoteAppState* state)
{
    const auto& ids = state->current_note.image_ids;
    if (state->image_index >= ids.size()) {
        state->image_error = true;
        return;
    }
    const std::string& id = ids[state->image_index];
    state->image_expected_id = id;
    state->image_error = false;
    if (state->image_loaded_id == id && state->image_wqni != nullptr) {
        state->image_request = false;
        return;
    }
    if (!state->image_in_flight) {
        state->image_request = true;
    }
    // [progress-fix] The bar appears at request time (empty), not at first
    // byte: the flip itself is what the user wants acknowledged.
    if (state->transfer_progress_bucket < 0) {
        state->transfer_progress_bucket = 0;
        state->transfer_progress_repaint_us = esp_timer_get_time();
    }
}

// The note is one vertical ring: [image 1 .. image N] above the body, ends
// joined. UP always moves up, DOWN always moves down, both wrap around --
// no dead ends, and leaving the image layer needs no long-press.

uint32_t NoteBodyMaxScroll(const wqn::NoteAppState& state)
{
    // RenderNoteBody shows 13 body lines (kContentTop=32, no footer line) or
    // 12 when the image entry line occupies the first row; clamping to the
    // last page keeps over-scroll from inflating the offset.
    const uint32_t visible_lines =
        state.current_note.image_ids.empty() ? 13 : 12;
    return state.note_body_total_lines > visible_lines
        ? state.note_body_total_lines - visible_lines
        : 0;
}

void EnterNoteImageView(wqn::NoteAppState* state, uint8_t index)
{
    state->mode = wqn::NoteAppMode::kNoteImageView;
    state->image_index = index;
    state->image_view_entered_us = esp_timer_get_time();
    RequestCurrentNoteImage(state);
    state->message.clear();
}

void ExitNoteImageViewToBody(wqn::NoteAppState* state, uint32_t scroll_lines)
{
    // Keep the loaded payload for instant re-entry (same as the long-press
    // exit); only the transient request/error state is cleared.
    state->mode = wqn::NoteAppMode::kNoteView;
    state->note_scroll_offset_lines = scroll_lines;
    state->image_request = false;
    state->image_error = false;
    state->message.clear();
}

void HandleNoteViewInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    if (!state->current_note_loaded) {
        // [body-fetch] Placeholder page (syncing / failed / missing). Confirm
        // retries a failed or lost fetch; long-confirm backs out and cancels
        // the wait; scroll inputs are no-ops on placeholder text.
        switch (input) {
            case wqn::NoteInput::kConfirm:
                if (!state->body_fetch_request && !state->body_fetch_in_flight) {
                    const auto& items = state->session.persisted.remote.items;
                    if (state->note_list_selected < items.size()) {
                        ArmNoteBodyFetch(
                            state,
                            items[state->note_list_selected].notebook_id);
                        state->message = "正在同步内容";
                    }
                }
                break;
            case wqn::NoteInput::kLongConfirm:
                state->body_fetch_request = false;
                state->body_fetch_error = false;
                // in_flight stays: a live fetch result still installs the
                // fresher index; the mode guard keeps view state untouched.
                state->current_note = wqn::WqnNoteEntry{};
                state->current_note_loaded = false;
                state->note_scroll_offset_lines = 0;
                ResetNoteImageViewer(state);
                state->mode = wqn::NoteAppMode::kNoteList;
                state->message.clear();
                break;
            default:
                break;
        }
        return;
    }
    switch (input) {
        case wqn::NoteInput::kUp: {
            constexpr uint32_t kNoteBodyScrollStep = 4;
            if (state->note_scroll_offset_lines == 0) {
                if (!state->current_note.image_ids.empty()) {
                    // Up from the body top reaches the image directly above
                    // it: the LAST attachment.
                    EnterNoteImageView(
                        state,
                        static_cast<uint8_t>(
                            state->current_note.image_ids.size() - 1));
                } else {
                    // No images: the ring is just the body; wrap to its end.
                    state->note_scroll_offset_lines = NoteBodyMaxScroll(*state);
                    state->message.clear();
                }
            } else {
                state->note_scroll_offset_lines =
                    state->note_scroll_offset_lines > kNoteBodyScrollStep
                        ? state->note_scroll_offset_lines - kNoteBodyScrollStep
                        : 0;
                state->message.clear();
            }
            break;
        }
        case wqn::NoteInput::kDown:
        case wqn::NoteInput::kConfirm: {
            constexpr uint32_t kNoteBodyScrollStep = 4;
            const uint32_t max_scroll = NoteBodyMaxScroll(*state);
            if (state->note_scroll_offset_lines >= max_scroll) {
                if (!state->current_note.image_ids.empty()) {
                    // Down past the body end wraps around the ring to the
                    // first image at the very top.
                    EnterNoteImageView(state, 0);
                } else {
                    state->note_scroll_offset_lines = 0;
                    state->message.clear();
                }
            } else {
                state->note_scroll_offset_lines = std::min<uint32_t>(
                    state->note_scroll_offset_lines + kNoteBodyScrollStep, max_scroll);
                state->message.clear();
            }
            break;
        }
        case wqn::NoteInput::kLongConfirm:
            state->current_note = wqn::WqnNoteEntry{};
            state->current_note_loaded = false;
            state->note_scroll_offset_lines = 0;
            ResetNoteImageViewer(state);
            state->mode = wqn::NoteAppMode::kNoteList;
            state->message.clear();
            break;
    }
}

void HandleNoteImageViewInput(wqn::NoteAppState* state, wqn::NoteInput input)
{
    const size_t count = state->current_note.image_ids.size();
    switch (input) {
        case wqn::NoteInput::kUp:
            if (state->image_index > 0) {
                --state->image_index;
                state->image_view_entered_us = esp_timer_get_time();
                RequestCurrentNoteImage(state);
            } else {
                // Up past the first image wraps around the ring to the body
                // end.
                ExitNoteImageViewToBody(state, NoteBodyMaxScroll(*state));
            }
            break;
        case wqn::NoteInput::kDown:
        case wqn::NoteInput::kConfirm:
            if (count > 0 && static_cast<size_t>(state->image_index) + 1 < count) {
                ++state->image_index;
                state->image_view_entered_us = esp_timer_get_time();
                RequestCurrentNoteImage(state);
            } else {
                // Down past the last image lands on the body top right below.
                ExitNoteImageViewToBody(state, 0);
            }
            break;
        case wqn::NoteInput::kLongConfirm:
            // Escape hatch kept for muscle memory: straight back to the body.
            state->mode = wqn::NoteAppMode::kNoteView;
            state->image_request = false;
            state->image_error = false;
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
        UpdateListViewport(state->note_list_selected,
                           state->session.persisted.remote.items.size(),
                           &state->note_list_window_start);
        const size_t row = FindNotebookRow(*state, state->session.persisted.remote.notebook_id);
        if (row != static_cast<size_t>(-1)) state->notebook_selected = row;
        UpdateListViewport(state->notebook_selected, state->pack_index.notebooks.size(),
                           &state->notebook_window_start);
        state->mode = NoteAppMode::kNoteList;
        state->message = "已恢复上次浏览";
        RequestNoteCandidatePageIfNeeded(state);
        // [note-scroll-diag] Same visibility for the restored-session path: a
        // stale persisted window with has_more=0 cannot grow, which would cap
        // the title list below the notebook's real note count.
        ESP_LOGI(
            kTag,
            "note session restored: candidates=%u notebook_notes=%u has_more=%d cursor=%s",
            static_cast<unsigned>(state->session.persisted.remote.items.size()),
            row != static_cast<size_t>(-1)
                ? static_cast<unsigned>(state->pack_index.notebooks[row].entry_count)
                : 0u,
            state->session.persisted.remote.has_more ? 1 : 0,
            state->session.persisted.remote.cursor.c_str());
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
        case NoteAppMode::kNoteImageView:
            HandleNoteImageViewInput(state, input);
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

// The mixed list may have shrunk under the selection (rows re-applied after
// a sync or a default-deck change); clamp against the combined row count so
// the highlight stays on-screen.
static void ClampNoteMixedListSelection(NoteAppState* state)
{
    const size_t count = state->pack_index.notebooks.size() +
        state->problem_sets.size() + state->word_decks.size();
    if (count == 0) {
        state->notebook_selected = 0;
        state->notebook_window_start = 0;
    } else if (state->notebook_selected >= count) {
        state->notebook_selected = count - 1;
        UpdateListViewport(state->notebook_selected, count, &state->notebook_window_start);
    }
}

void ApplyNoteProblemSetRows(NoteAppState* state, std::vector<NoteProblemSetRow> rows)
{
    if (state == nullptr) return;
    state->problem_sets = std::move(rows);
    ClampNoteMixedListSelection(state);
}

void ApplyNoteWordDeckRows(NoteAppState* state, std::vector<NoteWordDeckRow> rows)
{
    if (state == nullptr) return;
    state->word_decks = std::move(rows);
    ClampNoteMixedListSelection(state);
}

bool TakeNoteProblemSetOpenRequest(NoteAppState* state, std::string* set_id)
{
    if (state == nullptr || set_id == nullptr) return false;
    if (!state->problem_set_open_requested ||
        state->requested_problem_set_id.size() != 36) {
        // A malformed request cannot be dispatched; drop it so the flag does
        // not re-fire every tick.
        state->problem_set_open_requested = false;
        return false;
    }
    *set_id = state->requested_problem_set_id;
    state->problem_set_open_requested = false;
    state->requested_problem_set_id.clear();
    return true;
}

bool TakeNoteWordDeckOpenRequest(NoteAppState* state, std::string* deck_id)
{
    if (state == nullptr || deck_id == nullptr) return false;
    if (!state->word_deck_open_requested ||
        state->requested_word_deck_id.size() != 36) {
        state->word_deck_open_requested = false;
        return false;
    }
    *deck_id = state->requested_word_deck_id;
    state->word_deck_open_requested = false;
    state->requested_word_deck_id.clear();
    return true;
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
    esp_err_t compact_result,
    esp_err_t persist_result,
    PersistedNoteSession persisted)
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
    if (compact_result != ESP_OK) {
        state->mode = NoteAppMode::kNotebookList;
        state->message = "会话数据过大";
        return true;
    }
    if (persist_result != ESP_OK) {
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
    state->note_list_window_start = 0;
    state->mode = NoteAppMode::kNoteList;
    state->message = state->session.persisted.remote.items.empty()
        ? "该笔记本暂无笔记"
        : "选择要看的笔记";
    RequestNoteCandidatePageIfNeeded(state);
    // [note-scroll-diag] Reveals whether the candidate window is complete on
    // entry. candidates < notebook_notes with has_more=0 means the server
    // returned a partial set (no pages left to pull) -- the likely cause of
    // "can't scroll to the bottom"; has_more=1 means the rest arrive via prefetch.
    {
        const size_t diag_row =
            FindNotebookRow(*state, state->session.persisted.remote.notebook_id);
        const unsigned diag_notebook_notes =
            diag_row != static_cast<size_t>(-1)
                ? static_cast<unsigned>(state->pack_index.notebooks[diag_row].entry_count)
                : 0u;
        ESP_LOGI(
            kTag,
            "note session ready: candidates=%u notebook_notes=%u has_more=%d cursor=%s",
            static_cast<unsigned>(state->session.persisted.remote.items.size()),
            diag_notebook_notes,
            state->session.persisted.remote.has_more ? 1 : 0,
            state->session.persisted.remote.cursor.c_str());
    }
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

// Defined below ApplyNoteImageResult; the pump-side take path uses it to heal
// dropped or mismatched fetch results.
void EnsureNoteImageRequest(NoteAppState* state);

bool TakeNoteImageRequest(
    NoteAppState* state,
    std::string* note_id,
    uint8_t* image_index,
    std::string* image_id,
    uint32_t* progress_generation)
{
    if (state == nullptr || note_id == nullptr || image_index == nullptr ||
        image_id == nullptr || progress_generation == nullptr) {
        return false;
    }
    // Heal a dropped/mismatched result before checking the flag, so the pump
    // retries instead of leaving the viewer on a forever-loading page.
    EnsureNoteImageRequest(state);
    if (!state->image_request || state->image_in_flight ||
        state->mode != NoteAppMode::kNoteImageView ||
        state->image_expected_id.empty() ||
        state->current_note.note_id.size() != 36) {
        return false;
    }
    *note_id = state->current_note.note_id;
    *image_index = state->image_index;
    *image_id = state->image_expected_id;
    state->image_request = false;
    state->image_in_flight = true;
    state->image_dispatch_us = esp_timer_get_time();
    state->image_dispatched_id = *image_id;
    state->transfer_progress_generation = NextTransferProgressGeneration();
    *progress_generation = state->transfer_progress_generation;
    // [progress-fix] Show the empty bar the moment the download is dispatched.
    // HIL: a 15 KB image spends ~1.2 s in TLS setup with zero bytes flowing
    // while the loading frame renders, and the ~600 ms body window hides
    // behind an in-flight full refresh -- waiting for the first byte meant
    // the bar never had a frame to ride on.
    state->transfer_progress_bucket = 0;
    state->transfer_progress_repaint_us = esp_timer_get_time();
    ESP_LOGI(kTag, "note image fetch dispatched: id=%.12s index=%u note=%.8s",
             image_id->c_str(), static_cast<unsigned>(*image_index),
             note_id->c_str());
    return true;
}

void RestoreNoteImageRequest(NoteAppState* state)
{
    if (state == nullptr || !state->image_in_flight) return;
    state->image_in_flight = false;
    state->image_request = true;
}

bool TakeNoteBodyFetchRequest(NoteAppState* state, std::string* notebook_id,
                              uint32_t* progress_generation)
{
    if (state == nullptr || notebook_id == nullptr ||
        progress_generation == nullptr) {
        return false;
    }
    // The list is the home of the waiting user now (confirm on a missing
    // note stays there); kNoteView remains for the defensive placeholder.
    if ((state->mode != NoteAppMode::kNoteList &&
         state->mode != NoteAppMode::kNoteView) ||
        state->current_note_loaded) {
        return false;
    }
    if (state->body_fetch_in_flight) {
        // Outer self-heal, same shape as the image fetch: a dispatch whose
        // result was dropped or wedged past every inner timeout re-arms here
        // instead of leaving the page on "正在同步内容" forever.
        constexpr int64_t kBodyFetchTimeoutUs = 20LL * 1000 * 1000;
        if (state->body_fetch_dispatch_us > 0 &&
            esp_timer_get_time() - state->body_fetch_dispatch_us >=
                kBodyFetchTimeoutUs) {
            ESP_LOGW(kTag, "note body pack fetch timed out; re-arming: nb=%.8s",
                     state->body_fetch_notebook_id.c_str());
            state->body_fetch_in_flight = false;
            state->body_fetch_request = true;
        }
        return false;
    }
    if (!state->body_fetch_request || state->body_fetch_error ||
        state->body_fetch_notebook_id.size() != 36) {
        return false;
    }
    *notebook_id = state->body_fetch_notebook_id;
    state->body_fetch_request = false;
    state->body_fetch_in_flight = true;
    state->body_fetch_dispatch_us = esp_timer_get_time();
    state->transfer_progress_generation = NextTransferProgressGeneration();
    *progress_generation = state->transfer_progress_generation;
    // [progress-fix] Same dispatch-time empty bar as the image path.
    state->transfer_progress_bucket = 0;
    state->transfer_progress_repaint_us = esp_timer_get_time();
    ESP_LOGI(kTag, "note body pack fetch dispatched: nb=%.8s",
             notebook_id->c_str());
    return true;
}

void RestoreNoteBodyFetchRequest(NoteAppState* state)
{
    if (state == nullptr || !state->body_fetch_in_flight) return;
    state->body_fetch_in_flight = false;
    state->body_fetch_request = true;
}

void ApplyNoteBodyFetchResult(
    NoteAppState* state,
    esp_err_t result,
    NotePackIndex index,
    bool index_ready,
    bool auth_required,
    const std::string& fetched_notebook_id,
    const std::string& message)
{
    if (state == nullptr) return;
    state->body_fetch_in_flight = false;
    // [crosswire-fix] This result may belong to a PREVIOUS notebook: the user
    // can re-arm for notebook B while A's fetch is still in flight (browse on,
    // confirm elsewhere). Only a matching result may consume the open intent
    // or flip the error state -- a mismatched one still installs its index
    // (A's pack landed, keep it durable) and then lets the pump dispatch the
    // freshest request (body_fetch_request is still armed for B).
    const bool matches_current =
        !fetched_notebook_id.empty() &&
        fetched_notebook_id == state->body_fetch_notebook_id;
    const bool pending_open = matches_current && state->body_fetch_pending_open;
    if (matches_current) {
        state->body_fetch_pending_open = false;
    }
    if (result == ESP_OK && index_ready) {
        // Install in place, even mid-browse: unlike the full-catalog sync
        // (which staged as pending until the notebook list), this index only
        // advances the notebook the user is actively waiting on.
        InstallNotePackIndex(state, std::move(index), message);
        // Honor the deferred open intent -- but only if the user's selection
        // still points at the note they asked for, and no observation commit
        // is mid-flight. Otherwise the pack simply lands and the list keeps
        // browsing undisturbed.
        if (pending_open && state->mode == NoteAppMode::kNoteList &&
            state->session.commit_state !=
                NoteObservationCommitState::kPersisting) {
            const auto& items = state->session.persisted.remote.items;
            if (state->note_list_selected < items.size() &&
                state->body_fetch_pending_item_id ==
                    items[state->note_list_selected].item_id) {
                LoadCurrentNoteBody(state);
                if (state->current_note_loaded) {
                    ArmOpenObservation(state);
                    state->note_scroll_offset_lines = 0;
                    ResetNoteImageViewer(state);
                    state->mode = NoteAppMode::kNoteView;
                    state->message = "阅读中";
                } else {
                    state->body_fetch_error = true;
                    state->message = "云端暂无该笔记内容";
                }
                return;
            }
            state->message = "内容已就绪";
            return;
        }
        if (matches_current && state->mode == NoteAppMode::kNoteView &&
            !state->current_note_loaded) {
            // Defensive placeholder path (should not be reachable now that
            // confirm stays on the list, but a completed fetch must still
            // resolve it in place).
            LoadCurrentNoteBody(state);
            state->note_scroll_offset_lines = 0;
            if (state->current_note_loaded) {
                state->message = "阅读中";
            } else {
                state->body_fetch_error = true;
                state->message = "云端暂无该笔记内容";
            }
        }
        return;
    }
    if (!matches_current) {
        ESP_LOGW(kTag,
                 "stale note body fetch result dropped: %s nb=%.8s (now waiting %.8s)",
                 esp_err_to_name(result), fetched_notebook_id.c_str(),
                 state->body_fetch_notebook_id.c_str());
        return;
    }
    if ((state->mode == NoteAppMode::kNoteList && pending_open) ||
        (state->mode == NoteAppMode::kNoteView && !state->current_note_loaded)) {
        state->body_fetch_error = true;
        state->message = auth_required
            ? "请重新配对"
            : (!message.empty() ? message : "内容同步失败");
        ESP_LOGW(kTag, "note body pack fetch failed: %s nb=%.8s",
                 esp_err_to_name(result), state->body_fetch_notebook_id.c_str());
    }
}

bool UpdateNoteTransferProgress(
    NoteAppState* state,
    uint8_t kind,
    uint32_t generation,
    uint32_t done_bytes,
    uint32_t total_bytes,
    int64_t now_us)
{
    if (state == nullptr) return false;
    // Is this UI still waiting on a download at all? Independent of the
    // mailbox: the bar appears at request time and must survive the no-bytes
    // phases (queue behind a busy domain, TLS setup, result-in-queue).
    // Image: an in-flight download for the PREVIOUS image still counts while
    // the user has flipped onward -- it must finish before the new target
    // can start, so the bar honestly tracks "time until your image".
    const bool waiting_image =
        state->mode == NoteAppMode::kNoteImageView && !state->image_error &&
        (state->image_request || state->image_in_flight);
    const bool waiting_pack =
        (state->mode == NoteAppMode::kNoteList ||
         state->mode == NoteAppMode::kNoteView) &&
        !state->current_note_loaded && !state->body_fetch_error &&
        (state->body_fetch_request || state->body_fetch_in_flight);
    if (!waiting_image && !waiting_pack) {
        if (state->transfer_progress_bucket < 0) {
            return false;
        }
        state->transfer_progress_bucket = -1;
        state->transfer_progress_repaint_us = now_us;
        return true;
    }
    // Consume only bytes that belong to the dispatch THIS UI is waiting on
    // (generation) AND whose kind matches the page (the dispatched-id defense
    // above already rules out a stale image download painting under a newer
    // image's loading page). Anything else: keep showing what we have.
    const bool kind_matches =
        (waiting_image && kind == 1) || (waiting_pack && kind == 2);
    if (!kind_matches || generation == 0 ||
        generation != state->transfer_progress_generation || total_bytes == 0) {
        return false;
    }
    const uint64_t scaled = static_cast<uint64_t>(done_bytes) * 5U / total_bytes;
    const int next = static_cast<int>(std::min<uint64_t>(scaled, 5));
    if (next == state->transfer_progress_bucket) {
        return false;
    }
    // Throttle forward segment steps to one e-ink repaint per ~800 ms;
    // appear/disappear transitions above stay immediate.
    if (next > state->transfer_progress_bucket &&
        state->transfer_progress_bucket >= 0 &&
        now_us - state->transfer_progress_repaint_us < 800000) {
        return false;
    }
    state->transfer_progress_bucket = static_cast<int8_t>(next);
    state->transfer_progress_repaint_us = now_us;
    return true;
}

void ApplyNoteImageResult(
    NoteAppState* state,
    esp_err_t result,
    const std::string& image_id,
    std::shared_ptr<const std::vector<uint8_t>> wqni)
{
    if (state == nullptr) return;
    state->image_in_flight = false;
    if (result == ESP_OK && wqni != nullptr) {
        // Keep the payload even if the user already flipped elsewhere; it only
        // becomes visible when the ids line up again.
        state->image_loaded_id = image_id;
        state->image_wqni = std::move(wqni);
        if (state->image_expected_id == image_id) {
            state->image_error = false;
        }
        return;
    }
    // Only one fetch is ever in flight, so a failure terminates the wait for
    // the image it belongs to. Early exits (invalid token, DNS failure) carry
    // no image id; attribute those to the id recorded at dispatch instead of
    // blindly poisoning whatever the viewer wants NOW -- if the user flipped
    // while the doomed fetch was in flight, image_error would block the
    // self-heal and lock the new image on the loading page forever.
    const std::string& failed_id =
        image_id.empty() ? state->image_dispatched_id : image_id;
    if (state->mode == NoteAppMode::kNoteImageView && !state->image_request &&
        failed_id == state->image_expected_id) {
        state->image_error = true;
        ESP_LOGW(kTag, "note image fetch failed: %s id=%.12s",
                 esp_err_to_name(result), failed_id.c_str());
    }
}

// Self-healing for the image viewer: if the page sits in "loading" with no
// request, no in-flight fetch and no error (a dropped/mismatched result), re-arm
// the request so the pump retries instead of hanging forever. Failed fetches
// set image_error and are NOT re-armed (the user backs out or re-enters).
void EnsureNoteImageRequest(NoteAppState* state)
{
    if (state == nullptr || state->mode != NoteAppMode::kNoteImageView ||
        state->image_request || state->image_error ||
        state->image_expected_id.empty()) {
        return;
    }
    if (state->image_in_flight) {
        // A fetch wedged in the network stack (weak Wi-Fi, half-open TCP)
        // never delivers a result, and in_flight would block the self-heal
        // forever. Past the outer timeout, drop the claim and re-arm; a late
        // result is ignored by ApplyNoteImageResult's in_flight guard, and
        // the re-armed request cannot double-dispatch while the note cloud
        // domain is still busy.
        constexpr int64_t kImageFetchTimeoutUs = 20LL * 1000 * 1000;
        if (state->image_dispatch_us <= 0 ||
            esp_timer_get_time() - state->image_dispatch_us <
                kImageFetchTimeoutUs) {
            return;
        }
        ESP_LOGW(kTag, "note image fetch timed out; re-arming: id=%.12s",
                 state->image_expected_id.c_str());
        state->image_in_flight = false;
        state->image_request = true;
        return;
    }
    if (state->image_loaded_id == state->image_expected_id &&
        state->image_wqni != nullptr) {
        return;  // already showing
    }
    ESP_LOGW(kTag, "note image request re-armed: id=%.12s index=%u",
             state->image_expected_id.c_str(),
             static_cast<unsigned>(state->image_index));
    state->image_request = true;
}

// Drops the 15 KB image payload when the user leaves the note screen; SRAM is
// too scarce to keep it parked behind an invisible page. Re-entry goes through
// the loading page + SPIFFS cache, so the cost of coming back is one cache
// read, not a download. In-flight state stays untouched: a live fetch result
// is still applied (and simply held until the ids line up again).
void ReleaseNoteImagePayload(NoteAppState* state)
{
    if (state == nullptr) return;
    state->image_wqni.reset();
    state->image_loaded_id.clear();
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
    // [last-viewed-pin] Project the opened action into the session's pinned
    // last-viewed label right away. The pin is otherwise frozen at
    // session-creation time, so a notebook entered before any reading kept
    // showing 未读 for every note all day even though the observations reached
    // the cloud -- the fresh label only arrived with the NEXT session.
    if (pending.action == protocol::note_study_v1::ObservationAction::kOpened) {
        for (auto& item : advanced.remote.items) {
            if (pending.item_id == item.item_id) {
                std::snprintf(
                    item.last_opened_at,
                    sizeof(item.last_opened_at),
                    "%s",
                    pending.occurred_at.c_str());
                break;
            }
        }
    }
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

void RestoreNoteObservationEffect(NoteAppState* state)
{
    if (state == nullptr) return;
    // Only a taken-but-undispatched effect may be re-armed; a terminal
    // cursor-invalid failure (kFailed) stays failed.
    if (state->session.commit_state == NoteObservationCommitState::kPersisting) {
        state->session.observation_effect_ready = true;
    }
}

bool NoteImageLoadingGraceActive(const NoteAppState& state, int64_t now_us)
{
    if (state.mode != NoteAppMode::kNoteImageView || state.image_error) {
        return false;
    }
    if (state.image_loaded_id == state.image_expected_id &&
        state.image_wqni != nullptr) {
        return false;  // payload ready: paint it now
    }
    constexpr int64_t kNoteImageGraceUs = 400LL * 1000;
    return state.image_view_entered_us > 0 &&
        now_us - state.image_view_entered_us < kNoteImageGraceUs;
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
    snapshot.notebook_window_start = state.notebook_window_start;
    snapshot.note_list_window_start = state.note_list_window_start;
    snapshot.note_scroll_offset_lines = state.note_scroll_offset_lines;
    snapshot.cloud_sync_failed = state.cloud_sync_failed;
    snapshot.notebook_count = state.pack_index.notebooks.size();
    snapshot.notebooks.reserve(
        state.pack_index.notebooks.size() + state.problem_sets.size());
    for (const NotePackNotebook& notebook : state.pack_index.notebooks) {
        NoteNotebookRow row;
        row.title = notebook.title;
        row.note_count = notebook.entry_count;
        row.has_pack = notebook.has_pack;
        snapshot.notebooks.push_back(std::move(row));
    }
    // Problem sets render as [题] rows after every notebook; the row count
    // column shows their problem count so the mixed list stays scannable.
    for (const NoteProblemSetRow& set : state.problem_sets) {
        NoteNotebookRow row;
        row.title = "[题] " + set.name;
        row.note_count = set.entry_count;
        row.has_pack = true;
        snapshot.notebooks.push_back(std::move(row));
    }
    // Non-default word decks follow as [词] rows (word counts trailing).
    for (const NoteWordDeckRow& deck : state.word_decks) {
        NoteNotebookRow row;
        row.title = "[词] " + deck.title;
        row.note_count = deck.entry_count;
        row.has_pack = true;
        snapshot.notebooks.push_back(std::move(row));
    }

    if (state.mode == NoteAppMode::kNoteList || state.mode == NoteAppMode::kNoteView ||
        state.mode == NoteAppMode::kNoteImageView) {
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
    if (state.mode == NoteAppMode::kNoteView || state.mode == NoteAppMode::kNoteImageView) {
        snapshot.has_body = state.current_note_loaded;
        snapshot.body_fetch_active = !state.current_note_loaded &&
            (state.body_fetch_request || state.body_fetch_in_flight);
        snapshot.body_fetch_failed =
            !state.current_note_loaded && state.body_fetch_error;
        snapshot.note_title = state.current_note.title;
        snapshot.note_body = state.current_note.content;
        snapshot.note_id = state.current_note.note_id;
        snapshot.note_image_count =
            static_cast<uint8_t>(std::min<size_t>(state.current_note.image_ids.size(), 4));
    }
    if (state.mode == NoteAppMode::kNoteImageView) {
        snapshot.note_image_index = state.image_index;
        snapshot.note_image_id = state.image_expected_id;
        snapshot.note_image_error = state.image_error;
        snapshot.note_image_ready = !state.image_error &&
            state.image_wqni != nullptr &&
            state.image_loaded_id == state.image_expected_id &&
            !state.image_expected_id.empty();
        // [status-bar-progress] Carry the payload whenever one exists, ready
        // or not: a flip to a not-yet-downloaded image keeps SHOWING the
        // previous image (with the progress banner) instead of a blocking
        // loading page. shared_ptr: no copy.
        if (state.image_wqni != nullptr) {
            snapshot.note_image_wqni = state.image_wqni;
        }
    }
    // Progress presentation is not mode-gated: the list shows the status-bar
    // strip while a body pack downloads behind it.
    snapshot.transfer_progress_bucket = state.transfer_progress_bucket;
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
        case NoteAppMode::kNoteImageView:
            snapshot.hint = "上下翻图 · 长按返回";
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
            if (state.current_note_loaded) return "阅读中";
            if (state.body_fetch_request || state.body_fetch_in_flight) {
                return "正在同步内容";
            }
            return state.body_fetch_error ? "内容同步失败" : "内容未同步";
        case NoteAppMode::kNoteImageView:
            return state.image_error ? "图片加载失败" : "查看图片";
    }
    return "";
}

std::string NoteAppSignature(const NoteAppState& state)
{
    // Compact identity for the render layer to detect meaningful frame changes.
    // image_index/error must participate: flipping images or a failed fetch
    // changes the frame with every other field identical. The body-fetch
    // flags likewise: syncing->loaded/failed transitions repaint the body
    // placeholder with every other field unchanged.
    char buffer[144] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%d",
        static_cast<unsigned>(state.mode),
        static_cast<unsigned>(state.notebook_selected),
        static_cast<unsigned>(state.note_list_selected),
        static_cast<unsigned>(state.note_scroll_offset_lines),
        static_cast<unsigned>(state.pack_index.notebooks.size()),
        static_cast<unsigned>(state.problem_sets.size()),
        static_cast<unsigned>(state.word_decks.size()),
        static_cast<unsigned>(state.session.commit_state),
        static_cast<unsigned>(state.image_index),
        static_cast<unsigned>(state.image_error ? 1 : 0),
        static_cast<unsigned>(
            (state.body_fetch_request || state.body_fetch_in_flight) ? 1 : 0),
        static_cast<unsigned>(state.body_fetch_error ? 1 : 0),
        static_cast<int>(state.transfer_progress_bucket));
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

    // Opening a title whose pack is NOT on disk stays on the list, arms the
    // targeted body fetch and records the auto-open intent; the durable
    // `opened` observation is deferred until the content actually shows
    // (status-bar progress model).
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
        !require(b.mode == NoteAppMode::kNoteList,
                 "confirm on missing pack stays on the list") ||
        !require(b.body_fetch_request, "confirm arms the body fetch") ||
        !require(b.body_fetch_pending_open, "confirm records the open intent") ||
        !require(b.session.commit_state == NoteObservationCommitState::kIdle,
                 "observation deferred until content shows") ||
        !require(!b.session.observation_effect_ready,
                 "no observation effect before content shows")) {
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
    // The fixture has no pack on disk, so confirm parked the open intent on
    // the list; arm the observation directly, exactly as the auto-open path
    // does once the fetched content is shown.
    ArmOpenObservation(&e);
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

    // Abandoning from the title list returns to the notebook list (the plan's
    // "结束/放弃会话" affordance) and drops the loaded note, without touching
    // storage; re-entering a notebook starts a fresh session.
    NotePageFixture abandon;
    if (!abandon) return require(false, "allocate abandon fixture");
    NoteAppState& a = abandon.get();
    a.initialized = true;
    a.mode = NoteAppMode::kNoteList;
    a.session.persisted.active = true;
    a.session.persisted.remote.session_id = "00000000-0000-4000-8000-000000000003";
    a.session.persisted.remote.mode = Mode::kRecent;
    a.session.persisted.remote.items.push_back(MakeItem(0));
    a.current_note_loaded = true;
    a.note_list_selected = 0;
    if (HandleNoteAppInput(&a, NoteInput::kLongConfirm) != ESP_OK ||
        !require(a.mode == NoteAppMode::kNotebookList, "abandon returns to notebook list") ||
        !require(!a.current_note_loaded, "abandon clears loaded note")) {
        return false;
    }

    // Stale-result isolation: a session-start result that arrives when none is
    // expected (the open was abandoned before the cloud replied) is discarded
    // without activating a session or leaving the notebook list.
    NotePageFixture stale;
    if (!stale) return require(false, "allocate stale fixture");
    NoteAppState& st = stale.get();
    st.initialized = true;
    st.mode = NoteAppMode::kNotebookList;
    st.session.start_result_expected = false;
    PersistedNoteSession stale_persisted;
    stale_persisted.active = true;
    if (!require(!ApplyNoteSessionStartResult(
                     &st, ESP_OK, ESP_OK, ESP_OK, stale_persisted),
                 "stale session result discarded") ||
        !require(st.mode == NoteAppMode::kNotebookList, "stale result keeps screen") ||
        !require(!st.session.persisted.active, "stale result does not activate session")) {
        return false;
    }

    // Snapshot-consistency guard: a candidate page whose session_id does not
    // match the pinned snapshot is rejected without growing the item window.
    NotePageFixture snap;
    if (!snap) return require(false, "allocate snapshot fixture");
    NoteAppState& sn = snap.get();
    sn.initialized = true;
    sn.mode = NoteAppMode::kNoteList;
    sn.session.persisted.active = true;
    sn.session.persisted.remote.session_id = "00000000-0000-4000-8000-000000000005";
    sn.session.persisted.remote.mode = Mode::kRecent;
    sn.session.persisted.remote.items.push_back(MakeItem(0));
    sn.session.page_in_flight = true;
    const size_t snapshot_before = sn.session.persisted.remote.items.size();
    protocol::note_study_v1::CandidatePageData mismatched;
    mismatched.session_id = "ffffffff-ffff-4fff-8fff-ffffffffffff";
    protocol::note_study_v1::SessionItem stray;
    stray.item_id = "22222222-2222-4222-8222-222222222222";
    stray.notebook_id = "11111111-1111-4111-8111-111111111111";
    stray.ordinal = 1;
    mismatched.items.push_back(stray);
    ApplyNoteCandidatePageResult(&sn, ESP_OK, mismatched);
    if (!require(sn.session.persisted.remote.items.size() == snapshot_before,
                 "snapshot mismatch rejects candidate page") ||
        !require(!sn.session.page_in_flight, "candidate result clears in-flight")) {
        return false;
    }
    return true;
}

}  // namespace wqn
