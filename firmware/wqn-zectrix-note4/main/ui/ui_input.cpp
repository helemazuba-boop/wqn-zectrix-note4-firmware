// Button event dispatch: AI long-release, time-app editing repeats, settings dialog,
// per-screen input handling, todo/word side effects.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <string>

#include "ai_session.h"
#include "esp_log.h"
#include "flash_session.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr int64_t kRepeatedLongPressMinDurationMs = 1150;

RefreshSchedule ApplySettingsButtonEvent(const wqn::ButtonEvent& event, wqn::UiState* state)
{
    if (state == nullptr || state->screen != wqn::UiScreen::kSettings || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }

    const bool short_press = event.type == wqn::ButtonEventType::kShortPress;
    const bool long_press = event.type == wqn::ButtonEventType::kLongPress;
    const bool repeated_long_press = long_press && event.duration_ms >= kRepeatedLongPressMinDurationMs;
    if (repeated_long_press) {
        return RefreshSchedule::kNone;
    }

    if (event.type == wqn::ButtonEventType::kLongRelease) {
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kAutoSync) {
        if (short_press && event.button == wqn::ButtonId::kUp) {
            if (state->settings.auto_sync_selected == 0) {
                return RefreshSchedule::kNone;
            }
            --state->settings.auto_sync_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kDownPower) {
            if (state->settings.auto_sync_selected + 1 >= kAutoSyncOptionsCount) {
                return RefreshSchedule::kNone;
            }
            ++state->settings.auto_sync_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            const uint32_t minutes = kAutoSyncOptions[state->settings.auto_sync_selected];
            const esp_err_t result = wqn::SaveAutoSyncIntervalMinutes(minutes);
            if (result == ESP_OK) {
                state->settings.auto_sync_interval_min = minutes;
                state->settings.notice = "自动同步已保存：" + wqn::AutoSyncIntervalLabel(minutes);
                wqn::RequestOnlineSyncNow();
            } else {
                state->settings.notice = "自动同步保存失败";
                ESP_LOGW(kTag, "save auto sync interval failed: %s", esp_err_to_name(result));
            }
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kBattery ||
        state->settings.dialog == wqn::SettingsDialog::kStorage) {
        if (event.button == wqn::ButtonId::kConfirm && (short_press || long_press)) {
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kFactoryReset) {
        if (long_press && event.button == wqn::ButtonId::kConfirm) {
            state->settings.notice = "正在恢复出厂";
            ESP_LOGW(kTag, "factory reset requested from settings page");
            const esp_err_t reset_result = wqn::FactoryResetNvsAndRestart();
            state->settings.notice = "恢复失败";
            ESP_LOGE(kTag, "factory reset failed: %s", esp_err_to_name(reset_result));
            return RefreshSchedule::kCommit;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            state->settings.dialog = wqn::SettingsDialog::kNone;
            state->settings.notice = "已取消恢复出厂";
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (long_press && event.button == wqn::ButtonId::kConfirm) {
        state->screen = wqn::UiScreen::kHome;
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }
    if (long_press && event.button == wqn::ButtonId::kUp) {
        wqn::HandleUiInput(state, wqn::UiInput::kTopPrevious);
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }
    if (long_press && event.button == wqn::ButtonId::kDownPower) {
        wqn::HandleUiInput(state, wqn::UiInput::kTopNext);
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }

    if (!short_press) {
        return RefreshSchedule::kNone;
    }

    if (event.button == wqn::ButtonId::kUp) {
        if (state->settings.selected == 0) {
            state->settings.selected = kSettingsItemCount - 1;
        } else {
            --state->settings.selected;
        }
        return RefreshSchedule::kSelection;
    }
    if (event.button == wqn::ButtonId::kDownPower) {
        if (state->settings.selected + 1 >= kSettingsItemCount) {
            state->settings.selected = 0;
        } else {
            ++state->settings.selected;
        }
        return RefreshSchedule::kSelection;
    }
    if (event.button != wqn::ButtonId::kConfirm) {
        return RefreshSchedule::kNone;
    }

    switch (state->settings.selected) {
        case 0:
            wqn::RequestOnlineSyncNow();
            state->settings.sync_status = "已请求同步";
            state->settings.notice = "已请求同步";
            return RefreshSchedule::kConfig;
        case 1:
            OpenSettingsDialog(state, wqn::SettingsDialog::kAutoSync);
            return RefreshSchedule::kConfig;
        case 2:
            OpenSettingsDialog(state, wqn::SettingsDialog::kBattery);
            return RefreshSchedule::kConfig;
        case 3:
            OpenSettingsDialog(state, wqn::SettingsDialog::kStorage);
            return RefreshSchedule::kConfig;
        case 4:
            UpdateSettingsDiagnostics(state);
            state->settings.notice = "固件 " + state->settings.diagnostics.firmware_version;
            return RefreshSchedule::kConfig;
        case 5:
            OpenSettingsDialog(state, wqn::SettingsDialog::kFactoryReset);
            return RefreshSchedule::kConfig;
        default:
            return RefreshSchedule::kNone;
    }
}

RefreshSchedule ApplyButtonEvent(const wqn::ButtonEvent& event, wqn::UiState* state)
{
    if (state == nullptr || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }

    const size_t old_page = state->ai.page;
    const wqn::AiTier old_ai_tier = state->ai.tier;

    // [PTT-Filter-Fix] Filter out raw kPress/kRelease edge events for all UI
    // components except the Confirm button on the AI page when the Flash
    // voice tier is active. This prevents double-firing / double-paging bugs.
    const bool is_flash_ptt =
        state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        state->ai.tier == wqn::AiTier::kFlash;

    if ((event.type == wqn::ButtonEventType::kPress ||
         event.type == wqn::ButtonEventType::kRelease) &&
        !is_flash_ptt) {
        return RefreshSchedule::kNone;
    }

    // [ptt-fix] Edge events (kPress / kRelease) drive the Flash PTT path with
    // sub-50 ms latency. Detect them here before any long-press logic so the
    // confirm-button press triggers capture immediately, the release stops it
    // immediately, and the legacy kLongPress/kLongRelease paths are left
    // unused on the AI / Flash page (avoiding double-firing).
    if (state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        (event.type == wqn::ButtonEventType::kPress ||
         event.type == wqn::ButtonEventType::kRelease)) {
#if CONFIG_WQN_AI_ENABLE
        if (state->ai.tier == wqn::AiTier::kFlash) {
            if (event.type == wqn::ButtonEventType::kPress) {
                wqn::OnFlashButtonPressed();
            } else {
                wqn::OnFlashButtonReleased();
            }
            return RefreshSchedule::kAi;
        }
#endif
        // Non-Flash tier: let kPress translate to kConfirm (legacy behaviour)
        // by leaving the event unmodified and falling through. Other tiers
        // (STD / Pro) still rely on kLongPress to start recording.
    }

    const bool long_press = event.type == wqn::ButtonEventType::kLongPress;
    const bool long_release = event.type == wqn::ButtonEventType::kLongRelease;
    const bool repeated_long_press = long_press && event.duration_ms >= kRepeatedLongPressMinDurationMs;
    const bool time_value_edit_repeat =
        repeated_long_press && state->screen == wqn::UiScreen::kTime &&
        wqn::TimeAppIsEditingValue(state->time_app) &&
        (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower);
    const bool time_running_exit =
        long_press && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kTime &&
        wqn::TimeAppHasActiveTimer(state->time_app);
    if (state->screen == wqn::UiScreen::kSettings) {
        return ApplySettingsButtonEvent(event, state);
    }

    // Tier switch on AI page: double-press Up/Down within a WIDE window.
    // The global kDoublePressWindowMs (500 ms) in button_input.cpp is too
    // tight — users naturally wait 300-500 ms for the EPD refresh between
    // taps, so their second tap arrives >900 ms later and is never detected
    // as a kDoublePress. Instead of widening the global window (which would
    // affect other pages), we track the last AI scroll button + timestamp
    // here and treat a second short-press of the SAME button within 1 s as
    // a tier switch. We also catch kDoublePress (fast <500 ms) directly.
    static int64_t last_ai_tap_ms = 0;
    static wqn::ButtonId last_ai_tap_button = wqn::ButtonId::kNone;
    constexpr int64_t kAiTierSwitchWindowMs = 1000;

    // Fast path: button_input already detected a kDoublePress (<500 ms).
    if (state->screen == wqn::UiScreen::kAi &&
        event.type == wqn::ButtonEventType::kDoublePress &&
        (event.button == wqn::ButtonId::kUp ||
         event.button == wqn::ButtonId::kDownPower)) {
        const wqn::AiTier old_tier = wqn::GetAiTier();
        wqn::AiTier new_tier = old_tier;
        if (event.button == wqn::ButtonId::kUp) {
            new_tier = wqn::PrevAiTier(new_tier);
        } else {
            new_tier = wqn::NextAiTier(new_tier);
        }
        wqn::SetAiTier(new_tier);
        state->ai.tier = new_tier;
        wqn::RequestForceFullRefresh();
        ESP_LOGI(kTag, "AI tier switch (fast double-press): tier %d -> %d",
                 static_cast<int>(old_tier), static_cast<int>(new_tier));
        last_ai_tap_button = wqn::ButtonId::kNone;
        return RefreshSchedule::kAi;
    }

    // Slow path: two kShortPress events of the same button within 1 s.
    const bool is_ai_short_press =
        state->screen == wqn::UiScreen::kAi &&
        event.type == wqn::ButtonEventType::kShortPress &&
        (event.button == wqn::ButtonId::kUp ||
         event.button == wqn::ButtonId::kDownPower);

    if (is_ai_short_press) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (last_ai_tap_button == event.button &&
            (now_ms - last_ai_tap_ms) <= kAiTierSwitchWindowMs) {
            // Second tap of the same button within 1 s → tier switch
            const wqn::AiTier old_tier = wqn::GetAiTier();
            wqn::AiTier new_tier = old_tier;
            if (event.button == wqn::ButtonId::kUp) {
                new_tier = wqn::PrevAiTier(new_tier);
            } else {
                new_tier = wqn::NextAiTier(new_tier);
            }
            wqn::SetAiTier(new_tier);
            state->ai.tier = new_tier;
            wqn::RequestForceFullRefresh();
            ESP_LOGI(kTag,
                     "AI tier switch (slow double-tap): button=%d tier %d -> %d, dt=%lld ms",
                     static_cast<int>(event.button),
                     static_cast<int>(old_tier), static_cast<int>(new_tier),
                     static_cast<long long>(now_ms - last_ai_tap_ms));
            last_ai_tap_button = wqn::ButtonId::kNone;
            last_ai_tap_ms = 0;
            return RefreshSchedule::kAi;
        }
        // First tap (or different button / expired) → record and fall through
        // to normal scroll handling.
        last_ai_tap_button = event.button;
        last_ai_tap_ms = now_ms;
    }

    if (repeated_long_press && !time_value_edit_repeat && !time_running_exit) {
        return RefreshSchedule::kNone;
    }
    if (long_release && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kAi) {
#if CONFIG_WQN_AI_ENABLE
        // [ptt-fix] Flash tier uses the kRelease edge event for its stop hook
        // (delivered ~50 ms after the press transition), which arrives before
        // this kLongRelease. Calling OnFlashButtonReleased twice would
        // double-submit the audio buffer, so skip here.
        if (state->ai.tier == wqn::AiTier::kFlash) {
            return RefreshSchedule::kAi;
        }
        if (state->ai.status == wqn::AiSessionStatus::kListening ||
            state->ai.status == wqn::AiSessionStatus::kWaitingReply) {
            const esp_err_t ret = wqn::StopAiRecordingAndSubmit();
            wqn::AiSessionState ai_state;
            if (wqn::CopyAiSessionToUi(&ai_state)) {
                state->ai = ai_state;
            }
            if (ret != ESP_OK) {
                ESP_LOGW(kTag, "AI recording stop failed: %s", esp_err_to_name(ret));
                state->ai.status = wqn::AiSessionStatus::kError;
                state->ai.assistant_text = "AI 录音停止失败";
                state->ai.pending_text.clear();
                state->ai.status_since_ms = esp_timer_get_time() / 1000;
            }
            return RefreshSchedule::kAi;
        }
#else
        state->ai.status = wqn::AiSessionStatus::kWaitingReply;
        state->ai.status_since_ms = esp_timer_get_time() / 1000;
        if (state->ai.pending_text.empty()) {
            state->ai.pending_text = "AI 功能未启用";
        }
        return RefreshSchedule::kAi;
#endif
        return RefreshSchedule::kNone;
    }
    if (long_release) {
        return RefreshSchedule::kNone;
    }

    // -------------------------------------------------------------------
    // v2 AI scroll: short-press Up/Down on AI page drives the chat
    // viewport instead of paging through the assistant text. We do this
    // explicitly here (instead of letting it fall through to HandleUiInput)
    // so tier switch (kDoublePress) remains untouched and idle stays clean.
    //
    // Recording / waiting state must NOT scroll — the page is reserved for
    // the recording surface.
    // -------------------------------------------------------------------
    if (state->screen == wqn::UiScreen::kAi && !long_press &&
        (event.type == wqn::ButtonEventType::kShortPress) &&
        (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) &&
        state->ai.status != wqn::AiSessionStatus::kListening &&
        state->ai.status != wqn::AiSessionStatus::kWaitingReply &&
        state->ai.status != wqn::AiSessionStatus::kStreaming) {
        constexpr int32_t kScrollStepRows = 2;
        if (event.button == wqn::ButtonId::kUp) {
            // Up = "older" content above. Scroll band shifts content down.
            wqn::RequestAiScrollUp(kScrollStepRows);
        } else {
            // Down = "newer". Scroll band shifts content up.
            const int32_t before = state->ai.scroll_offset_lines;
            wqn::RequestAiScrollDown(kScrollStepRows);
            // [scroll-hint] If the user is already at the limit the scroll
            // is a no-op. Stamp a transient hint so the E-ink panel gives
            // visible feedback ("已最新") instead of looking frozen.
            state->ai.scroll_offset_lines = wqn::GetAiScrollOffsetLines();
            if (state->ai.scroll_offset_lines == before) {
                wqn::StampScrollNoOpHint();
            }
        }
        state->ai.scroll_offset_lines = wqn::GetAiScrollOffsetLines();
        wqn::AiSessionState updated;
        if (wqn::CopyAiSessionToUi(&updated)) {
            state->ai.scroll_no_op_hint_ms = updated.scroll_no_op_hint_ms;
            state->ai.toast_label = updated.toast_label;
            state->ai.toast_visible = updated.toast_visible;
        }
        ESP_LOGI(kTag, "AI scroll: button=%d step=%ld -> offset=%ld hint_ms=%lld",
                 static_cast<int>(event.button),
                 static_cast<long>(kScrollStepRows),
                 static_cast<long>(state->ai.scroll_offset_lines),
                 static_cast<long long>(state->ai.scroll_no_op_hint_ms));
        return RefreshSchedule::kAi;
    }
    // Block Up/Down while busy on AI page so the user cannot accidentally
    // scroll mid-recording or mid-stream.
    if (state->screen == wqn::UiScreen::kAi && !long_press &&
        (event.type == wqn::ButtonEventType::kShortPress) &&
        (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) &&
        (state->ai.status == wqn::AiSessionStatus::kListening ||
         state->ai.status == wqn::AiSessionStatus::kWaitingReply ||
         state->ai.status == wqn::AiSessionStatus::kStreaming)) {
        return RefreshSchedule::kNone;
    }
    // Short-press confirm on AI page is a no-op now (long-press starts,
    // long-release submits). Keep behavior symmetric with the no-scroll
    // case so the user doesn't get double-submits.
    if (state->screen == wqn::UiScreen::kAi && !long_press && !long_release &&
        event.type == wqn::ButtonEventType::kShortPress &&
        event.button == wqn::ButtonId::kConfirm) {
        return RefreshSchedule::kNone;
    }

    const wqn::UiScreen old_screen = state->screen;
    const size_t old_home_task = state->selected_home_task;
    const size_t old_problem = state->selected_problem;
    const size_t old_todo = state->todo.selected;
    const wqn::ReviewChoice old_review = state->selected_review;
    const wqn::TimeAppState old_time_app = state->time_app;
    const std::string old_word_signature = wqn::WordAppSignature(state->word_app);
    ESP_LOGI(
        kTag,
        "button event: id=%d type=%d duration_ms=%lld",
        static_cast<int>(event.button),
        static_cast<int>(event.type),
        static_cast<long long>(event.duration_ms));
    if (!long_press && !long_release && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kReviewScore) {
        return QueueSelectedReview(state);
    }
    if (!long_press && !long_release && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kTodo) {
        return CompleteSelectedTodo(state);
    }
    if (!long_press && !long_release && state->screen == wqn::UiScreen::kTodo && event.button == wqn::ButtonId::kUp &&
        state->todo.selected == 0 && state->todo.has_earlier && !state->todo.previous_cursor.empty()) {
        if (QueueTodoRefreshCursor(state->todo.previous_cursor)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
            state->todo.status_message = "Todo syncing";
            return RefreshSchedule::kSelection;
        }
    }
    if (!long_press && !long_release && state->screen == wqn::UiScreen::kTodo && event.button == wqn::ButtonId::kDownPower &&
        !state->todo.todos.empty() && state->todo.selected + 1 >= state->todo.todos.size() &&
        state->todo.has_later && !state->todo.next_cursor.empty()) {
        if (QueueTodoRefreshCursor(state->todo.next_cursor)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
            state->todo.status_message = "Todo syncing";
            return RefreshSchedule::kSelection;
        }
    }

    switch (event.button) {
        case wqn::ButtonId::kUp:
            if (long_press && state->screen == wqn::UiScreen::kTime && wqn::TimeAppIsEditingValue(state->time_app)) {
                wqn::HandleTimeAppInput(&state->time_app, wqn::TimeInput::kLongUp);
            } else {
                wqn::HandleUiInput(state, long_press ? wqn::UiInput::kTopPrevious : wqn::UiInput::kUp);
            }
            break;
        case wqn::ButtonId::kDownPower:
            if (long_press && state->screen == wqn::UiScreen::kTime && wqn::TimeAppIsEditingValue(state->time_app)) {
                wqn::HandleTimeAppInput(&state->time_app, wqn::TimeInput::kLongDown);
            } else {
                wqn::HandleUiInput(state, long_press ? wqn::UiInput::kTopNext : wqn::UiInput::kDown);
            }
            break;
        case wqn::ButtonId::kConfirm:
            if (time_running_exit) {
                wqn::HandleTimeAppInput(&state->time_app, wqn::TimeInput::kLongConfirm);
            } else {
                wqn::HandleUiInput(state, long_press ? wqn::UiInput::kLongConfirm : wqn::UiInput::kConfirm);
            }
            break;
        case wqn::ButtonId::kNone:
            return RefreshSchedule::kNone;
    }

    if (state->screen != old_screen) {
        if (state->screen == wqn::UiScreen::kTodo) {
            RefreshTodosFromCloud(state);
        } else if (state->screen == wqn::UiScreen::kWord && state->word_app.cloud_sync_requested) {
            if (!QueueWordReviewRefresh()) {
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "单词同步失败";
            } else {
                state->word_app.message = "单词同步中";
            }
        }
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }
    if (state->screen == wqn::UiScreen::kAi) {
        if (state->ai.page != old_page || state->ai.tier != old_ai_tier) {
            return RefreshSchedule::kAi;
        }
        return RefreshSchedule::kNone;
    }
    if (!SameTimeAppState(state->time_app, old_time_app)) {
        BuildHomeSummary(state);
        if (TimeAppStructureChanged(old_time_app, state->time_app)) {
            return RefreshSchedule::kCommit;
        }
        if (state->time_app.config_mode && old_time_app.config_mode) {
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kCommit;
    }
    if (state->screen == wqn::UiScreen::kWord &&
        wqn::WordAppSignature(state->word_app) != old_word_signature) {
        wqn::WqnWordReviewSubmission submission;
        std::string word;
        if (wqn::TakeWordReviewSubmission(&state->word_app, &submission, &word)) {
            if (!QueueWordReviewSubmit(submission, word)) {
                state->word_app.pending_submit_word_id = submission.word_id;
                state->word_app.pending_submit_outcome = submission.outcome;
                state->word_app.pending_submit_word = word;
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "单词同步失败";
            }
        }
        wqn::WqnWordSearchRequest search_request;
        if (wqn::TakeWordSearchRequest(&state->word_app, &search_request)) {
            if (!QueueWordSearch(search_request)) {
                state->word_app.search_pending = true;
                state->word_app.pending_search_query = search_request.query.empty() ? search_request.prefix : search_request.query;
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "在线搜索失败";
            }
        }
        wqn::WqnWordAiLookupRequest lookup_request;
        if (wqn::TakeWordAiLookupRequest(&state->word_app, &lookup_request)) {
            if (!QueueWordAiLookup(lookup_request)) {
                state->word_app.ai_lookup_pending = true;
                state->word_app.pending_ai_query = lookup_request.query.empty() ? lookup_request.prefix : lookup_request.query;
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "AI 查词失败";
            }
        }
        BuildHomeSummary(state);
        return RefreshSchedule::kSelection;
    }
    if (state->selected_home_task != old_home_task ||
        state->selected_problem != old_problem ||
        state->todo.selected != old_todo ||
        state->selected_review != old_review) {
        return RefreshSchedule::kSelection;
    }
    return RefreshSchedule::kNone;
}

}  // namespace device_ui_internal
