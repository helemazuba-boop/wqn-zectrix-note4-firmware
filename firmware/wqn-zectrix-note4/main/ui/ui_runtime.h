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

// Every source which is allowed to mutate AppState has a typed entry point in
// UiRuntime.  The enum is also emitted in structured logs so event/revision
// sequences can be replayed during M4 hardware validation.
enum class AppEventKind : uint8_t {
    kBootstrap,
    kButton,
    kTodoCloudResult,
    kWordCloudResult,
    kNoteCloudResult,
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
    bool TakeWordCandidatePageRequest(
        wqn::protocol::word_study_v1::CandidatePageRequest* request,
        std::string* session_id);
    void RestoreWordCandidatePageRequest();
    bool TakeNoteCandidatePageRequest(
        wqn::protocol::note_study_v1::CandidatePageRequest* request,
        std::string* session_id);
    void RestoreNoteCandidatePageRequest();

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
