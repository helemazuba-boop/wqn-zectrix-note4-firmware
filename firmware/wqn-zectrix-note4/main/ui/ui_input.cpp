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

    // Tier switch on AI page: prev tier via kUp, next tier via kDownPower.
    // Accept any of these gestures:
    //   1. The original kDoublePress -- still works for fast double-taps.
    //   2. A plain kShortPress on the top page (page == 0). At page 0 there
    //      is no page-up/page-down action to conflict with, so the press
    //      can drive the tier switcher instead. This makes tier cycling
    //      reliable without depending on a 300 ms double-tap window.
    const bool on_ai_top_page =
        state->screen == wqn::UiScreen::kAi && state->ai.page == 0;
    const bool double_press_tier =
        event.type == wqn::ButtonEventType::kDoublePress &&
        state->screen == wqn::UiScreen::kAi;
    const bool short_press_tier =
        event.type == wqn::ButtonEventType::kShortPress && on_ai_top_page;
    const bool tier_switch_event =
        double_press_tier || short_press_tier;
    if (tier_switch_event &&
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
        // Sync the global tier into the UI state so the next FrameSignature
        // differs (otherwise the refresh dedup pipeline drops the redraw and
        // the user only sees the change after navigating away and back).
        state->ai.tier = new_tier;
        ESP_LOGI(kTag,
                 "AI tier switch: button=%d event_type=%d tier %d -> %d, schedule=kAi",
                 static_cast<int>(event.button),
                 static_cast<int>(event.type),
                 static_cast<int>(old_tier), static_cast<int>(new_tier));
        return RefreshSchedule::kAi;
    }
    // [tier-reset] When the user presses Up/DownPower on the AI top page
    // without a tier switch intent (none of the gestures above), it's a
    // no-op -- swallow the event so the page index doesn't get bumped and
    // we don't burn an unnecessary refresh.
    if (on_ai_top_page &&
        event.type == wqn::ButtonEventType::kShortPress &&
        (event.button == wqn::ButtonId::kUp ||
         event.button == wqn::ButtonId::kDownPower)) {
        return RefreshSchedule::kNone;
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
        return RefreshSchedule::kAi;
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
