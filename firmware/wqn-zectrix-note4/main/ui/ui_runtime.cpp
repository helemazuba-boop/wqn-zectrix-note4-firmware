#include "ui_runtime.h"

#include <utility>

#include "esp_log.h"
#include "opencode_session.h"  // [follow] Agent viewport setters
#include "persist_worker.h"    // [detail] SubmitAgentDetailLevelSave
#include "ui_internal.h"
#include "services/sync_service.h"
#include "storage.h"
#include "word_app.h"

namespace device_ui_internal {
namespace {

constexpr char kTag[] = "wqn_ui_runtime";

// [detail] How long the Agent detail tier must hold still before its NVS write
// is submitted. Deliberately longer than the status bar's 400 ms double-confirm
// window: the undo gesture lands back on the persisted value, so waiting past
// the window means a double-click writes nothing at all.
constexpr int64_t kAgentDetailSaveDebounceMs = 500;

// [picker-stale] How long the session picker's rows may keep claiming 运行中
// before the UI re-reads them. Long enough that an actively-pressing user is not
// racing a 1-2 s list load, short enough to matter for a list the user is
// reading rather than glancing at.
constexpr int64_t kAgentListPollIntervalMs = 10000;

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
        case AppEventKind::kProblemCloudResult:
            return "problem-result";
        case AppEventKind::kTimeTick:
            return "time-tick";
        case AppEventKind::kAiTick:
            return "ai-tick";
        case AppEventKind::kAiStreamingSnapshot:
            return "ai-stream";
        case AppEventKind::kAiSessionSnapshot:
            return "ai-session";
        case AppEventKind::kAiViewportFollow:
            return "ai-follow";
        case AppEventKind::kFlashSnapshot:
            return "flash";
        case AppEventKind::kAgentSnapshot:
            return "agent";
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
        case AppEventKind::kTransferProgress:
            return "transfer-progress";
        case AppEventKind::kWordObservationPersist:
            return "word-persist";
        case AppEventKind::kNoteObservationPersist:
            return "note-persist";
        case AppEventKind::kProblemVerdictPersist:
            return "problem-persist";
        case AppEventKind::kSettingsPersist:
            return "settings-persist";
        case AppEventKind::kAgentListPoll:
            return "agent-list-poll";
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
    RetainTimeAppState(state_.time_app);
    return FinishEvent(
        AppEventKind::kButton, refresh, refresh != RefreshSchedule::kNone);
}

UiUpdate UiRuntime::DispatchTodoCloudResult(const TodoCloudResult& result)
{
    bool content_changed = true;
    const bool changed = ApplyTodoCloudResult(&state_, result, &content_changed);
    // Unchanged refreshes only need to clear the "syncing" hint: a partial
    // repaint, not the 1.3s full-refresh flash a content change warrants.
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kTodo
            ? (content_changed ? RefreshSchedule::kCommit
                               : RefreshSchedule::kSelection)
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
    wqn::PersistedWordSession* snapshot,
    std::string* session_id)
{
    return wqn::TakeWordCandidatePageRequest(
        &state_.word_app, request, snapshot, session_id);
}

void UiRuntime::RestoreWordCandidatePageRequest()
{
    wqn::RestoreWordCandidatePageRequest(&state_.word_app);
}

UiUpdate UiRuntime::DispatchNoteCloudResult(NoteCloudResult& result)
{
    const bool note_visible = state_.screen == wqn::UiScreen::kNote;
    const std::string before_signature = note_visible
        ? FrameSignature(wqn::RenderUiFrame(state_))
        : std::string();
    bool content_changed = true;
    const bool changed = ApplyNoteCloudResult(
        &state_, result, &content_changed);
    const bool visible_changed = note_visible &&
        before_signature != FrameSignature(wqn::RenderUiFrame(state_));
    // Structural note changes require a commit waveform. Pack-sync status is
    // a lightweight selection repaint, and an identical repeated result does
    // not submit at all. This also avoids refreshing a hidden/staged pack
    // index whose pixels are not represented on the current note subpage.
    const RefreshSchedule refresh =
        changed && visible_changed
            ? (content_changed ? RefreshSchedule::kCommit
                               : RefreshSchedule::kSelection)
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
    std::string* note_id, uint8_t* image_index, std::string* image_id,
    bool* gray4,
    uint32_t* progress_generation)
{
    return wqn::TakeNoteImageRequest(
        &state_.note_app, note_id, image_index, image_id, gray4,
        progress_generation);
}

void UiRuntime::RestoreNoteImageRequest()
{
    wqn::RestoreNoteImageRequest(&state_.note_app);
}

bool UiRuntime::TakeNoteBodyFetchRequest(std::string* notebook_id,
                                         uint32_t* progress_generation)
{
    return wqn::TakeNoteBodyFetchRequest(
        &state_.note_app, notebook_id, progress_generation);
}

void UiRuntime::RestoreNoteBodyFetchRequest()
{
    wqn::RestoreNoteBodyFetchRequest(&state_.note_app);
}

UiUpdate UiRuntime::DispatchTransferProgress(
    uint8_t kind,
    uint32_t generation,
    uint32_t done_bytes,
    uint32_t total_bytes,
    int64_t now_us)
{
    const bool changed = wqn::UpdateNoteTransferProgress(
        &state_.note_app, kind, generation, done_bytes, total_bytes, now_us);
    // kTimer keeps the small status-strip repaint on the local-partial path
    // (clock-tick precedent: tiny diffs, never wedged the SSD1683).
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kNote
            ? RefreshSchedule::kTimer
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kTransferProgress, refresh, changed);
}

bool UiRuntime::TakeNoteObservationEffect(
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    wqn::DurableNoteObservation* observation,
    wqn::PersistedNoteSession* advanced_session)
{
    return wqn::TakeNoteObservationEffect(
        &state_.note_app, request_id, occurred_at, operation_id, observation,
        advanced_session);
}

UiUpdate UiRuntime::DispatchNoteObservationPersistResult(
    esp_err_t result, uint32_t operation_id)
{
    // [persist-worker] Bind the result to the note session it was taken from
    // (mirrors word). A late result after a session reset / newer submit is
    // dropped, not applied; the caller still acks the mailbox.
    const wqn::NoteSessionState& session = state_.note_app.session;
    if (session.commit_state != wqn::NoteObservationCommitState::kPersisting ||
        session.pending_persist_operation_id != operation_id) {
        ESP_LOGW(kTag,
                 "stale note persist result: op=%lu expected=%lu state=%d",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(session.pending_persist_operation_id),
                 static_cast<int>(session.commit_state));
        return FinishEvent(AppEventKind::kNoteObservationPersist, RefreshSchedule::kNone, false);
    }
    wqn::ApplyNoteObservationCommitResult(&state_.note_app, result);
    if (result == ESP_OK) {
        wqn::services::RequestNoteOutboxUpload();
    } else {
        ESP_LOGW(kTag, "note observation persist failed: %s", esp_err_to_name(result));
    }
    BuildHomeSummary(&state_);
    // A successful note commit is invisible bookkeeping -> no refresh. A FAILURE
    // sets a status message (记录未保存 / 记录空间已满) that must reach the screen,
    // so repaint the note page (never flashing a refresh for normal opens).
    const RefreshSchedule refresh =
        (result != ESP_OK && state_.screen == wqn::UiScreen::kNote)
            ? RefreshSchedule::kSelection
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kNoteObservationPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchNoteObservationTakeFailed()
{
    // wqn::TakeNoteObservationEffect already moved the session to kFailed and
    // set "会话游标无效"; advance the revision AND repaint the note page so the
    // failure surfaces instead of a stuck "阅读中".
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kNote
        ? RefreshSchedule::kSelection
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kNoteObservationPersist, refresh, true);
}

bool UiRuntime::TakeWordObservationEffect(
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    wqn::DurableWordObservation* observation,
    wqn::PersistedWordSession* advanced_session)
{
    return wqn::TakeWordObservationEffect(
        &state_.word_app, request_id, occurred_at, operation_id, observation,
        advanced_session);
}

UiUpdate UiRuntime::DispatchWordObservationPersistResult(
    esp_err_t result, uint32_t operation_id)
{
    // [persist-worker] Bind the result to the word state it was taken from.
    // The worker ran async; the user may have left the scoped page or switched
    // decks (ResetWordSessionsForScopeChange resets the session, clearing the
    // expected id and commit_state). Applying then would install a stale/empty
    // advanced session over freshly-reset state. Drop it (log), but the caller
    // still acks the mailbox so the pool slot frees.
    const wqn::WordSessionState& session = state_.word_app.session;
    if (session.commit_state != wqn::WordObservationCommitState::kPersisting ||
        session.pending_persist_operation_id != operation_id) {
        ESP_LOGW(kTag,
                 "stale word persist result: op=%lu expected=%lu state=%d",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(session.pending_persist_operation_id),
                 static_cast<int>(session.commit_state));
        return FinishEvent(
            AppEventKind::kWordObservationPersist, RefreshSchedule::kNone, false);
    }
    // Worker-side storage has completed; apply it on the UI task (the worker
    // never touches AppState). The card leaves kPersisting: on success the
    // advanced session installs and the next card shows; on failure it moves to
    // a Confirm-to-retry state. Either way it is a content change on the word
    // screen -> selection-level partial (never a full refresh for bookkeeping),
    // matching the old synchronous path.
    wqn::ApplyWordObservationCommitResult(&state_.word_app, result);
    if (result == ESP_OK) {
        // Outbox is the interaction boundary; upload after a quiet period only
        // once the record is durable.
        wqn::services::RequestWordOutboxUpload();
    } else {
        ESP_LOGW(kTag, "word observation persist failed: %s", esp_err_to_name(result));
    }
    BuildHomeSummary(&state_);
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kWord
        ? RefreshSchedule::kSelection
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kWordObservationPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchWordObservationTakeFailed()
{
    // wqn::TakeWordObservationEffect already moved the session to kFailed and
    // cleared the armed effect; this only advances the revision (via
    // FinishEvent) so the failure frame is not treated as a duplicate of the
    // "正在保存" frame still in the display ledger.
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kWord
        ? RefreshSchedule::kSelection
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kWordObservationPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchProblemCloudResult(ProblemCloudResult& result)
{
    const bool changed = ApplyProblemCloudResult(&state_, result);
    // Catalog transitions cover most of Note, while Home only needs its summary
    // cards repainted.
    RefreshSchedule refresh = RefreshSchedule::kNone;
    if (changed && state_.screen == wqn::UiScreen::kNote) {
        refresh = RefreshSchedule::kCommit;
    } else if (changed && state_.screen == wqn::UiScreen::kHome) {
        refresh = RefreshSchedule::kSelection;
    }
    return FinishEvent(AppEventKind::kProblemCloudResult, refresh, changed);
}

bool UiRuntime::TakeProblemImageRequest(
    std::string* problem_id,
    bool* is_solution,
    uint8_t* image_index,
    std::string* image_id,
    bool* gray4)
{
    return wqn::TakeProblemImageRequest(
        &state_.problem_app, problem_id, is_solution, image_index, image_id,
        gray4);
}

void UiRuntime::RestoreProblemImageRequest()
{
    wqn::RestoreProblemImageRequest(&state_.problem_app);
}

bool UiRuntime::TakeProblemVerdictEffect(
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    wqn::DurableProblemObservation* observation)
{
    return wqn::TakeProblemVerdictEffect(
        &state_.problem_app, request_id, occurred_at, operation_id, observation);
}

UiUpdate UiRuntime::DispatchProblemVerdictPersistResult(
    esp_err_t result, uint32_t operation_id)
{
    // [persist-worker] Bind the result to the problem view it was taken from.
    // A late result after a reset / newer verdict is dropped, not applied; the
    // caller still acks the mailbox.
    wqn::ProblemAppState& problem = state_.problem_app;
    if (problem.commit_state != wqn::ProblemVerdictCommitState::kPersisting ||
        problem.pending_persist_operation_id != operation_id) {
        ESP_LOGW(kTag,
                 "stale problem persist result: op=%lu expected=%lu state=%d",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(problem.pending_persist_operation_id),
                 static_cast<int>(problem.commit_state));
        return FinishEvent(AppEventKind::kProblemVerdictPersist, RefreshSchedule::kNone, false);
    }
    wqn::ApplyProblemVerdictCommitResult(&problem, result);
    if (result == ESP_OK) {
        wqn::services::RequestProblemOutboxUpload();
    } else {
        ESP_LOGW(kTag, "problem verdict persist failed: %s", esp_err_to_name(result));
    }
    // Success advances to the next problem (a full-face change on the SSD1683
    // -> kCommit clears large-partial ghosting, the old cloud-result policy);
    // failure only flips the status message -> kSelection is enough.
    const bool problem_on_screen =
        state_.screen == wqn::UiScreen::kNote && problem.active;
    const RefreshSchedule refresh = !problem_on_screen
        ? RefreshSchedule::kNone
        : (result == ESP_OK ? RefreshSchedule::kCommit
                            : RefreshSchedule::kSelection);
    return FinishEvent(AppEventKind::kProblemVerdictPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchProblemVerdictTakeFailed()
{
    // wqn::TakeProblemVerdictEffect already moved the state to kFailed and set
    // "记录无效"; advance the revision and repaint the status line so the
    // failure surfaces instead of a stale view.
    const RefreshSchedule refresh =
        (state_.screen == wqn::UiScreen::kNote && state_.problem_app.active)
            ? RefreshSchedule::kSelection
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kProblemVerdictPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchAutoSyncSaveResult(esp_err_t result, uint32_t operation_id)
{
    // [persist-worker] Bind to the record armed at Confirm; a mismatched id
    // means the pending save was superseded/cleared -- drop it (caller still
    // acks the mailbox so the slot frees).
    wqn::SettingsAppState& settings = state_.settings;
    if (settings.auto_sync_save_op_id == 0 ||
        settings.auto_sync_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale auto-sync save result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(settings.auto_sync_save_op_id));
        return FinishEvent(AppEventKind::kSettingsPersist, RefreshSchedule::kNone, false);
    }
    settings.auto_sync_save_op_id = 0;
    if (result == ESP_OK) {
        settings.auto_sync_interval_min = settings.pending_auto_sync_minutes;
        settings.auto_sync_pending_valid = false;  // durably saved: nothing to retry
        settings.notice =
            "自动同步已保存：" + wqn::AutoSyncIntervalLabel(settings.auto_sync_interval_min);
        // The setting is local-authoritative. Re-arm its absolute deadline
        // only after the NVS ACK; changing an interval is not itself a reason
        // to spend radio energy on an immediate full sync.
        wqn::services::NotifyAutoSyncIntervalChanged(
            settings.auto_sync_interval_min);
    } else {
        // Keep the displayed (old) value AND the armed pending value so a
        // re-open preselects the intended interval and the user re-Confirms.
        settings.notice = "自动同步保存失败，请重试";
        ESP_LOGW(kTag, "auto-sync save failed: %s", esp_err_to_name(result));
    }
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kSettings
        ? RefreshSchedule::kConfig
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kSettingsPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchWordSessionResetResult(
    esp_err_t result, uint32_t operation_id)
{
    wqn::WordAppState& word_app = state_.word_app;
    if (word_app.scope_reset_save_op_id == 0 ||
        word_app.scope_reset_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale word scope reset result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(word_app.scope_reset_save_op_id));
        return FinishEvent(AppEventKind::kWordObservationPersist, RefreshSchedule::kNone, false);
    }
    word_app.scope_reset_save_op_id = 0;
    if (result == ESP_OK) {
        // Durable state is committed (new scope generation, four session files
        // and the walk cursor gone). NOW install the in-memory half and switch
        // screens -- the reset never blocked the UI task.
        // The pending deck is only installed when the user is still on the note
        // screen waiting for it (the [词] row switches screens on success). The
        // leave-word-page tail clears the override immediately and submits with
        // an empty pending deck, so an ACK landing later must not re-install a
        // scope the user already left, nor yank them back to the word page.
        const bool switching_screens = !word_app.scope_reset_pending_deck_id.empty();
        if (switching_screens) {
            word_app.scoped_deck_id = word_app.scope_reset_pending_deck_id;
            word_app.scoped_deck_title = word_app.scope_reset_pending_deck_title;
            state_.screen = wqn::UiScreen::kWord;
        }
        wqn::ResetWordSessionsInMemory(&word_app);
        word_app.scope_reset_pending_deck_id.clear();
        word_app.scope_reset_pending_deck_title.clear();
        word_app.scope_reset_pending_valid = false;
        state_.note_app.message = "词库范围已切换";
        ESP_LOGI(kTag, "word scope reset committed: switching=%s deck=%s",
                 switching_screens ? "yes" : "no",
                 word_app.scoped_deck_id.empty() ? "all"
                                                 : word_app.scoped_deck_id.c_str());
    } else {
        // Keep the old scope AND the armed pending pair so the user can retry
        // without re-picking the deck. If the transaction died mid-way the new
        // scope generation is already committed, so every old session file is
        // inert and boot cannot resume a half-switched scope.
        state_.note_app.message = "词库切换未保存，请重试";
        ESP_LOGW(kTag, "word scope reset failed: %s", esp_err_to_name(result));
    }
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kWord
        ? RefreshSchedule::kConfig
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kWordObservationPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchVolumeSaveResult(esp_err_t result, uint32_t operation_id)
{
    wqn::SettingsAppState& settings = state_.settings;
    if (settings.volume_save_op_id == 0 ||
        settings.volume_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale volume save result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(settings.volume_save_op_id));
        return FinishEvent(AppEventKind::kSettingsPersist, RefreshSchedule::kNone, false);
    }
    settings.volume_save_op_id = 0;
    if (result == ESP_OK) {
        settings.volume_percent = settings.pending_volume_percent;
        settings.volume_pending_valid = false;  // durably saved: nothing to retry
        settings.notice = "音量已保存：" + wqn::VolumeLabel(settings.volume_percent);
    } else {
        // The runtime playback cache already holds pending_volume_percent (set
        // at submit); keep the armed value so a re-open preselects it. Make the
        // status explicit that it is not durably saved yet.
        settings.notice = "音量未保存，请重试";
        ESP_LOGW(kTag, "volume save failed: %s", esp_err_to_name(result));
    }
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kSettings
        ? RefreshSchedule::kConfig
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kSettingsPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchImageRenderSaveResult(
    esp_err_t result, uint32_t operation_id)
{
    wqn::SettingsAppState& settings = state_.settings;
    if (settings.image_render_save_op_id == 0 ||
        settings.image_render_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale image render save result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(settings.image_render_save_op_id));
        return FinishEvent(AppEventKind::kSettingsPersist,
                           RefreshSchedule::kNone, false);
    }
    settings.image_render_save_op_id = 0;
    if (result == ESP_OK) {
        settings.image_render_mode = settings.pending_image_render_mode;
        settings.image_render_selected =
            settings.image_render_mode == wqn::ImageRenderMode::kBlackWhite
            ? 0
            : 1;
        settings.image_render_pending_valid = false;
        wqn::SetNoteImageRenderMode(
            &state_.note_app, settings.image_render_mode);
        wqn::SetProblemImageRenderMode(
            &state_.problem_app, settings.image_render_mode);
        settings.notice = "图片渲染已保存：" +
            wqn::ImageRenderModeLabel(settings.image_render_mode);
    } else {
        settings.notice = "图片渲染未保存，请重试";
        ESP_LOGW(kTag, "image render save failed: %s",
                 esp_err_to_name(result));
    }
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kSettings
        ? RefreshSchedule::kConfig
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kSettingsPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchDefaultDeckChangeResult(
    esp_err_t result, uint32_t operation_id)
{
    wqn::SettingsAppState& settings = state_.settings;
    if (settings.word_deck_save_op_id == 0 ||
        settings.word_deck_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale deck change result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(settings.word_deck_save_op_id));
        return FinishEvent(AppEventKind::kSettingsPersist, RefreshSchedule::kNone, false);
    }
    settings.word_deck_save_op_id = 0;
    if (result == ESP_OK) {
        // Durable state is committed (marker cleared, sessions wiped, deck +
        // scope generation saved). NOW install the in-memory half: the deck,
        // the session/card reset (clear_persisted=false -- the durable clears
        // were part of the worker transaction) and the note screen's [词] rows.
        wqn::SetDefaultWordDeck(
            &state_.word_app, settings.pending_word_deck_id,
            settings.pending_word_deck_title);
        settings.default_word_deck_title = state_.word_app.default_deck_title;
        wqn::ResetWordSessionsForScopeChange(&state_.word_app, false);
        RebuildNoteWordDeckRows(&state_);
        settings.notice = settings.pending_word_deck_id.empty()
            ? "默认词库：全部词库"
            : "默认词库：" + settings.pending_word_deck_title;
        settings.pending_word_deck_id.clear();
        settings.pending_word_deck_title.clear();
        settings.word_deck_pending_valid = false;
        ESP_LOGI(kTag, "wordbook change committed: id=%s",
                 state_.word_app.default_deck_id.empty()
                     ? "all"
                     : state_.word_app.default_deck_id.c_str());
    } else {
        // Keep the displayed (old) deck AND the armed pending pair so a
        // re-open preselects the intended deck and re-Confirm retries. If the
        // transaction died mid-way, the NVS marker makes boot recovery replay
        // it -- the UI never shows a half-switched state.
        settings.notice = "默认词库未保存，请重试";
        ESP_LOGW(kTag, "deck change failed: %s", esp_err_to_name(result));
    }
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kSettings
        ? RefreshSchedule::kConfig
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kSettingsPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchAiFollowSaveResult(esp_err_t result, uint32_t operation_id)
{
    wqn::SettingsAppState& settings = state_.settings;
    if (settings.auto_follow_save_op_id == 0 ||
        settings.auto_follow_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale ai follow save result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(settings.auto_follow_save_op_id));
        return FinishEvent(AppEventKind::kSettingsPersist,
                           RefreshSchedule::kNone, false);
    }
    settings.auto_follow_save_op_id = 0;
    if (result == ESP_OK) {
        settings.auto_follow = settings.pending_auto_follow;
        settings.auto_follow_pending_valid = false;
        // The follow step reads the worker's copy and every snapshot replaces
        // the UI copy, so the RAM effect has to go through the setter.
        wqn::SetAiAutoFollow(settings.auto_follow);
        settings.notice = settings.auto_follow ? "翻页已保存：开" : "翻页已保存：关";
    } else {
        settings.notice = "翻页设置未保存，请重试";
        ESP_LOGW(kTag, "ai follow save failed: %s", esp_err_to_name(result));
    }
    const RefreshSchedule refresh = state_.screen == wqn::UiScreen::kSettings
        ? RefreshSchedule::kConfig
        : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kSettingsPersist, refresh, true);
}

UiUpdate UiRuntime::DispatchAgentDetailSaveResult(
    esp_err_t result, uint32_t operation_id, int64_t now_ms)
{
    wqn::SettingsAppState& settings = state_.settings;
    if (settings.agent_detail_save_op_id == 0 ||
        settings.agent_detail_save_op_id != operation_id) {
        ESP_LOGW(kTag, "stale agent detail save result: op=%lu expected=%lu",
                 static_cast<unsigned long>(operation_id),
                 static_cast<unsigned long>(settings.agent_detail_save_op_id));
        return FinishEvent(AppEventKind::kSettingsPersist,
                           RefreshSchedule::kNone, false);
    }
    settings.agent_detail_save_op_id = 0;
    if (result == ESP_OK) {
        // Install what the worker actually wrote, not `desired`: the user can
        // keep cycling while the commit is in flight, and the pump picks the
        // newer value up on a later tick.
        settings.agent_detail_persisted = settings.agent_detail_inflight;
    } else {
        // Re-anchor so the retry waits a full debounce window; otherwise the
        // pump would re-submit on every UI tick while NVS keeps failing.
        settings.agent_detail_last_change_ms = now_ms;
        ESP_LOGW(kTag, "agent detail save failed: %s", esp_err_to_name(result));
    }
    return FinishEvent(AppEventKind::kSettingsPersist, RefreshSchedule::kNone, true);
}

UiUpdate UiRuntime::DispatchAgentDetailPersist(int64_t now_ms)
{
    wqn::SettingsAppState& settings = state_.settings;
    // Nothing to do while a write is in flight (the worker's per-kind busy gate
    // would reject a second one anyway), when the durable value already matches
    // the status bar, or while the value is still inside its debounce window.
    if (settings.agent_detail_save_op_id != 0 ||
        settings.agent_detail_desired == settings.agent_detail_persisted ||
        now_ms - settings.agent_detail_last_change_ms < kAgentDetailSaveDebounceMs) {
        return FinishEvent(AppEventKind::kSettingsPersist,
                           RefreshSchedule::kNone, false);
    }
    const uint8_t target = settings.agent_detail_desired;
    const uint32_t op_id = SubmitAgentDetailLevelSave(target);
    if (op_id == 0) {
        // Busy or pool-full: retry after another window, not on every tick.
        settings.agent_detail_last_change_ms = now_ms;
        return FinishEvent(AppEventKind::kSettingsPersist,
                           RefreshSchedule::kNone, true);
    }
    settings.agent_detail_inflight = target;
    settings.agent_detail_save_op_id = op_id;
    return FinishEvent(AppEventKind::kSettingsPersist,
                       RefreshSchedule::kNone, true);
}

UiUpdate UiRuntime::DispatchAgentSessionListPoll(int64_t now_ms)
{
    // [picker-stale] The picker's 运行中 marker is drawn from each row's
    // `outcome`, and g_state.sessions has exactly two writers: a list load, and
    // the terminal-status path that settles the ATTACHED session's row. With the
    // picker open the device is attached to nothing, so the second cannot run,
    // and both list-load paths are user gestures (tier cycle onto the tier with
    // no session locked, status-bar slot 1) that the open picker makes
    // unreachable -- it returns kHandled ahead of both. So the marker is a fixed
    // point: it can say 运行中 about a session that finished while the user was
    // reading the list, and nothing on the device can discover otherwise.
    //
    // The user then acts on it. In one direction they lock a session they believe
    // is live and watch it say nothing; in the other they skip one that just
    // started. The attach corrects both within a frame or two, but the marker
    // exists precisely so the user does not have to find out by locking (D-which).
    //
    // So the only writer that can reach this state is this tick. Conditions, in
    // order of how much they narrow it:
    //
    //   picker open, list non-empty, at least one row claims running
    //
    // The running-row gate is what bounds the cost: the poll fires at most a
    // couple of times per picker opening and then disarms itself permanently,
    // because a list with no running row is a list with no marker that can be
    // lying. A poll is a non-blocking arm (RequestOpenCodeSessionList takes
    // g_lock briefly and posts to the same single agent worker -- AGENTS.md §5:
    // no second task, no second TLS session), and when the worker already holds
    // the slot it degrades to a deferred switch whose tail runs the same load,
    // so a busy worker costs a retry rather than a lost refresh.
    //
    // The trade, stated plainly: while a load is in flight a confirm press is
    // refused with kWorkerBusy and only logged. That race already exists for the
    // user-initiated list load, and the running-row gate means a background load
    // can be in flight for a second or two at most a couple of times per picker
    // opening. A failed poll is invisible here on purpose -- DrawAgentSessionPicker
    // reads only the rows, and a list that has not been replaced is still the
    // list it was drawing.
    if (state_.screen != wqn::UiScreen::kAi ||
        state_.ai.tier != wqn::AiTier::kAgent ||
        state_.agent.session_locked ||
        state_.agent.sessions.empty()) {
        return FinishEvent(AppEventKind::kAgentListPoll, RefreshSchedule::kNone, false);
    }
    if (now_ms - agent_list_poll_at_ms_ < kAgentListPollIntervalMs) {
        return FinishEvent(AppEventKind::kAgentListPoll, RefreshSchedule::kNone, false);
    }
    bool any_running = false;
    for (const wqn::AgentSessionOption& option : state_.agent.sessions) {
        if (option.outcome == wqn::OpenCodeSessionOutcome::kRunning) {
            any_running = true;
            break;
        }
    }
    if (!any_running) {
        // [picker-stale] Disarmed for this picker opening: nothing is claiming a
        // run, so nothing can be lying. Not latched -- a later fetch may bring
        // the marker back, and it should be polled again if it does.
        return FinishEvent(AppEventKind::kAgentListPoll, RefreshSchedule::kNone, false);
    }
    // Set on attempt, not on success: a rejected poll (worker busy) must not
    // retry on the next tick, or a chain that outlives one interval spins.
    agent_list_poll_at_ms_ = now_ms;
    wqn::OpenCodeRejectReason reason = wqn::OpenCodeRejectReason::kNone;
    if (wqn::RequestOpenCodeSessionList(&reason) != ESP_OK) {
        ESP_LOGW(kTag, "Agent picker: stale-marker refresh refused (%s)",
                 AgentRejectLabel(reason));
        return FinishEvent(AppEventKind::kAgentListPoll, RefreshSchedule::kNone, false);
    }
    ESP_LOGI(kTag, "Agent picker: stale-marker refresh requested");
    // The next snapshot carries the new rows; the renderer's signature already
    // carries each row's running bit, so the repaint needs no extra push here.
    return FinishEvent(AppEventKind::kAgentListPoll, RefreshSchedule::kNone, false);
}

UiUpdate UiRuntime::DispatchTimeTick(int64_t now_ms)
{
    const wqn::TimeAppState before = state_.time_app;
    const int previous_progress_bucket = wqn::TimeAppVisualProgressBucket(before);
    const bool changed = wqn::TickTimeApp(&state_.time_app, now_ms);
    RefreshSchedule refresh = RefreshSchedule::kNone;
    if (changed) {
        RetainTimeAppState(state_.time_app);
        UpdateHomePrimaryTimeLine(&state_);
        const bool status_changed = state_.time_app.status != before.status;
        const bool action_focus_changed =
            state_.time_app.action_armed != before.action_armed;
        const bool endpoint_changed =
            state_.time_app.phase_ends_unix_seconds != before.phase_ends_unix_seconds ||
            state_.time_app.phase_started_unix_seconds != before.phase_started_unix_seconds;
        const bool progress_changed =
            wqn::TimeAppVisualProgressBucket(state_.time_app) != previous_progress_bucket;
        const bool visible_timer_page =
            state_.screen == wqn::UiScreen::kTime &&
            state_.time_app.tile != wqn::TimeTile::kClock &&
            !state_.time_app.config_mode;

        if (status_changed) {
            // Home only changes one plain status sentence; the timer page
            // changes its whole semantic composition at pause/end boundaries.
            refresh = state_.screen == wqn::UiScreen::kHome
                ? RefreshSchedule::kTimer
                : visible_timer_page ? RefreshSchedule::kCommit : RefreshSchedule::kNone;
        } else if (visible_timer_page && action_focus_changed &&
                   (progress_changed || endpoint_changed)) {
            // A focus timeout coinciding with a milestone redraw needs one
            // stable whole composition, not two competing partial intents.
            refresh = RefreshSchedule::kCommit;
        } else if (visible_timer_page && action_focus_changed) {
            refresh = RefreshSchedule::kSelection;
        } else if (visible_timer_page && (progress_changed || endpoint_changed)) {
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

// [follow] Per-tick auto-follow. See the declaration in ui_runtime.h and the
// [follow] block in ui_model.h for the setting's semantics: while a turn is in
// flight the viewport is pinned to the live tail so the user watches the
// execution; as soon as the answer body lands the follow retires -- the Agent
// parks on the answer's first line, STD/Pro leave the position to the
// conditional recenter at the end of the turn (ai_session.cpp kFinal).
UiUpdate UiRuntime::DispatchAiViewportFollow()
{
    constexpr AppEventKind kEvent = AppEventKind::kAiViewportFollow;
    if (state_.screen != wqn::UiScreen::kAi) {
        return FinishEvent(kEvent, RefreshSchedule::kNone, false);
    }

    const wqn::AiTier tier = state_.ai.tier;
    const bool agent_tier = tier == wqn::AiTier::kAgent;
    // Which history channel and which follow flags this tier owns. Flash never
    // arms the follow (it has no turn to watch), so the check below no-ops
    // there without needing its own branch.
    const wqn::AiHistoryChannel channel = agent_tier
        ? wqn::AiHistoryChannel::kAgent
        : (tier == wqn::AiTier::kFlash ? wqn::AiHistoryChannel::kFlash
                                       : wqn::AiHistoryChannel::kStdPro);
    const bool follow_active =
        agent_tier ? state_.agent.follow_active : state_.ai.follow_active;
    if (!state_.ai.auto_follow || !follow_active) {
        return FinishEvent(kEvent, RefreshSchedule::kNone, false);
    }

    // Capture owns the surface: the ES8311 is being configured and the panel
    // must not be repainted underneath it (the same policy that defers the
    // kPreparingCapture snapshot refresh above). The follow resumes with the
    // reply, which is what it exists to watch.
    if (agent_tier) {
        const wqn::AiFeaturePhase phase = state_.agent.ui.phase;
        if (phase == wqn::AiFeaturePhase::kLoading ||
            phase == wqn::AiFeaturePhase::kRecording ||
            phase == wqn::AiFeaturePhase::kTranscribing) {
            return FinishEvent(kEvent, RefreshSchedule::kNone, false);
        }
    } else if (state_.ai.status == wqn::AiSessionStatus::kPreparingCapture ||
               state_.ai.status == wqn::AiSessionStatus::kListening) {
        return FinishEvent(kEvent, RefreshSchedule::kNone, false);
    }

    const auto snapshot = wqn::GetAiHistorySnapshot(channel);
    if (!snapshot || snapshot->messages.empty()) {
        return FinishEvent(kEvent, RefreshSchedule::kNone, false);
    }
    int32_t min_scroll = 0;
    int32_t max_scroll = 0;
    // The Agent tier's band is only up while there is something to answer, so
    // the reserve the follow computes with is the one the renderer draws with.
    GetAiScrollBounds(snapshot, state_.ai.expand_content,
                      agent_tier ? AiBottomReserve(state_.agent) : 0, &min_scroll,
                      &max_scroll);

    // Has the answer body started? Both tiers now mirror their streamed text
    // straight into history, so the newest non-empty assistant entry IS the body
    // on either one and GetAiNewestAnswerTopOffsetLines can answer it directly.
    // assistant_partial stays in the predicate as the tier-independent fallback:
    // it is set unconditionally on every delta while the mirror rides the 50 ms
    // render watermark (ai_session.cpp), so on the tick a body starts it can be
    // non-empty up to a watermark period before the history write lands.
    // `answer_top` is still consumed only by the Agent tier -- the STD viewport
    // follow has no per-tier scroll offset of its own to park.
    int32_t answer_top = 0;
    const bool body_started =
        GetAiNewestAnswerTopOffsetLines(snapshot, state_.ai.expand_content,
                                        agent_tier ? AiBottomReserve(state_.agent) : 0,
                                        &answer_top) ||
        (!agent_tier && !state_.ai.assistant_partial.empty());

    if (body_started) {
        bool moved = false;
        if (agent_tier && state_.agent.ui.scroll_offset_lines != answer_top) {
            wqn::SetOpenCodeScrollOffsetClamped(answer_top, min_scroll, max_scroll);
            // Mirror into the UI copy: the renderer reads this field this very
            // tick, and the next snapshot would otherwise lag one frame behind.
            state_.agent.ui.scroll_offset_lines = answer_top;
            moved = true;
        }
        // Retire the follow for the rest of the turn, keeping user_moved as it
        // is: the follow retired itself, the user did not move the viewport.
        if (agent_tier) {
            wqn::SetOpenCodeFollowState(false, state_.agent.user_moved);
        } else {
            wqn::SetAiFollowState(false, state_.ai.user_moved);
        }
        ESP_LOGI(kTag, "ai-follow: body started -> retire (tier=%d parked=%d offset=%ld)",
                 static_cast<int>(tier), moved ? 1 : 0,
                 static_cast<long>(agent_tier ? state_.agent.ui.scroll_offset_lines
                                              : state_.ai.scroll_offset_lines));
        return FinishEvent(kEvent, moved ? RefreshSchedule::kAi : RefreshSchedule::kNone, true);
    }

    // No body yet: pin to the live tail (min_scroll), where the newest line
    // stays put as the run appends thinking/tool blocks above it.
    if (agent_tier) {
        if (state_.agent.ui.scroll_offset_lines == min_scroll) {
            return FinishEvent(kEvent, RefreshSchedule::kNone, false);
        }
        wqn::SetOpenCodeScrollOffsetClamped(min_scroll, min_scroll, max_scroll);
        state_.agent.ui.scroll_offset_lines = min_scroll;
    } else {
        if (state_.ai.scroll_offset_lines == min_scroll) {
            return FinishEvent(kEvent, RefreshSchedule::kNone, false);
        }
        wqn::SetAiScrollOffsetLinesClamped(min_scroll, min_scroll, max_scroll);
        state_.ai.scroll_offset_lines = min_scroll;
    }
    return FinishEvent(kEvent, RefreshSchedule::kAi, true);
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

UiUpdate UiRuntime::DispatchAgentSnapshot(const wqn::AgentSessionState& snapshot)
{
    const wqn::AiFeaturePhase previous_phase = state_.agent.ui.phase;
    state_.agent = snapshot;
    // Keep the logical state current while the codec is being initialized,
    // but avoid overlapping that hardware transaction with an EPD refresh.
    const bool defer_refresh_for_audio_init =
        snapshot.ui.phase == wqn::AiFeaturePhase::kLoading &&
        snapshot.ui.status_label == "准备录音";
    RefreshSchedule refresh = RefreshSchedule::kNone;
    // [agent] The Agent tier lives on the AI screen; kOpenCode is a reserved
    // unreachable screen ID.
    if (state_.screen == wqn::UiScreen::kAi &&
        state_.ai.tier == wqn::AiTier::kAgent &&
        !defer_refresh_for_audio_init) {
        refresh = snapshot.ui.phase == wqn::AiFeaturePhase::kComplete &&
                previous_phase != wqn::AiFeaturePhase::kComplete
            ? RefreshSchedule::kCommit
            : RefreshSchedule::kAi;
    }
    return FinishEvent(
        AppEventKind::kAgentSnapshot,
        refresh,
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
    // A status reload updates background data; it is not a navigation event.
    // Preserve the foreground screen even if a snapshot loader regresses and
    // supplies a stale/default screen value.
    snapshot.screen = state_.screen;
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
            break;
        case wqn::services::SyncEventStatus::kPartial:
            status = "部分完成，待重试";
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
    BuildHomeSummary(&state_);
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
