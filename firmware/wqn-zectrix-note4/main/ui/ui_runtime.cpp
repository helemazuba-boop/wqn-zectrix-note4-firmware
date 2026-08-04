#include "ui_runtime.h"

#include <utility>

#include "esp_log.h"
#include "ui_internal.h"
#include "services/sync_service.h"
#include "storage.h"
#include "word_app.h"

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
    // Note cloud results (pack sync / session start / candidate page / image)
    // repaint the note screen when they change visible state; the observation
    // commit no longer rides this path (it went to the persist worker in c3).
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
    std::string* note_id, uint8_t* image_index, std::string* image_id,
    uint32_t* progress_generation)
{
    return wqn::TakeNoteImageRequest(
        &state_.note_app, note_id, image_index, image_id, progress_generation);
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
    // Mode/content transitions repaint most of the panel; commit here so the
    // full refresh clears any large-partial ghosting (same rule as note).
    const RefreshSchedule refresh =
        changed && state_.screen == wqn::UiScreen::kNote
            ? RefreshSchedule::kCommit
            : RefreshSchedule::kNone;
    return FinishEvent(AppEventKind::kProblemCloudResult, refresh, changed);
}

bool UiRuntime::TakeProblemImageRequest(
    std::string* problem_id,
    bool* is_solution,
    uint8_t* image_index,
    std::string* image_id)
{
    return wqn::TakeProblemImageRequest(
        &state_.problem_app, problem_id, is_solution, image_index, image_id);
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
        // Only a durably saved interval may kick a sync round.
        wqn::services::RequestSyncNow();
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
    // Scoped full-refresh request: handed back via UiUpdate.force_full so the
    // UI task applies it to the AI frame built this tick. NOT the global
    // RequestForceFullRefresh() one-shot -- that flag can be consumed by
    // whichever page renders next (e.g. after the user leaves AI), spending a
    // heavy SSD1683 full refresh on the wrong screen.
    bool ai_force_full = false;
    if (state_.screen == wqn::UiScreen::kAi && view.force_full_render) {
        visible_change = true;
        ai_force_full = true;
    }
    if (state_.screen == wqn::UiScreen::kAi) {
        changed = changed || state_.ai.status_detail != view.tool_label;
        state_.ai.status_detail = view.tool_label;
    }
    UiUpdate update = FinishEvent(
        AppEventKind::kAiStreamingSnapshot,
        visible_change ? RefreshSchedule::kAi : RefreshSchedule::kNone,
        changed);
    update.force_full = ai_force_full;
    return update;
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
            wqn::RefreshProblemOutboxState(&state_.problem_app);
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
