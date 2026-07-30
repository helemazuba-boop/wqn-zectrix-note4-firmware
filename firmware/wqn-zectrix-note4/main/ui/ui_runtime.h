#pragma once

#include <cstdint>

#include "ai_session.h"
#include "button_input.h"
#include "display/display_types.h"
#include "flash_session.h"
#include "services/sync_service.h"
#include "ui_model.h"

namespace device_ui_internal {

enum class RefreshSchedule;
struct TodoCloudResult;
struct WordCloudResult;
struct NoteCloudResult;
struct ProblemCloudResult;

// Every source which is allowed to mutate AppState has a typed entry point in
// UiRuntime.  The enum is also emitted in structured logs so event/revision
// sequences can be replayed during M4 hardware validation.
enum class AppEventKind : uint8_t {
    kBootstrap,
    kButton,
    kTodoCloudResult,
    kWordCloudResult,
    kNoteCloudResult,
    kProblemCloudResult,
    kTimeTick,
    kAiTick,
    kAiStreamingSnapshot,
    kAiSessionSnapshot,
    kFlashSnapshot,
    kClockMinute,
    kStatusEditTimeout,
    kStatusReload,
    kSyncResult,
    kDisplayResult,
    kTransferProgress,
    kWordObservationPersist,
    kNoteObservationPersist,
};

struct UiUpdate {
    AppEventKind event = AppEventKind::kBootstrap;
    RefreshSchedule refresh;
    uint64_t event_sequence = 0;
    uint64_t revision = 0;
    bool state_changed = false;
    bool revision_advanced = false;
};

// Single-task state owner. All methods must be called from DeviceUiTask; worker
// observations enter through typed snapshots or bounded result queues.
class UiRuntime final {
public:
    UiRuntime() = default;
    UiRuntime(const UiRuntime&) = delete;
    UiRuntime& operator=(const UiRuntime&) = delete;

    void Initialize(wqn::AppState&& initial_state);

    const wqn::AppState& state() const { return state_; }
    uint64_t revision() const { return state_.revision; }

    UiUpdate DispatchButton(const wqn::ButtonEvent& event, int64_t event_time_ms);
    UiUpdate DispatchTodoCloudResult(const TodoCloudResult& result);
    UiUpdate DispatchWordCloudResult(WordCloudResult& result);
    UiUpdate DispatchNoteCloudResult(NoteCloudResult& result);
    UiUpdate DispatchProblemCloudResult(ProblemCloudResult& result);
    UiUpdate DispatchTimeTick(int64_t now_ms);
    UiUpdate DispatchAiTick(int64_t now_ms);
    UiUpdate DispatchAiStreamingSnapshot(const wqn::AiStreamingStatusView& view);
    UiUpdate DispatchAiSessionSnapshot(const wqn::AiSessionState& snapshot);
    UiUpdate DispatchFlashSnapshot(const wqn::FlashUiState& snapshot);
    UiUpdate DispatchClockMinute(bool panel_needs_refresh);
    UiUpdate DispatchStatusEditTimeout(int64_t now_ms);
    UiUpdate DispatchStatusReload(wqn::AppState&& snapshot);
    UiUpdate DispatchSyncResult(const wqn::services::SyncEvent& event);
    UiUpdate DispatchDisplayResult(const wqn::display::DisplayResult& result);
    // Folds the runner-side transfer-progress mailbox into the note domain's
    // waiting-page presentation (quantize + throttle live in note_app).
    UiUpdate DispatchTransferProgress(
        uint8_t kind,
        uint32_t generation,
        uint32_t done_bytes,
        uint32_t total_bytes,
        int64_t now_us);
    bool TakeWordCandidatePageRequest(
        wqn::protocol::word_study_v1::CandidatePageRequest* request,
        std::string* session_id);
    void RestoreWordCandidatePageRequest();
    bool TakeNoteCandidatePageRequest(
        wqn::protocol::note_study_v1::CandidatePageRequest* request,
        std::string* session_id);
    void RestoreNoteCandidatePageRequest();
    bool TakeNoteImageRequest(
        std::string* note_id, uint8_t* image_index, std::string* image_id,
        uint32_t* progress_generation);
    void RestoreNoteImageRequest();
    bool TakeNoteBodyFetchRequest(std::string* notebook_id,
                                  uint32_t* progress_generation);
    void RestoreNoteBodyFetchRequest();
    bool TakeNoteObservationEffect(
        const std::string& request_id,
        const std::string& occurred_at,
        uint32_t operation_id,
        wqn::DurableNoteObservation* observation,
        wqn::PersistedNoteSession* advanced_session);
    bool TakeProblemImageRequest(
        std::string* problem_id,
        bool* is_solution,
        uint8_t* image_index,
        std::string* image_id);
    void RestoreProblemImageRequest();
    bool TakeProblemVerdictEffect(
        const std::string& request_id,
        const std::string& occurred_at,
        wqn::DurableProblemObservation* observation);
    void RestoreProblemVerdictEffect();

    // [persist-worker] Word observation commit moved to the persist worker
    // (see ui/persist_worker.h). Take mirrors the note/problem effect wrappers;
    // the persist result is applied on the UI task here and returns the
    // card-advance refresh (the worker never touches AppState).
    bool TakeWordObservationEffect(
        const std::string& request_id,
        const std::string& occurred_at,
        uint32_t operation_id,
        wqn::DurableWordObservation* observation,
        wqn::PersistedWordSession* advanced_session);
    UiUpdate DispatchWordObservationPersistResult(esp_err_t result, uint32_t operation_id);
    // Take failed inside the pump (cursor desync -> session already moved to
    // kFailed). Route it through FinishEvent so the revision advances and the
    // failure frame is not deduped against the "正在保存" frame's revision.
    UiUpdate DispatchWordObservationTakeFailed();

    // [persist-worker] Note observation commit moved to the persist worker
    // (c3). Same shape as word: bound operation_id, applied only when it still
    // matches the pending commit; Take-failure advances the revision. A success
    // does not refresh (invisible bookkeeping); a failure / Take-failure returns
    // kSelection so the error status reaches the note screen.
    UiUpdate DispatchNoteObservationPersistResult(esp_err_t result, uint32_t operation_id);
    UiUpdate DispatchNoteObservationTakeFailed();

private:
    UiUpdate FinishEvent(
        AppEventKind event,
        RefreshSchedule refresh,
        bool state_changed,
        bool force_revision = false);
    uint64_t NextRevision();

    wqn::AppState state_;
    uint64_t event_sequence_ = 0;
};

const char* AppEventKindName(AppEventKind event);

}  // namespace device_ui_internal
