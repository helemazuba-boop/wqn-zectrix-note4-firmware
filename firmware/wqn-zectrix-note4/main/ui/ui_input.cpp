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
                wqn::services::RequestSyncNow();
            } else {
                state->settings.notice = "自动同步保存失败";
                ESP_LOGW(kTag, "save auto sync interval failed: %s", esp_err_to_name(result));
            }
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kVolume) {
        if (short_press && event.button == wqn::ButtonId::kUp) {
            if (state->settings.volume_selected == 0) {
                return RefreshSchedule::kNone;
            }
            --state->settings.volume_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kDownPower) {
            if (state->settings.volume_selected + 1 >= kVolumeOptionsCount) {
                return RefreshSchedule::kNone;
            }
            ++state->settings.volume_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            const int percent = kVolumeOptions[state->settings.volume_selected];
            const esp_err_t result = wqn::SaveVolumePercent(percent);
            if (result == ESP_OK) {
                state->settings.volume_percent = percent;
                state->settings.notice = "音量已保存：" + wqn::VolumeLabel(percent);
            } else {
                state->settings.notice = "音量保存失败";
                ESP_LOGW(kTag, "save volume failed: %s", esp_err_to_name(result));
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
            wqn::services::RequestSyncNow();
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
            OpenSettingsDialog(state, wqn::SettingsDialog::kVolume);
            return RefreshSchedule::kConfig;
        case 5:
            UpdateSettingsDiagnostics(state);
            state->settings.notice = "固件 " + state->settings.diagnostics.firmware_version;
            return RefreshSchedule::kConfig;
        case 6:
            OpenSettingsDialog(state, wqn::SettingsDialog::kFactoryReset);
            return RefreshSchedule::kConfig;
        default:
            return RefreshSchedule::kNone;
    }
}

// [shell] Cycle the AI status-bar toggle at `index`. Index 0=tier (handled in
// ApplyStatusBarEditEvent as an immediate switch), 1=thinking, 2=tts, 3=expand,
// 4=trash (also immediate, in ApplyStatusBarEditEvent). This fn only cycles 1-3.
static void CycleAiStatusBarToggle(wqn::UiState* state, uint8_t index)
{
    wqn::AiSessionState& ai = state->ai;
    switch (index) {
        case 1: {  // thinking: off->low->med->high->off
            int v = (static_cast<int>(ai.thinking_level) + 1) % static_cast<int>(wqn::ThinkingLevel::kCount);
            ai.thinking_level = static_cast<wqn::ThinkingLevel>(v);
            wqn::SetAiThinkingLevel(ai.thinking_level);
            break;
        }
        case 2: ai.tts_on = !ai.tts_on; wqn::SetAiTtsOn(ai.tts_on); break;
        case 3: ai.expand_content = !ai.expand_content; wqn::SetAiExpandContent(ai.expand_content); break;
        default: break;
    }
}

// [shell] Reverse of CycleAiStatusBarToggle (undo the last forward cycle). Used
// when a double-confirm turns the last single-cycle into a "save & exit" instead
// (the optimistic forward cycle is rolled back so the saved value is the one the
// user was looking at before the double-click).
static void CycleAiStatusBarToggleReverse(wqn::UiState* state, uint8_t index)
{
    wqn::AiSessionState& ai = state->ai;
    switch (index) {
        case 1: {  // thinking reverse (wrap high->med->low->off)
            int cnt = static_cast<int>(wqn::ThinkingLevel::kCount);
            int v = (static_cast<int>(ai.thinking_level) - 1 + cnt) % cnt;
            ai.thinking_level = static_cast<wqn::ThinkingLevel>(v);
            wqn::SetAiThinkingLevel(ai.thinking_level);
            break;
        }
        case 2: ai.tts_on = !ai.tts_on; wqn::SetAiTtsOn(ai.tts_on); break;
        case 3: ai.expand_content = !ai.expand_content; wqn::SetAiExpandContent(ai.expand_content); break;
        default: break;
    }
}

// [shell] Status-bar edit mode owns all button input on the AI page while active.
// short-confirm = cycle selected toggle; up/down = move selection (wrap 0..2);
// long-confirm (release) = exit. Edge events (Press/Release) are consumed so
// Flash PTT never fires mid-edit (though edit mode is only entered on Std/Pro).
static RefreshSchedule ApplyStatusBarEditEvent(
    const wqn::ButtonEvent& event,
    int64_t now_ms,
    wqn::UiState* state)
{
    if (event.button == wqn::ButtonId::kConfirm) {
        if (event.type == wqn::ButtonEventType::kShortPress) {
            // [tier] index 0 = cycle tier (immediate switch + exit, no double-click).
            if (state->status_edit.selected == 0) {
                wqn::AiTier prev_tier = state->ai.tier;
                wqn::AiTier next = wqn::NextAiTier(prev_tier);
                state->ai.tier = next;
                wqn::SetAiTier(next);  // swaps dual history + MarkChanged
                // [i2s-handoff] Leaving Flash tier must tear down its WS +
                // AudioStreamingTask + 常驻 I2S duplex channels, otherwise they
                // keep I2S_NUM_0 occupied and STD/Pro's InitI2s fails with
                // ESP_ERR_NOT_FOUND (-> 0 ms stale-listening submit). Mirrors
                // the screen-leave teardown in ui_model.cpp. STD/Pro->Flash
                // needs nothing: Flash starts lazily on first PTT and already
                // calls StopAudioPlayback to release the TX slot.
                if (prev_tier == wqn::AiTier::kFlash && next != wqn::AiTier::kFlash) {
                    wqn::StopFlashSession();
                }
                state->status_edit.active = false;
                state->status_edit.last_cycle_ms = 0;
                wqn::RequestForceFullRefresh();
                ESP_LOGI(kTag, "AI status-bar: tier switch -> %d", static_cast<int>(next));
                return RefreshSchedule::kAi;
            }
            // [trash] index 4 = clear-context action: clear + exit immediately.
            if (state->status_edit.selected == 4) {
                wqn::ClearAiConversationContext();
                state->status_edit.active = false;
                state->status_edit.last_cycle_ms = 0;
                wqn::RequestForceFullRefresh();
                ESP_LOGI(kTag, "AI status-bar: trash (clear context) + exit");
                return RefreshSchedule::kSelection;
            }
            constexpr int64_t kStatusBarEditDblMs = 400;
            // Double-confirm (2nd short-press within window of the last forward
            // cycle) = save & exit: undo the last cycle so the value saved is the
            // one BEFORE this double, then leave edit mode. Single short-press =
            // cycle forward (optimistic). Applies to toggles (1-3) only.
            if (state->status_edit.last_cycle_ms > 0 &&
                now_ms - state->status_edit.last_cycle_ms <= kStatusBarEditDblMs) {
                CycleAiStatusBarToggleReverse(state, state->status_edit.selected);
                state->status_edit.active = false;
                state->status_edit.last_cycle_ms = 0;
                ESP_LOGI(kTag, "AI status-bar edit: save & exit (double-confirm)");
                return RefreshSchedule::kSelection;
            }
            CycleAiStatusBarToggle(state, state->status_edit.selected);
            state->status_edit.last_cycle_ms = now_ms;
            state->status_edit.last_action_ms = now_ms;
            return RefreshSchedule::kSelection;
        }
        if (event.type == wqn::ButtonEventType::kLongRelease) {
            state->status_edit.active = false;
            state->status_edit.last_cycle_ms = 0;
            ESP_LOGI(kTag, "AI status-bar edit: exit (long-confirm)");
            return RefreshSchedule::kSelection;
        }
        return RefreshSchedule::kNone;  // consume kPress/kRelease/kLongPress
    }
    if (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) {
        if (event.type == wqn::ButtonEventType::kShortPress ||
            event.type == wqn::ButtonEventType::kLongPress) {
            const int dir = (event.button == wqn::ButtonId::kUp) ? -1 : 1;
            // Flash edit mode has only the tier button (index 0); STD/Pro have 0..4.
            const int max_idx = (state->ai.tier == wqn::AiTier::kFlash) ? 0 : 4;
            int s = static_cast<int>(state->status_edit.selected) + dir;
            if (s < 0) { s = max_idx; }
            if (s > max_idx) { s = 0; }
            state->status_edit.selected = static_cast<uint8_t>(s);
            state->status_edit.last_action_ms = now_ms;
            // A double-confirm is only meaningful for two presses on the
            // same toggle. Do not let a recent cycle on the previous icon
            // reverse a value that was never changed on the new selection.
            state->status_edit.last_cycle_ms = 0;
            return RefreshSchedule::kSelection;
        }
        return RefreshSchedule::kNone;
    }
    return RefreshSchedule::kNone;
}

RefreshSchedule ApplyButtonEvent(
    const wqn::ButtonEvent& event,
    int64_t event_time_ms,
    wqn::UiState* state)
{
    if (state == nullptr || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }

    // [shell] Status-bar edit mode intercepts all input on the AI page.
    if (state->screen == wqn::UiScreen::kAi && state->status_edit.active) {
        return ApplyStatusBarEditEvent(event, event_time_ms, state);
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
         event.type == wqn::ButtonEventType::kRelease ||
         event.type == wqn::ButtonEventType::kHoldPress) &&
        !is_flash_ptt) {
        return RefreshSchedule::kNone;
    }

#if CONFIG_WQN_AI_ENABLE
    // [barge-in] Flash TTS playback (ai.status == kReplyReady) is interruptible
    // by any DERIVED button event EXCEPT a single-click Up/Down (scrolls the
    // chat history while listening) and PTT (Confirm hold/release has its own
    // barge-in in OnFlashButtonPressed). Raw kPress/kRelease/kHoldPress edges
    // were filtered above, so they never reach here - previously they did, and
    // a Down-key kPress edge spuriously aborted playback on every scroll,
    // causing the audio stutter. Fall through so the button's normal action
    // still runs after silencing the speaker.
    if (state->screen == wqn::UiScreen::kAi &&
        state->ai.tier == wqn::AiTier::kFlash &&
        state->ai.status == wqn::AiSessionStatus::kReplyReady &&
        !(event.type == wqn::ButtonEventType::kShortPress &&
          (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower)) &&
        !(event.button == wqn::ButtonId::kConfirm &&
          (event.type == wqn::ButtonEventType::kHoldPress ||
           event.type == wqn::ButtonEventType::kRelease))) {
        wqn::AbortFlashPlayback();
    }
#endif

    // [mistouch/PTT] Flash capture starts only after the physical confirm key
    // remains held for 200ms (button_input's one-shot kHoldPress). Raw kPress is
    // a candidate only; short/double taps therefore never enter "识别". kRelease
    // submits only if kHoldPress actually started capture.
    if (state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        (event.type == wqn::ButtonEventType::kPress ||
         event.type == wqn::ButtonEventType::kRelease ||
         event.type == wqn::ButtonEventType::kHoldPress)) {
#if CONFIG_WQN_AI_ENABLE
        if (state->ai.tier == wqn::AiTier::kFlash) {
            if (event.type == wqn::ButtonEventType::kPress) {
                // Reset a stale candidate left by a screen/tier transition
                // before this physical press starts a new PTT gesture.
                state->gestures.flash_ptt_started = false;
                return RefreshSchedule::kNone;
            }
            if (event.type == wqn::ButtonEventType::kHoldPress) {
                wqn::OnFlashButtonPressed();
                state->gestures.flash_ptt_started = true;
                return RefreshSchedule::kAi;
            }
            if (event.type == wqn::ButtonEventType::kRelease &&
                state->gestures.flash_ptt_started) {
                state->gestures.flash_ptt_started = false;
                wqn::OnFlashButtonReleased(true);
                return RefreshSchedule::kAi;
            }
            return RefreshSchedule::kNone;  // raw press or short-tap release
        }
#endif
        // Non-Flash tiers ignore raw/hold edges. Their legacy long-press path
        // below still starts recording at kLongPress.
        return RefreshSchedule::kNone;
    }

    // A 200..999 ms Flash PTT hold is still classified by button_input as a
    // derived short/double press before its queued raw kRelease arrives. Do
    // not treat that derived event as a status-bar double-confirm: doing so
    // would enter edit mode and swallow the raw release, leaving capture on.
    if (state->screen == wqn::UiScreen::kAi &&
        state->ai.tier == wqn::AiTier::kFlash &&
        event.button == wqn::ButtonId::kConfirm && state->gestures.flash_ptt_started &&
        (event.type == wqn::ButtonEventType::kShortPress ||
         event.type == wqn::ButtonEventType::kDoublePress ||
         event.type == wqn::ButtonEventType::kLongRelease)) {
        return RefreshSchedule::kAi;
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

    // [tier] Tier switch moved to the status-bar edit mode (tier icon, button 0,
    // confirm cycles tier). The old double-press Up/Down tier switch is removed.

    // [shell] Double-press confirm on AI -> enter status-bar edit mode. Flash
    // works too: the mis-touch filter (<200ms) discards the PTT captures of the
    // two short taps, so no voice is submitted. Fast double-press (<500ms) is
    // caught as kDoublePress directly; slow (two kShortPress within 1s) below.
    constexpr int64_t kAiStatusBarEditWindowMs = 1000;
    if (state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        event.type == wqn::ButtonEventType::kDoublePress) {
        state->status_edit.active = true;
        state->status_edit.selected = 0;
        state->status_edit.last_action_ms = event_time_ms;
        state->status_edit.last_cycle_ms = 0;
        state->gestures.last_ai_confirm_tap_ms = 0;
        ESP_LOGI(kTag, "AI status-bar edit: enter (fast double-press)");
        return RefreshSchedule::kSelection;
    }
    if (state->screen == wqn::UiScreen::kAi &&
        event.type == wqn::ButtonEventType::kShortPress &&
        event.button == wqn::ButtonId::kConfirm) {
        if (state->gestures.last_ai_confirm_tap_ms > 0 &&
            event_time_ms - state->gestures.last_ai_confirm_tap_ms <=
            kAiStatusBarEditWindowMs) {
            state->status_edit.active = true;
            state->status_edit.selected = 0;
            state->status_edit.last_action_ms = event_time_ms;
            state->status_edit.last_cycle_ms = 0;
            state->gestures.last_ai_confirm_tap_ms = 0;
            ESP_LOGI(kTag, "AI status-bar edit: enter (double-confirm)");
            return RefreshSchedule::kSelection;
        }
        state->gestures.last_ai_confirm_tap_ms = event_time_ms;
        // fall through: first tap acts normally
    }

    // Flash PTT already started at kHoldPress (200ms); swallow legacy 1s
    // kLongPress repeats so HandleUiInput(kLongConfirm) cannot start/error a
    // second recording path. kLongRelease is likewise handled by raw kRelease.
    if (state->screen == wqn::UiScreen::kAi &&
        state->ai.tier == wqn::AiTier::kFlash &&
        event.button == wqn::ButtonId::kConfirm && long_press) {
        return RefreshSchedule::kAi;
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
                state->ai.status_since_ms = event_time_ms;
            }
            return RefreshSchedule::kAi;
        }
#else
        state->ai.status = wqn::AiSessionStatus::kWaitingReply;
        state->ai.status_since_ms = event_time_ms;
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
        constexpr int32_t kScrollStepRows = 4;  // [scroll-2x] was 2; doubled per request
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
        state->gestures.flash_ptt_started = false;
        state->gestures.last_ai_confirm_tap_ms = 0;
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
        wqn::protocol::word_study_v1::CreateSessionRequest session_request;
        session_request.metadata = wqn::services::MakeDeviceRequestMetadata();
        if (wqn::TakeWordSessionStartRequest(&state->word_app, &session_request) &&
            !QueueWordSessionStart(session_request)) {
            wqn::CancelWordSessionStartResult(&state->word_app);
            if (session_request.mode ==
                wqn::protocol::word_study_v1::Mode::kDictionary) {
                state->word_app.session.requested_mode =
                    wqn::protocol::word_study_v1::Mode::kDictionary;
                state->word_app.session.start_requested = true;
                state->word_app.message = "词典可浏览，记录稍后准备";
            } else {
                state->word_app.mode = wqn::WordAppMode::kHome;
                state->word_app.message = IsWordCloudBusy()
                    ? "单词服务忙，请重试"
                    : "本轮准备失败，请重试";
            }
        }
        wqn::DurableWordObservation observation;
        wqn::PersistedWordSession advanced_session;
        const auto observation_metadata = wqn::services::MakeDeviceRequestMetadata();
        std::string occurred_at = CurrentIsoTimestamp();
        if (occurred_at.empty()) {
            // The event must remain durable even before SNTP is available.
            // The server clamps implausible/future times when projecting it.
            occurred_at = "2024-01-01T00:00:00Z";
        }
        if (wqn::TakeWordObservationEffect(
                &state->word_app,
                observation_metadata.request_id,
                occurred_at,
                &observation,
                &advanced_session)) {
            const esp_err_t commit_result = wqn::CommitWordObservation(
                observation, advanced_session);
            wqn::ApplyWordObservationCommitResult(
                &state->word_app, commit_result);
            if (commit_result == ESP_OK) {
                // The durable outbox is the interaction boundary. Upload it
                // after a quiet period; a card action must never launch the
                // full bootstrap/problem/content sync pipeline.
                wqn::services::RequestWordOutboxUpload();
            } else {
                ESP_LOGW(
                    kTag,
                    "word observation local commit failed: %s",
                    esp_err_to_name(commit_result));
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
