#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "device_protocol/note_study.h"
#include "esp_err.h"
#include "note_pack.h"
#include "note_store.h"
#include "wqn_api.h"

namespace wqn {

enum class NoteInput {
    kUp,
    kDown,
    kConfirm,
    kLongConfirm,
};

// 笔记本 -> 标题 -> 笔记, with long-press Confirm returning one level. There is no
// card flip and no known/unknown: browsing is the whole interaction.
// kNoteImageView is a full-screen image layer above the body: entered by
// pressing Up at the top of the body when the note carries images.
enum class NoteAppMode : uint8_t {
    kNotebookList,
    kSessionStarting,
    kNoteList,
    kNoteView,
    kNoteImageView,
};

enum class NoteObservationCommitState : uint8_t {
    kIdle,
    kPersisting,
    kCloudPending,
    kCloudAcknowledged,
    kFailed,
};

struct NoteSessionState {
    bool start_requested = false;
    bool start_result_expected = false;
    std::string requested_notebook_id;
    std::string create_request_id;
    bool page_requested = false;
    bool page_in_flight = false;
    PersistedNoteSession persisted;
    NoteObservationCommitState commit_state = NoteObservationCommitState::kIdle;
    bool observation_effect_ready = false;
    DurableNoteObservation pending_observation;
    PersistedNoteSession pending_advanced_session;
};

struct NoteOutboxState {
    size_t pending_count = 0;
    size_t capacity = kNoteObservationOutboxCapacity;
};

struct NoteAppState {
    bool initialized = false;
    NoteAppMode mode = NoteAppMode::kNotebookList;
    size_t notebook_selected = 0;
    size_t note_list_selected = 0;
    // Edge-triggered list viewports: first visible row of the notebook/title
    // lists. note_app.cpp moves them only when the selection walks off a window
    // edge, so in-window steps repaint just two rows on the panel.
    size_t notebook_window_start = 0;
    size_t note_list_window_start = 0;
    uint32_t note_scroll_offset_lines = 0;
    // Total wrapped body lines for the open note (computed at load with the same
    // wrap width as RenderNoteBody). Bounds Down-scroll so over-scrolling past the
    // end cannot inflate the offset and leave Up-scroll with nothing to repaint.
    uint32_t note_body_total_lines = 0;

    NoteSessionState session;
    NoteOutboxState outbox;

    NotePackIndex pack_index;
    // A cloud refresh never mutates the content backing an active browse; it
    // becomes current only after returning to the notebook list.
    NotePackIndex pending_pack_index;
    bool pending_pack_index_ready = false;

    WqnNoteEntry current_note;
    bool current_note_loaded = false;

    // Full-screen image viewing (kNoteImageView). The WQNI bytes live behind a
    // shared_ptr so snapshots share them without copying 15 KB per frame and a
    // late replacement cannot tear a render in progress.
    uint8_t image_index = 0;
    bool image_request = false;
    bool image_in_flight = false;
    bool image_error = false;
    // Id the viewer currently wants; results for anything else are dropped.
    std::string image_expected_id;
    // Id of the payload currently held in image_wqni (may lag expected).
    std::string image_loaded_id;
    std::shared_ptr<const std::vector<uint8_t>> image_wqni;
    // Dispatch time of the in-flight fetch (us); lets the pump time out a
    // request wedged in the network stack and re-arm it.
    int64_t image_dispatch_us = 0;

    bool cloud_sync_requested = false;
    bool cloud_sync_failed = false;
    bool cloud_loaded_once = false;
    std::string message;
};

struct NoteNotebookRow {
    std::string title;
    size_t note_count = 0;
    bool has_pack = false;
};

struct NoteTitleRow {
    std::string title;
    // Empty when never viewed; otherwise the pinned last-viewed ISO timestamp.
    std::string last_opened_at;
};

struct NoteAppSnapshot {
    NoteAppMode mode = NoteAppMode::kNotebookList;
    size_t notebook_selected = 0;
    size_t note_list_selected = 0;
    // Mirrors of the edge-triggered viewports; the render layer draws exactly
    // this window instead of recomputing one from the selection.
    size_t notebook_window_start = 0;
    size_t note_list_window_start = 0;
    uint32_t note_scroll_offset_lines = 0;
    bool has_body = false;
    bool cloud_sync_failed = false;
    size_t notebook_count = 0;
    std::vector<NoteNotebookRow> notebooks;
    std::string notebook_title;
    std::vector<NoteTitleRow> titles;
    std::string note_title;
    std::string note_body;
    // Identity of the currently-open note; the render layer keys its wrapped-line
    // cache on this so scrolling does not re-wrap the body every frame.
    std::string note_id;
    // Image layer: count enables the body entry line; the payload pointer is
    // only set when ready and matching note_image_id.
    uint8_t note_image_index = 0;
    uint8_t note_image_count = 0;
    bool note_image_ready = false;
    bool note_image_error = false;
    std::string note_image_id;
    std::shared_ptr<const std::vector<uint8_t>> note_image_wqni;
    std::string status_line;
    std::string hint;
};

esp_err_t InitNoteApp(NoteAppState* state);
esp_err_t HandleNoteAppInput(NoteAppState* state, NoteInput input);
void ApplyNotePackIndex(NoteAppState* state, NotePackIndex index, const std::string& message);

bool TakeNoteSessionStartRequest(
    NoteAppState* state,
    protocol::note_study_v1::CreateSessionRequest* request);
// The runner thread already compacted (compact_result) and persisted
// (persist_result) the session snapshot; apply only installs it in memory so
// the UI task never runs the multi-second snapshot fsync.
bool ApplyNoteSessionStartResult(
    NoteAppState* state,
    esp_err_t result,
    esp_err_t compact_result,
    esp_err_t persist_result,
    PersistedNoteSession persisted);
void CancelNoteSessionStartResult(NoteAppState* state);
// Discards a server-invalid session (snapshot corrupt / not found / not active)
// and returns to the notebook list so the device stops reusing a bad session_id;
// re-opening a notebook creates a fresh session. Pack content stays mounted.
void ResetNoteSessionForServerInvalid(NoteAppState* state);
bool TakeNoteCandidatePageRequest(
    NoteAppState* state,
    protocol::note_study_v1::CandidatePageRequest* request,
    std::string* session_id);
void RestoreNoteCandidatePageRequest(NoteAppState* state);
void ApplyNoteCandidatePageResult(
    NoteAppState* state,
    esp_err_t result,
    protocol::note_study_v1::CandidatePageData page);
// Image download request/result plumbing; same take/restore/apply shape as the
// candidate page path, executed by the note cloud task.
bool TakeNoteImageRequest(
    NoteAppState* state,
    std::string* note_id,
    uint8_t* image_index,
    std::string* image_id);
void RestoreNoteImageRequest(NoteAppState* state);
// Frees the held WQNI payload (used when the user leaves the note screen);
// keeps request/in-flight bookkeeping intact.
void ReleaseNoteImagePayload(NoteAppState* state);
void ApplyNoteImageResult(
    NoteAppState* state,
    esp_err_t result,
    const std::string& image_id,
    std::shared_ptr<const std::vector<uint8_t>> wqni);
bool TakeNoteObservationEffect(
    NoteAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    DurableNoteObservation* observation,
    PersistedNoteSession* advanced_session);
void ApplyNoteObservationCommitResult(NoteAppState* state, esp_err_t result);
void RefreshNoteOutboxState(NoteAppState* state);
NoteAppSnapshot BuildNoteAppSnapshot(const NoteAppState& state);
std::string NoteAppStatusLine(const NoteAppState& state);
std::string NoteAppSignature(const NoteAppState& state);
bool RunNotePageStateSelfTest();

}  // namespace wqn
