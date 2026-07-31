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

// One problem set mixed into the notebook list (rendered with a [题] prefix
// after every notebook). Plain data mirrored from the problem pack index so
// note_app carries no dependency on the problem layer.
struct NoteProblemSetRow {
    std::string set_id;
    std::string name;
    size_t entry_count = 0;
};

// One non-default word deck mixed into the notebook list ([词] rows after the
// [题] rows). Plain data mirrored from the word deck catalog.
struct NoteWordDeckRow {
    std::string deck_id;
    std::string title;
    size_t entry_count = 0;
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

    // Problem sets appended to the notebook list ([题] rows). Selecting one
    // raises problem_set_open_requested instead of a note session; the UI
    // layer hands the id to the problem browse layer.
    std::vector<NoteProblemSetRow> problem_sets;
    bool problem_set_open_requested = false;
    std::string requested_problem_set_id;

    // Non-default word decks appended after the [题] rows ([词] rows).
    // Selecting one raises word_deck_open_requested; the UI layer scopes the
    // word page to that deck and switches screens.
    std::vector<NoteWordDeckRow> word_decks;
    bool word_deck_open_requested = false;
    std::string requested_word_deck_id;

    WqnNoteEntry current_note;
    bool current_note_loaded = false;

    // Targeted body-pack fetch (kNoteView opened a note whose pack is not on
    // disk). Same take/restore/apply shape as the image fetch below: the user
    // explicitly asked for this content, so the open turns into a foreground
    // single-notebook sync instead of a passive "try again later" page.
    bool body_fetch_request = false;
    bool body_fetch_in_flight = false;
    bool body_fetch_error = false;
    std::string body_fetch_notebook_id;
    // Confirm on a missing note stays on the LIST (the user keeps browsing
    // instead of staring at a blocking wait page); these record the intent so
    // a completed fetch auto-opens the note -- but only if the selection
    // still points at it (item_id match, not index: pages append).
    bool body_fetch_pending_open = false;
    std::string body_fetch_pending_item_id;
    // Dispatch time (us); the take path times out a fetch wedged in the
    // network stack and re-arms it (mirrors image_dispatch_us).
    int64_t body_fetch_dispatch_us = 0;

    // Full-screen image viewing (kNoteImageView). The WQNI bytes live behind a
    // shared_ptr so snapshots share them without copying 15 KB per frame and a
    // late replacement cannot tear a render in progress.
    uint8_t image_index = 0;
    bool image_request = false;
    bool image_in_flight = false;
    bool image_error = false;
    // Id the viewer currently wants; results for anything else are dropped.
    std::string image_expected_id;
    // Id handed to the fetcher on the last dispatch: failures that arrive with
    // an empty id (early exits before the download) are attributed to it, so a
    // stale failure cannot poison an image the user flipped to meanwhile.
    std::string image_dispatched_id;
    // Id of the payload currently held in image_wqni (may lag expected).
    std::string image_loaded_id;
    std::shared_ptr<const std::vector<uint8_t>> image_wqni;
    // Dispatch time of the in-flight fetch (us); lets the pump time out a
    // request wedged in the network stack and re-arm it.
    int64_t image_dispatch_us = 0;
    // When the viewer entered loading (entry or flip); drives the one-refresh
    // grace window for cache hits.
    int64_t image_view_entered_us = 0;

    bool cloud_sync_requested = false;
    bool cloud_sync_failed = false;
    bool cloud_loaded_once = false;
    // [transfer-progress] Download-progress presentation for the two waiting
    // pages (image loading / body-pack syncing). generation identifies the
    // dispatch this UI is willing to consume from the runner-side mailbox;
    // bucket is the quantized 0..5 fill (-1 hidden); repaint_us throttles
    // e-ink repaints to one forward step per ~800 ms.
    uint32_t transfer_progress_generation = 0;
    int8_t transfer_progress_bucket = -1;
    int64_t transfer_progress_repaint_us = 0;
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
    // Body-pack fetch presentation: active shows "正在同步内容", failed shows
    // the retry hint. Both must reach FrameSignature or the page freezes on
    // the loading text (the classic missing-signature-field trap).
    bool body_fetch_active = false;
    bool body_fetch_failed = false;
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
    // Quantized download progress (-1 hidden, 0..5 segments) for the image
    // loading page and the body "正在同步内容" page.
    int8_t transfer_progress_bucket = -1;
    std::string status_line;
    std::string hint;
};

esp_err_t InitNoteApp(NoteAppState* state);
esp_err_t HandleNoteAppInput(NoteAppState* state, NoteInput input);
void ApplyNotePackIndex(NoteAppState* state, NotePackIndex index, const std::string& message);
// Installs the mixed-list problem set rows (from the problem pack index).
void ApplyNoteProblemSetRows(NoteAppState* state, std::vector<NoteProblemSetRow> rows);
// Consumes a pending [题] row selection; returns false when none is armed.
bool TakeNoteProblemSetOpenRequest(NoteAppState* state, std::string* set_id);
// Installs the mixed-list word deck rows (from the word deck catalog, the
// default deck already excluded by the caller).
void ApplyNoteWordDeckRows(NoteAppState* state, std::vector<NoteWordDeckRow> rows);
// Consumes a pending [词] row selection; returns false when none is armed.
bool TakeNoteWordDeckOpenRequest(NoteAppState* state, std::string* deck_id);

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
    std::string* image_id,
    uint32_t* progress_generation);
void RestoreNoteImageRequest(NoteAppState* state);
// Targeted single-notebook pack fetch for an opened-but-missing note body;
// same take/restore/apply shape as the image path. Apply installs the rebuilt
// index in place (even mid-browse) and reloads the open note's body.
bool TakeNoteBodyFetchRequest(NoteAppState* state, std::string* notebook_id,
                              uint32_t* progress_generation);
void RestoreNoteBodyFetchRequest(NoteAppState* state);
void ApplyNoteBodyFetchResult(
    NoteAppState* state,
    esp_err_t result,
    NotePackIndex index,
    bool index_ready,
    bool auth_required,
    const std::string& fetched_notebook_id,
    const std::string& message);
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
// [transfer-progress] Folds one mailbox snapshot into the presentation state.
// kind mirrors the ui-layer CloudTransferKind (0 none, 1 note image, 2
// notebook pack). Returns true when the quantized bucket changed and the
// waiting page should repaint (caller schedules a kTimer refresh).
bool UpdateNoteTransferProgress(
    NoteAppState* state,
    uint8_t kind,
    uint32_t generation,
    uint32_t done_bytes,
    uint32_t total_bytes,
    int64_t now_us);
// Re-arms a taken-but-not-dispatched observation effect (queue full/busy) so
// the pump retries; the recorded request_id keeps the retry idempotent.
void RestoreNoteObservationEffect(NoteAppState* state);
// True while the image viewer is freshly loading and still inside the grace
// window: the display loop holds the loading-page commit briefly so a fast
// cache hit paints the image with ONE full refresh instead of two.
bool NoteImageLoadingGraceActive(const NoteAppState& state, int64_t now_us);
void RefreshNoteOutboxState(NoteAppState* state);
NoteAppSnapshot BuildNoteAppSnapshot(const NoteAppState& state);
std::string NoteAppStatusLine(const NoteAppState& state);
std::string NoteAppSignature(const NoteAppState& state);
bool RunNotePageStateSelfTest();

}  // namespace wqn
