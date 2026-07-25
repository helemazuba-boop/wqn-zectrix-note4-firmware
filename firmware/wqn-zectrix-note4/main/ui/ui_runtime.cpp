#include "ui_runtime.h"

#include <utility>

#include "esp_log.h"
#include "ui_internal.h"

namespace device_ui_internal {
namespace {

constexpr char kTag[] = "wqn_ui_runtime";

}  // namespace

const char* AppEventKindName(AppEventKind event)
{
    switch (event) {
        case AppEventKind::kBootstrap:
            return "bootstrap";
        case AppEventKind::kButton:
            return "button";
        case AppEventKind::kTodoCloudResult:
            return "todo-result";
        case AppEventKind::kWordCloudResult:
            return "word-result";
        case AppEventKind::kNoteCloudResult:
            return "note-result";
        case AppEventKind::kTimeTick:
            return "time-tick";
        case AppEventKind::kAiTick:
            return "ai-tick";
        case AppEventKind::kAiStreamingSnapshot:
            return "ai-stream";
        case AppEventKind::kAiSessionSnapshot:
            return "ai-session";
        case AppEventKind::kFlashSnapshot:
            return "flash";
        case AppEventKind::kClockMinute:
            return "clock-minute";
        case AppEventKind::kStatusEditTimeout:
            return "status-timeout";
        case AppEventKind::kStatusReload:
            return "status-reload";
        case AppEventKind::kSyncResult:
            return "sync-result";
        case AppEventKind::kDisplayResult:
            return "display-result";
        default:
            return "unknown";
    }
}

uint64_t UiRuntime::NextRevision()
{
    ++state_.revision;
    if (state_.revision == wqn::display::kInvalidDisplayRevision) {
        ++state_.revision;
    }
    return state_.revision;
}

UiUpdate UiRuntime::FinishEvent(
    AppEventKind event,
    RefreshSchedule refresh,
    bool state_changed,
    bool force_revision)
{
    UiUpdate update;
    update.event = event;
    update.refresh = refresh;
    update.event_sequence = ++event_sequence_;
    update.state_changed = state_changed;
    // A render effect is itself revision-bearing even when it repairs the
    // current state (for example a forced waveform after a panel fault).
    update.revision_advanced =
        state_changed || force_revision || refresh != RefreshSchedule::kNone;
    if (update.revision_advanced) {
        NextRevision();
    }
    update.revision = state_.revision;

    if (update.revision_advanced || refresh != RefreshSchedule::kNone) {
        ESP_LOGI(kTag,
                 "app event: seq=%llu kind=%s changed=%d revision=%llu refresh=%s",
                 static_cast<unsigned long long>(update.event_sequence),
                 AppEventKindName(event), state_changed ? 1 : 0,
                 static_cast<unsigned long long>(update.revision),
                 RefreshScheduleName(refresh));
    }
    return update;
}

void UiRuntime::Initialize(wqn::AppState&& initial_state)
{
    state_ = std::move(initial_state);
    // Bootstrap always owns revision 1, including a degraded load. The UI can
    // therefore submit its recovery frame without using reserved revision 0.
    FinishEvent(AppEventKind::kBootstrap, RefreshSchedule::kImmediate, true);
}

UiUpdate UiRuntime::DispatchButton(
    const wqn::ButtonEvent& event,
    int64_t event_time_ms)
{
    const RefreshSchedule refresh = ApplyButtonEvent(event, event_time_ms, &state_);
    return FinishEvent(
        AppEventKind::kButton, refresh, refresh != RefreshSchedule::kNone);
}

UiUpdate UiRuntime::DispatchTodoCloudResult(const TodoCloudResult& result)
{
    const bool changed = ApplyTodoCloudResult(&state_, result);
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kTodo
            ? RefreshSchedule::kCommit
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kTodoCloudResult, refresh, changed);
}

UiUpdate UiRuntime::DispatchWordCloudResult(WordCloudResult& result)
{
    const bool changed = ApplyWordCloudResult(&state_, result);
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kWord
            ? RefreshSchedule::kCommit
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kWordCloudResult, refresh, changed);
}

bool UiRuntime::TakeWordCandidatePageRequest(
    wqn::protocol::word_study_v1::CandidatePageRequest* request,
    std::string* session_id)
{
    return wqn::TakeWordCandidatePageRequest(
        &state_.word_app, request, session_id);
}

void UiRuntime::RestoreWordCandidatePageRequest()
{
    wqn::RestoreWordCandidatePageRequest(&state_.word_app);
}

UiUpdate UiRuntime::DispatchNoteCloudResult(NoteCloudResult& result)
{
    const bool changed = ApplyNoteCloudResult(&state_, result);
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kNote
            ? RefreshSchedule::kCommit
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kNoteCloudResult, refresh, changed);
}

bool UiRuntime::TakeNoteCandidatePageRequest(
    wqn::protocol::note_study_v1::CandidatePageRequest* request,
    std::string* session_id)
{
    return wqn::TakeNoteCandidatePageRequest(
        &state_.note_app, request, session_id);
}

void UiRuntime::RestoreNoteCandidatePageRequest()
{
    wqn::RestoreNoteCandidatePageRequest(&state_.note_app);
}

bool UiRuntime::TakeNoteImageRequest(
    std::string* note_id, uint8_t* image_index, std::string* image_id)
{
    return wqn::TakeNoteImageRequest(
        &state_.note_app, note_id, image_index, image_id);
}

void UiRuntime::RestoreNoteImageRequest()
{
    wqn::RestoreNoteImageRequest(&state_.note_app);
}

UiUpdate UiRuntime::DispatchTimeTick(int64_t now_ms)
{
    const bool changed = wqn::TickTimeApp(&state_.time_app, now_ms);
    RefreshSchedule refresh = RefreshSchedule::kNone;
    if (changed) {
        UpdateHomePrimaryTimeLine(&state_);
        if (ShouldRefreshTimeTick(state_)) {
            refresh = RefreshSchedule::kTimer;
        }
    }
    return FinishEvent(AppEventKind::kTimeTick, refresh, changed);
}

UiUpdate UiRuntime::DispatchAiTick(int64_t now_ms)
{
    const bool changed = wqn::TickAiSession(&state_, now_ms);
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kAi
            ? RefreshSchedule::kAi
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kAiTick, refresh, changed);
}

UiUpdate UiRuntime::DispatchAiStreamingSnapshot(const wqn::AiStreamingStatusView& view)
{
    bool changed = false;
    switch (view.status) {
        case wqn::AiSessionStatus::kStreaming:
            changed = state_.ai.status != wqn::AiSessionStatus::kStreaming ||
                      state_.ai.pending_text != view.pending_label;
            state_.ai.status = wqn::AiSessionStatus::kStreaming;
            state_.ai.pending_text = view.pending_label;
            break;
        case wqn::AiSessionStatus::kReplyReady:
            changed = state_.ai.status != wqn::AiSessionStatus::kReplyReady ||
                      !state_.ai.pending_text.empty();
            state_.ai.status = wqn::AiSessionStatus::kReplyReady;
            state_.ai.pending_text.clear();
            break;
        case wqn::AiSessionStatus::kError:
            changed = state_.ai.status != wqn::AiSessionStatus::kError;
            state_.ai.status = wqn::AiSessionStatus::kError;
            break;
        default:
            break;
    }

    bool visible_change = false;
    if (state_.screen == wqn::UiScreen::kAi &&
        view.last_render_ms != state_.ai.last_render_ms) {
        state_.ai.last_render_ms = view.last_render_ms;
        visible_change = true;
        changed = true;
    }
    if (state_.screen == wqn::UiScreen::kAi && view.force_full_render) {
        visible_change = true;
    }
    if (state_.screen == wqn::UiScreen::kAi) {
        changed = changed || state_.ai.status_detail != view.tool_label;
        state_.ai.status_detail = view.tool_label;
    }
    return FinishEvent(
        AppEventKind::kAiStreamingSnapshot,
        visible_change ? RefreshSchedule::kAi : RefreshSchedule::kNone,
        changed);
}

UiUpdate UiRuntime::DispatchAiSessionSnapshot(const wqn::AiSessionState& snapshot)
{
    state_.ai = snapshot;
    // Keep the logical state in "preparing" immediately, but do not wake the
    // EPD while the ES8311 is being configured.  A physical panel refresh was
    // overlapping every failing codec transaction seen in the capture logs.
    // The following listening or error snapshot will render the final state.
    const bool defer_refresh_for_audio_init =
        snapshot.status == wqn::AiSessionStatus::kPreparingCapture;
    return FinishEvent(
        AppEventKind::kAiSessionSnapshot,
        state_.screen == wqn::UiScreen::kAi && !defer_refresh_for_audio_init
            ? RefreshSchedule::kAi
            : RefreshSchedule::kNone,
        true);
}

UiUpdate UiRuntime::DispatchFlashSnapshot(const wqn::FlashUiState& flash)
{
    switch (flash.status) {
        case wqn::FlashStatus::kError:
            state_.ai.status = wqn::AiSessionStatus::kError;
            state_.ai.flash_status_label = "错误";
            state_.ai.flash_is_streaming = false;
            break;
        case wqn::FlashStatus::kStreaming:
            if (flash.capture_started) {
                state_.ai.status = wqn::AiSessionStatus::kListening;
                state_.ai.flash_status_label = "录音";
            } else if (flash.playback_active) {
                state_.ai.status = wqn::AiSessionStatus::kReplyReady;
                state_.ai.flash_status_label = "播放";
            } else if (flash.response_in_flight && flash.response_started) {
                state_.ai.status = wqn::AiSessionStatus::kStreaming;
                state_.ai.flash_status_label = "生成";
            } else if (flash.response_in_flight) {
                state_.ai.status = wqn::AiSessionStatus::kWaitingReply;
                state_.ai.flash_status_label = "识别";
            } else {
                state_.ai.status = wqn::AiSessionStatus::kIdle;
                state_.ai.flash_status_label = "就绪";
            }
            state_.ai.flash_is_streaming = flash.response_in_flight;
            break;
        case wqn::FlashStatus::kConnecting:
            state_.ai.status = wqn::AiSessionStatus::kListening;
            state_.ai.flash_status_label = "连接";
            state_.ai.flash_is_streaming = false;
            break;
        default:
            state_.ai.status = wqn::AiSessionStatus::kIdle;
            state_.ai.flash_status_label = "空闲";
            state_.ai.flash_is_streaming = false;
            break;
    }
    state_.ai.flash_transcript = flash.user_transcript;
    state_.ai.assistant_text = flash.assistant_text;
    state_.ai.pending_text = flash.pending_text;
    state_.ai.flash_pending = flash.pending_text;
    state_.ai.flash_error = flash.error_message;
    if (!flash.tool_label.empty()) {
        state_.ai.status_detail = flash.tool_label;
    }
    state_.ai.status_since_ms = flash.status_since_ms;
    return FinishEvent(
        AppEventKind::kFlashSnapshot,
        state_.screen == wqn::UiScreen::kAi
            ? RefreshSchedule::kAi
            : RefreshSchedule::kNone,
        true);
}

UiUpdate UiRuntime::DispatchClockMinute(bool panel_needs_refresh)
{
    UpdateHomePrimaryTimeLine(&state_);
    const RefreshSchedule refresh =
        ScreenUsesClockMinute(state_) && panel_needs_refresh
            ? RefreshSchedule::kClock
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kClockMinute, refresh, true);
}

UiUpdate UiRuntime::DispatchStatusEditTimeout(int64_t now_ms)
{
    const bool expired =
        state_.status_edit.active && state_.screen == wqn::UiScreen::kAi &&
        state_.status_edit.last_action_ms > 0 &&
        now_ms - state_.status_edit.last_action_ms >= 3000;
    if (expired) {
        state_.status_edit.active = false;
    }
    return FinishEvent(
        AppEventKind::kStatusEditTimeout,
        expired ? RefreshSchedule::kSelection : RefreshSchedule::kNone,
        expired);
}

UiUpdate UiRuntime::DispatchStatusReload(wqn::AppState&& snapshot)
{
    const std::string before = FrameSignature(wqn::RenderUiFrame(state_));
    const uint64_t revision = state_.revision;
    state_ = std::move(snapshot);
    state_.revision = revision;
    const std::string after = FrameSignature(wqn::RenderUiFrame(state_));
    const bool changed = before != after;
    return FinishEvent(
        AppEventKind::kStatusReload,
        changed ? RefreshSchedule::kSelection : RefreshSchedule::kNone,
        changed);
}

UiUpdate UiRuntime::DispatchSyncResult(const wqn::services::SyncEvent& event)
{
    std::string status;
    switch (event.status) {
        case wqn::services::SyncEventStatus::kSucceeded:
            status = "同步完成";
            wqn::RefreshWordOutboxState(&state_.word_app);
            wqn::RefreshNoteOutboxState(&state_.note_app);
            break;
        case wqn::services::SyncEventStatus::kAwaitingClaim:
            status = "等待配对";
            break;
        case wqn::services::SyncEventStatus::kFailed:
        default:
            status = "同步失败";
            break;
    }

    const std::string claim_code = event.claim_code;
    const bool paired =
        event.status != wqn::services::SyncEventStatus::kAwaitingClaim;
    const bool claim_state_changed =
        state_.status.claim_code != claim_code ||
        state_.status.paired != paired;
    const bool changed =
        state_.settings.sync_status != status ||
        state_.status.last_sync_status != status ||
        claim_state_changed;
    state_.settings.sync_status = status;
    state_.status.last_sync_status = status;
    state_.status.claim_code = claim_code;
    state_.status.paired = paired;
    if (!paired && !claim_code.empty()) {
        state_.home.current_status =
            "配对码 " + claim_code + " · 请在网页确认";
    } else {
        state_.home.current_status =
            "本地 " + std::to_string(state_.problems.size()) + " 题 · 待上传 " +
            std::to_string(state_.status.pending_reviews);
    }
    const bool visible =
        state_.screen == wqn::UiScreen::kSettings ||
        state_.screen == wqn::UiScreen::kHome;
    const RefreshSchedule refresh =
        changed && visible
            ? (state_.screen == wqn::UiScreen::kSettings && claim_state_changed
                   ? RefreshSchedule::kCommit
                   : RefreshSchedule::kSelection)
            : RefreshSchedule::kNone;
    return FinishEvent(
        AppEventKind::kSyncResult,
        refresh,
        changed);
}

UiUpdate UiRuntime::DispatchDisplayResult(const wqn::display::DisplayResult& result)
{
    const bool failed = result.status == wqn::display::DisplayStatus::kFailed;
    if (failed) {
        wqn::RequestForceFullRefresh();
    }
    // A failed physical transaction gets a fresh revision for the forced-full
    // retry. Presented/Superseded are terminal observations and do not mutate
    // AppState or manufacture another render revision.
    return FinishEvent(
        AppEventKind::kDisplayResult,
        failed ? RefreshSchedule::kImmediate : RefreshSchedule::kNone,
        false,
        failed);
}

}  // namespace device_ui_internal
