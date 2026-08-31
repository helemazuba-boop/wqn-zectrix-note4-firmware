// Button event dispatch: AI long-release, time-app editing repeats, settings dialog,
// per-screen input handling, todo/word side effects.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "persist_worker.h"

#include <string>
#include <utility>

#include "ai_session.h"
#include "esp_log.h"
#include "flash_session.h"
#include "opencode_session.h"
#include "power_manager.h"
#include "services/connectivity_service.h"
#include "services/sync_service.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

RefreshSchedule ApplySettingsButtonEvent(const wqn::ButtonEvent& event, wqn::UiState* state)
{
    if (state == nullptr || state->screen != wqn::UiScreen::kSettings || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }

    const bool short_press = event.type == wqn::ButtonEventType::kShortPress;
    const bool long_press = event.type == wqn::ButtonEventType::kLongPress;
    // [longpress-fix] Driver-marked auto-repeat; the old duration-based gate
    // (>=1150ms) let the 2nd repeat (650+260=910ms) through as a fresh long
    // press, so holds past ~910ms backed out two levels at once.
    const bool repeated_long_press = long_press && event.repeat;
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
            // [persist-worker] Async save (c4): the NVS commit used to run
            // synchronously here and could stall the UI behind a background
            // write storm. Arm the value first so a rejected submit or a write
            // failure keeps it for a re-Confirm; the displayed value and
            // schedule re-arm waits for the durable ACK. The per-kind busy
            // rejects a duplicate Confirm while one save is in flight.
            state->settings.pending_auto_sync_minutes = minutes;
            state->settings.auto_sync_pending_valid = true;
            const uint32_t op_id = SubmitAutoSyncIntervalSave(minutes);
            if (op_id != 0) {
                state->settings.auto_sync_save_op_id = op_id;
                state->settings.notice = "正在保存…";
            } else {
                state->settings.auto_sync_save_op_id = 0;
                state->settings.notice = IsPersistKindBusy(PersistKind::kSettingsAutoSync)
                    ? "正在保存，请稍后"
                    : "保存繁忙，请重试";
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
            // [persist-worker] Async save (c4), mirrors the auto-sync dialog.
            state->settings.pending_volume_percent = percent;
            state->settings.volume_pending_valid = true;
            const uint32_t op_id = SubmitVolumeSave(percent);
            if (op_id != 0) {
                state->settings.volume_save_op_id = op_id;
                state->settings.notice = "正在保存…";
            } else {
                state->settings.volume_save_op_id = 0;
                state->settings.notice = IsPersistKindBusy(PersistKind::kSettingsVolume)
                    ? "正在保存，请稍后"
                    : "保存繁忙，请重试";
            }
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kImageRendering) {
        if (short_press && event.button == wqn::ButtonId::kUp) {
            if (state->settings.image_render_selected == 0) {
                return RefreshSchedule::kNone;
            }
            --state->settings.image_render_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kDownPower) {
            if (state->settings.image_render_selected >= 1) {
                return RefreshSchedule::kNone;
            }
            ++state->settings.image_render_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            const wqn::ImageRenderMode mode =
                state->settings.image_render_selected == 0
                ? wqn::ImageRenderMode::kBlackWhite
                : wqn::ImageRenderMode::kGray16;
            state->settings.pending_image_render_mode = mode;
            state->settings.image_render_pending_valid = true;
            const uint32_t op_id = SubmitImageRenderModeSave(mode);
            if (op_id != 0) {
                state->settings.image_render_save_op_id = op_id;
                state->settings.notice = "正在保存…";
            } else {
                state->settings.image_render_save_op_id = 0;
                state->settings.notice =
                    IsPersistKindBusy(PersistKind::kSettingsImageRender)
                    ? "正在保存，请稍后"
                    : "保存繁忙，请重试";
            }
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kDefaultWordDeck) {
        auto& settings = state->settings;
        if (short_press && event.button == wqn::ButtonId::kUp) {
            if (settings.word_deck_selected == 0) {
                return RefreshSchedule::kNone;
            }
            --settings.word_deck_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kDownPower) {
            if (settings.word_deck_selected + 1 >= settings.word_deck_options.size()) {
                return RefreshSchedule::kNone;
            }
            ++settings.word_deck_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            if (settings.word_deck_selected < settings.word_deck_options.size() &&
                state->word_app.session.commit_state ==
                    wqn::WordObservationCommitState::kPersisting) {
                // A word answer is still pending (kPersisting spans Prepare ->
                // worker Apply, so it also covers the Prepare->reserve gap where
                // persist-busy is briefly false but the effect is still armed):
                // the deck switch clears both session files inside its worker
                // transaction and must not race the in-flight commit's session
                // save. Defer; user retries.
                settings.notice = "正在保存，请稍后切换";
            } else if (settings.word_deck_selected < settings.word_deck_options.size()) {
                const wqn::WordDeckInfo& option =
                    settings.word_deck_options[settings.word_deck_selected];
                // [deck-scope] Async switch via the worker's recoverable marker
                // protocol (c5). Arm the choice first (a rejected submit or a
                // failed transaction keeps it for a re-Confirm); NOTHING is
                // installed until the durable ACK -- the displayed deck, the
                // in-memory session reset and the [词] rows all follow in
                // DispatchDefaultDeckChangeResult.
                settings.pending_word_deck_id = option.deck_id;
                settings.pending_word_deck_title = option.title;
                settings.word_deck_pending_valid = true;
                const uint32_t op_id = SubmitDefaultDeckChange(option.deck_id);
                if (op_id != 0) {
                    settings.word_deck_save_op_id = op_id;
                    settings.notice = "正在保存…";
                } else {
                    settings.word_deck_save_op_id = 0;
                    settings.notice =
                        IsPersistKindBusy(PersistKind::kSettingsDefaultDeck)
                            ? "正在保存，请稍后"
                            : "保存繁忙，请重试";
                }
            }
            settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kWifiManage) {
        if (event.button == wqn::ButtonId::kConfirm && short_press) {
            // [wifi-redundancy] Route through the connectivity service rather
            // than the provisioning component directly, keeping the
            // UI -> services dependency direction.
            state->settings.dialog = wqn::SettingsDialog::kNone;
            state->settings.notice = "正在启动配网…";
            wqn::services::SetConnectivityProvisioning();
            return RefreshSchedule::kConfig;
        }
        if ((event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) &&
            short_press) {
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
            // [persist-worker] Defensive second line behind the main-page gate:
            // a factory reset erases NVS and reboots -- never do it while a
            // durable local write is still in flight on the persist worker.
            if (device_ui_internal::IsAnyPersistBusy()) {
                state->settings.notice = "正在保存，请稍后";
                return RefreshSchedule::kConfig;
            }
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

    if (state->settings.dialog == wqn::SettingsDialog::kPowerOff) {
        if (long_press && event.button == wqn::ButtonId::kConfirm) {
            // [power-fix] Hand off to the PowerCoordinator: it whites the
            // panel on the EPD owner task, quiesces services and cuts the
            // latch. The request re-arms itself while quiesce is busy, so
            // there is no user-visible failure path here; the notice stays
            // on the panel until the shutdown clear overwrites it.
            state->settings.dialog = wqn::SettingsDialog::kNone;
            state->settings.notice = "正在关机…";
            ESP_LOGW(kTag, "power off requested from settings page");
            wqn::RequestUserPowerOff();
            return RefreshSchedule::kCommit;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            state->settings.dialog = wqn::SettingsDialog::kNone;
            state->settings.notice = "已取消关机";
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

    // [persist-worker] Every main-page Confirm below either reads storage on the
    // UI task (OpenSettingsDialog -> UpdateSettingsDiagnostics, the firmware-
    // version row) or writes it synchronously (default word deck, factory
    // reset). While any local write is in flight -- IsAnyPersistBusy(), or the
    // wider commit_state==kPersisting window that also covers a domain's
    // Prepare->reserve gap -- that work would contend with or queue behind the
    // persist worker's transaction and re-stall the UI. Refuse the action with
    // a notice; nothing is opened, read or written.
    const bool persist_pending =
        device_ui_internal::IsAnyPersistBusy() ||
        state->word_app.session.commit_state ==
            wqn::WordObservationCommitState::kPersisting ||
        state->note_app.session.commit_state ==
            wqn::NoteObservationCommitState::kPersisting ||
        state->problem_app.commit_state ==
            wqn::ProblemVerdictCommitState::kPersisting;
    if (persist_pending) {
        state->settings.notice = "正在保存，请稍后";
        return RefreshSchedule::kConfig;
    }

    switch (state->settings.selected) {
        case 0:
            OpenSettingsDialog(state, wqn::SettingsDialog::kWifiManage);
            return RefreshSchedule::kConfig;
        case 1:
            wqn::services::RequestSyncNow();
            state->settings.sync_status = "已请求同步";
            state->settings.notice = "已请求同步";
            return RefreshSchedule::kConfig;
        case 2:
            OpenSettingsDialog(state, wqn::SettingsDialog::kAutoSync);
            return RefreshSchedule::kConfig;
        case 3:
            OpenSettingsDialog(state, wqn::SettingsDialog::kBattery);
            return RefreshSchedule::kConfig;
        case 4:
            OpenSettingsDialog(state, wqn::SettingsDialog::kStorage);
            return RefreshSchedule::kConfig;
        case 5:
            OpenSettingsDialog(state, wqn::SettingsDialog::kImageRendering);
            return RefreshSchedule::kConfig;
        case 6:
            OpenSettingsDialog(state, wqn::SettingsDialog::kVolume);
            return RefreshSchedule::kConfig;
        case 7:
            OpenSettingsDialog(state, wqn::SettingsDialog::kDefaultWordDeck);
            return RefreshSchedule::kConfig;
        case 8:
            UpdateSettingsDiagnostics(state);
            state->settings.notice = "固件 " + state->settings.diagnostics.firmware_version;
            return RefreshSchedule::kConfig;
        case 9:
            OpenSettingsDialog(state, wqn::SettingsDialog::kFactoryReset);
            return RefreshSchedule::kConfig;
        case 10:
            OpenSettingsDialog(state, wqn::SettingsDialog::kPowerOff);
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
    // components except Confirm when it owns a PTT gesture. This prevents
    // double-firing / double-paging bugs while retaining low-latency audio.
    const bool is_flash_ptt =
        state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        state->ai.tier == wqn::AiTier::kFlash;
    const bool is_agent_ptt =
        state->screen == wqn::UiScreen::kOpenCode &&
        event.button == wqn::ButtonId::kConfirm;

    if ((event.type == wqn::ButtonEventType::kPress ||
         event.type == wqn::ButtonEventType::kRelease ||
         event.type == wqn::ButtonEventType::kHoldPress) &&
        !is_flash_ptt && !is_agent_ptt) {
        return RefreshSchedule::kNone;
    }

    // Agent PTT is intentionally a two-step operation. Releasing this gesture
    // can only stop capture and start transcription; it cannot submit a prompt.
    // The resulting kAwaitingConfirmation state requires a separate Up press.
    if (is_agent_ptt &&
        (event.type == wqn::ButtonEventType::kPress ||
         event.type == wqn::ButtonEventType::kRelease ||
         event.type == wqn::ButtonEventType::kHoldPress)) {
        if (event.type == wqn::ButtonEventType::kPress) {
            state->gestures.agent_ptt_started = false;
            return RefreshSchedule::kNone;
        }
        if (event.type == wqn::ButtonEventType::kHoldPress) {
            if (wqn::StartOpenCodeVoiceInput() == ESP_OK) {
                state->gestures.agent_ptt_started = true;
                wqn::AgentSessionState snapshot;
                if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                    state->agent = std::move(snapshot);
                }
                return RefreshSchedule::kAi;
            }
            return RefreshSchedule::kNone;
        }
        if (state->gestures.agent_ptt_started) {
            state->gestures.agent_ptt_started = false;
            const esp_err_t result = wqn::StopOpenCodeVoiceInput();
            wqn::AgentSessionState snapshot;
            if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                state->agent = std::move(snapshot);
            }
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "Agent recording stop failed: %s", esp_err_to_name(result));
            }
            return RefreshSchedule::kAi;
        }
        return RefreshSchedule::kNone;
    }

    // button_input emits a derived click/long-release around the same physical
    // hold. Swallow it so one PTT gesture cannot also lock/send/navigate.
    if (state->screen == wqn::UiScreen::kOpenCode &&
        event.button == wqn::ButtonId::kConfirm &&
        state->gestures.agent_ptt_started &&
        (event.type == wqn::ButtonEventType::kShortPress ||
         event.type == wqn::ButtonEventType::kDoublePress ||
         event.type == wqn::ButtonEventType::kLongPress ||
         event.type == wqn::ButtonEventType::kLongRelease)) {
        return RefreshSchedule::kAi;
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
    const bool repeated_long_press = long_press && event.repeat;
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

    if (state->screen == wqn::UiScreen::kOpenCode) {
        const bool short_press = event.type == wqn::ButtonEventType::kShortPress;
        if (event.button == wqn::ButtonId::kConfirm &&
            (long_press || long_release)) {
            // PTT is handled by raw Hold/Release above. Never route its derived
            // long event to the generic "back to home" action.
            return RefreshSchedule::kAi;
        }
        if (short_press && !state->agent.session_locked &&
            (event.button == wqn::ButtonId::kUp ||
             event.button == wqn::ButtonId::kDownPower)) {
            const int direction = event.button == wqn::ButtonId::kUp ? -1 : 1;
            (void)wqn::MoveOpenCodeSessionSelection(direction);
            wqn::AgentSessionState snapshot;
            if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                state->agent = std::move(snapshot);
            }
            return RefreshSchedule::kAi;
        }
        if (short_press && !state->agent.session_locked &&
            event.button == wqn::ButtonId::kConfirm) {
            (void)wqn::LockSelectedOpenCodeSession();
            wqn::AgentSessionState snapshot;
            if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                state->agent = std::move(snapshot);
            }
            return RefreshSchedule::kAi;
        }
        if (short_press && state->agent.ui.phase == wqn::AiFeaturePhase::kAwaitingConfirmation &&
            (event.button == wqn::ButtonId::kUp ||
             event.button == wqn::ButtonId::kDownPower)) {
            if (event.button == wqn::ButtonId::kUp) {
                (void)wqn::ConfirmOpenCodePrompt(event_time_ms);
            } else {
                wqn::CancelOpenCodePrompt();
            }
            wqn::AgentSessionState snapshot;
            if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                state->agent = std::move(snapshot);
            }
            return RefreshSchedule::kAi;
        }
        if (short_press && state->agent.session_locked &&
            (event.button == wqn::ButtonId::kUp ||
             event.button == wqn::ButtonId::kDownPower)) {
            if (!wqn::AiFeaturePhaseIsBusy(state->agent.ui.phase) ||
                state->agent.ui.phase == wqn::AiFeaturePhase::kRunning) {
                wqn::ScrollOpenCodeResponse(
                    event.button == wqn::ButtonId::kUp ? 1 : -1);
                wqn::AgentSessionState snapshot;
                if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                    state->agent = std::move(snapshot);
                }
                return RefreshSchedule::kAi;
            }
            return RefreshSchedule::kNone;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            // A short Confirm is deliberately never an Agent send action.
            return RefreshSchedule::kNone;
        }
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
            state->ai.status == wqn::AiSessionStatus::kPreparingCapture ||
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
        state->ai.status != wqn::AiSessionStatus::kPreparingCapture &&
        state->ai.status != wqn::AiSessionStatus::kListening &&
        state->ai.status != wqn::AiSessionStatus::kWaitingReply &&
        state->ai.status != wqn::AiSessionStatus::kStreaming) {
        constexpr int32_t kScrollStepRows = 4;  // [scroll-2x] was 2; doubled per request
        const wqn::AiHistoryChannel channel = state->ai.tier == wqn::AiTier::kFlash
            ? wqn::AiHistoryChannel::kFlash
            : wqn::AiHistoryChannel::kStdPro;
        auto snapshot = wqn::GetAiHistorySnapshot(channel);

        // Fail open: without history there are no trustworthy bounds, so the
        // offset stays untouched (an empty snapshot must never reset scroll).
        if (snapshot != nullptr && !snapshot->messages.empty()) {
            int32_t min_scroll = 0;
            int32_t max_scroll = 0;
            device_ui_internal::GetAiScrollBounds(
                snapshot, state->ai.expand_content, &min_scroll, &max_scroll);
            const int32_t current = wqn::GetAiScrollOffsetLines();
            if (event.button == wqn::ButtonId::kUp) {
                // Up = "older" content above. Scroll band shifts content down.
                wqn::SetAiScrollOffsetLinesClamped(
                    current + kScrollStepRows, min_scroll, max_scroll);
            } else {
                // Down = "newer". Scroll band shifts content up.
                if (current <= min_scroll) {
                    wqn::StampScrollNoOpHint();
                }
                wqn::SetAiScrollOffsetLinesClamped(
                    current - kScrollStepRows, min_scroll, max_scroll);
            }
        } else if (event.button != wqn::ButtonId::kUp) {
            wqn::StampScrollNoOpHint();
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
         state->ai.status == wqn::AiSessionStatus::kPreparingCapture ||
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
    const size_t old_todo = state->todo.selected;
    const wqn::TimeAppState old_time_app = state->time_app;
    const std::string old_word_signature = wqn::WordAppSignature(state->word_app);
    const std::string old_note_signature = wqn::NoteAppSignature(state->note_app);
    const wqn::NoteAppMode old_note_mode = state->note_app.mode;
    const size_t old_notebook_window = state->note_app.notebook_window_start;
    const size_t old_note_list_window = state->note_app.note_list_window_start;
    const std::string old_problem_signature =
        wqn::ProblemAppSignature(state->problem_app);
    const bool old_problem_active = state->problem_app.active;
    const wqn::ProblemAppMode old_problem_mode = state->problem_app.mode;
    const size_t old_problem_segment = state->problem_app.ring_segment;
    const size_t old_problem_list_window = state->problem_app.list_window_start;
    ESP_LOGI(
        kTag,
        "button event: id=%d type=%d duration_ms=%lld",
        static_cast<int>(event.button),
        static_cast<int>(event.type),
        static_cast<long long>(event.duration_ms));
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
        if (old_screen == wqn::UiScreen::kTime) {
            wqn::DisarmTimeAppAction(&state->time_app);
        }
        state->gestures.flash_ptt_started = false;
        state->gestures.agent_ptt_started = false;
        state->gestures.last_ai_confirm_tap_ms = 0;
        if (state->screen == wqn::UiScreen::kOpenCode &&
            !state->agent.session_locked && state->agent.sessions.empty()) {
            (void)wqn::RequestOpenCodeSessionList();
            wqn::AgentSessionState snapshot;
            if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
                state->agent = std::move(snapshot);
            }
        } else if (state->screen == wqn::UiScreen::kTodo) {
            RefreshTodosFromCloud(state);
        } else if (state->screen == wqn::UiScreen::kWord && state->word_app.cloud_sync_requested) {
            wqn::services::RequestContentRefresh(
                wqn::services::SyncContentDomain::kWordPacks);
            if (!QueueWordReviewRefresh()) {
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "单词同步失败";
            } else {
                state->word_app.message = "单词同步中";
            }
        } else if (state->screen == wqn::UiScreen::kNote &&
                   state->note_app.cloud_sync_requested) {
            wqn::services::RequestContentRefresh(
                wqn::services::SyncContentDomain::kNotePacks);
            if (!QueueNotePackSync()) {
                state->note_app.message = IsNoteCloudBusy() ? "笔记同步中" : "笔记同步失败";
            } else {
                state->note_app.message = "笔记同步中";
            }
            // The mixed list also carries the [题] rows: give the problem
            // packs the same entry refresh (coalesced by the busy CAS).
            if (state->problem_app.cloud_sync_requested) {
                wqn::services::RequestContentRefresh(
                    wqn::services::SyncContentDomain::kProblemPacks);
                QueueProblemPackSync();
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
        if (state->time_app.action_armed != old_time_app.action_armed) {
            return RefreshSchedule::kSelection;
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
        // [persist-worker] The word observation commit (durable outbox append +
        // session-cursor snapshot) no longer runs synchronously here -- it used
        // to block the UI task on foreground storage. PumpWordObservationCommit
        // now hands it to the persist worker; the card stays in kPersisting
        // ("正在保存") until the worker's result is applied (advance card / retry)
        // on the UI task via DispatchWordObservationPersistResult.
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
    if (state->screen == wqn::UiScreen::kNote &&
        wqn::ProblemAppSignature(state->problem_app) != old_problem_signature) {
        BuildHomeSummary(state);
        // Layer activation, mode transitions (列表<->题目<->弹窗), ring segment
        // hops (题面<->图<->答案) and list viewport jumps repaint most of the
        // panel; commit them so the full refresh clears large-partial ghosting
        // (the note domain's SSD1683 lesson). In-face scrolling and verdict
        // highlight moves ride the fast partial path.
        if (state->problem_app.active != old_problem_active ||
            state->problem_app.mode != old_problem_mode ||
            state->problem_app.ring_segment != old_problem_segment ||
            state->problem_app.list_window_start != old_problem_list_window) {
            return RefreshSchedule::kCommit;
        }
        return RefreshSchedule::kSelection;
    }
    if (state->screen == wqn::UiScreen::kNote &&
        wqn::NoteAppSignature(state->note_app) != old_note_signature) {
        wqn::protocol::note_study_v1::CreateSessionRequest note_session_request;
        note_session_request.metadata = wqn::services::MakeDeviceRequestMetadata();
        if (wqn::TakeNoteSessionStartRequest(&state->note_app, &note_session_request) &&
            !QueueNoteSessionStart(note_session_request)) {
            wqn::CancelNoteSessionStartResult(&state->note_app);
            state->note_app.mode = wqn::NoteAppMode::kNotebookList;
            state->note_app.message = IsNoteCloudBusy()
                ? "笔记服务忙，请重试"
                : "打开失败，请重试";
        }
        // The note-open observation commit (outbox append + session snapshot,
        // a ~0.9s foreground storage transaction) no longer runs here on the
        // UI task: PumpNoteObservationCommit hands it to the persist worker
        // and the kPersisting gate covers the in-flight window.
        BuildHomeSummary(state);
        // A note mode transition (notebook<->title<->body<->image) or a list
        // viewport jump repaints most of the screen; do a full refresh so the
        // panel is cleared (large partial waveforms ghost, and HIL logs show a
        // windowed local partial issued right after such a full-frame partial
        // wedges the SSD1683 BUSY line for 4+ s). Every step inside the image
        // layer is a whole-frame change too, so it always commits. Same-window
        // navigation -- including body scroll -- uses the fast partial path;
        // body over-scroll is bounded by the reducer clamp so reverse-scroll
        // always reveals new content.
        if (state->note_app.mode != old_note_mode ||
            state->note_app.mode == wqn::NoteAppMode::kNoteImageView ||
            state->note_app.notebook_window_start != old_notebook_window ||
            state->note_app.note_list_window_start != old_note_list_window) {
            return RefreshSchedule::kCommit;
        }
        return RefreshSchedule::kSelection;
    }
    if (state->selected_home_task != old_home_task ||
        state->todo.selected != old_todo) {
        return RefreshSchedule::kSelection;
    }
    return RefreshSchedule::kNone;
}

}  // namespace device_ui_internal
