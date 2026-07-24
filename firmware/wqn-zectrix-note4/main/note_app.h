#pragma once

#include <cstddef>
#include <cstdint>
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
enum class NoteAppMode : uint8_t {
    kNotebookList,
    kSessionStarting,
    kNoteList,
    kNoteView,
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
    uint32_t note_scroll_offset_lines = 0;

    NoteSessionState session;
    NoteOutboxState outbox;

    NotePackIndex pack_index;
    // A cloud refresh never mutates the content backing an active browse; it
    // becomes current only after returning to the notebook list.
    NotePackIndex pending_pack_index;
    bool pending_pack_index_ready = false;

    WqnNoteEntry current_note;
    bool current_note_loaded = false;

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
    uint32_t note_scroll_offset_lines = 0;
    bool has_body = false;
    bool cloud_sync_failed = false;
    size_t notebook_count = 0;
    std::vector<NoteNotebookRow> notebooks;
    std::string notebook_title;
    std::vector<NoteTitleRow> titles;
    std::string note_title;
    std::string note_body;
    std::string status_line;
    std::string hint;
};

esp_err_t InitNoteApp(NoteAppState* state);
esp_err_t HandleNoteAppInput(NoteAppState* state, NoteInput input);
void ApplyNotePackIndex(NoteAppState* state, NotePackIndex index, const std::string& message);

bool TakeNoteSessionStartRequest(
    NoteAppState* state,
    protocol::note_study_v1::CreateSessionRequest* request);
bool ApplyNoteSessionStartResult(
    NoteAppState* state,
    esp_err_t result,
    protocol::note_study_v1::SessionData session);
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
