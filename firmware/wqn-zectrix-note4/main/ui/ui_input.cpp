// Button event dispatch: AI long-release, time-app editing repeats, settings dialog,
// per-screen input handling, todo/word side effects.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "persist_worker.h"

#include <string>
#include <utility>

#include "ai_history.h"
#include "ai_session.h"
#include "esp_log.h"
#include "flash_session.h"
#include "opencode_session.h"
#include "power_manager.h"
#include "runtime/sleep_diagnostics.h"
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

    if (state->settings.dialog == wqn::SettingsDialog::kAiFollow) {
        if (short_press && event.button == wqn::ButtonId::kUp) {
            if (state->settings.ai_follow_selected == 0) {
                return RefreshSchedule::kNone;
            }
            --state->settings.ai_follow_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kDownPower) {
            if (state->settings.ai_follow_selected >= 1) {
                return RefreshSchedule::kNone;
            }
            ++state->settings.ai_follow_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            // [ai-follow] The toggle left the status-bar cluster for this row, so
            // it now follows the same two-phase shape as the other settings rows:
            // arm the choice here, install it only when the durable ACK lands
            // (DispatchAiFollowSaveResult). The flag the follow step reads lives
            // on the worker's AiSessionState (the UI copy is replaced by every
            // snapshot), so SetAiAutoFollow is called from the ACK, never here.
            const bool choice = state->settings.ai_follow_selected == 0;
            state->settings.pending_auto_follow = choice;
            state->settings.auto_follow_pending_valid = true;
            const uint32_t op_id = SubmitAiFollowSave(choice);
            if (op_id != 0) {
                state->settings.auto_follow_save_op_id = op_id;
                state->settings.notice = "正在保存…";
            } else {
                state->settings.auto_follow_save_op_id = 0;
                state->settings.notice =
                    IsPersistKindBusy(PersistKind::kSettingsAiFollow)
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

    // [dev-diag] kBattery/kStorage/kDevInfo/kDevSync/kDevErrors/kSleepDiag
    // share the read-only dialog contract: confirm (short or long) closes;
    // up/down do nothing.
    if (state->settings.dialog == wqn::SettingsDialog::kBattery ||
        state->settings.dialog == wqn::SettingsDialog::kStorage ||
        state->settings.dialog == wqn::SettingsDialog::kDevInfo ||
        state->settings.dialog == wqn::SettingsDialog::kDevSync ||
        state->settings.dialog == wqn::SettingsDialog::kDevErrors ||
        state->settings.dialog == wqn::SettingsDialog::kSleepDiag) {
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

    // [dev-diag] Second-level dev list (DEV_DIAGNOSTICS.md §3). Reached only
    // from the root row kSettingsRowDevMenu; long-Confirm returns to the root
    // list instead of Home, and every Confirm opens a read-only dialog. Placed
    // after all dialog blocks so an open dev dialog keeps its own handling.
    if (state->settings.view == wqn::SettingsView::kDev) {
        if (long_press && event.button == wqn::ButtonId::kConfirm) {
            state->settings.view = wqn::SettingsView::kRoot;
            state->settings.notice.clear();
            return RefreshSchedule::kConfig;
        }
        if (!short_press) {
            return RefreshSchedule::kNone;
        }
        if (event.button == wqn::ButtonId::kUp) {
            state->settings.dev_selected =
                state->settings.dev_selected == 0 ? wqn::kDevItemCount - 1
                                                  : state->settings.dev_selected - 1;
            return RefreshSchedule::kSelection;
        }
        if (event.button == wqn::ButtonId::kDownPower) {
            state->settings.dev_selected =
                state->settings.dev_selected + 1 >= wqn::kDevItemCount
                    ? 0
                    : state->settings.dev_selected + 1;
            return RefreshSchedule::kSelection;
        }
        if (event.button != wqn::ButtonId::kConfirm) {
            return RefreshSchedule::kNone;
        }
        switch (state->settings.dev_selected) {
            case wqn::kDevRowDevInfo:
                OpenSettingsDialog(state, wqn::SettingsDialog::kDevInfo);
                return RefreshSchedule::kConfig;
            case wqn::kDevRowDevSync:
                OpenSettingsDialog(state, wqn::SettingsDialog::kDevSync);
                return RefreshSchedule::kConfig;
            case wqn::kDevRowDevErrors:
                OpenSettingsDialog(state, wqn::SettingsDialog::kDevErrors);
                return RefreshSchedule::kConfig;
            case wqn::kDevRowBatteryRaw:
                OpenSettingsDialog(state, wqn::SettingsDialog::kBattery);
                return RefreshSchedule::kConfig;
            case wqn::kDevRowStorage:
                OpenSettingsDialog(state, wqn::SettingsDialog::kStorage);
                return RefreshSchedule::kConfig;
            case wqn::kDevRowSleepDiag:
                // The on-panel view comes from the snapshot, which is filled by
                // UpdateSettingsDiagnostics. This additionally asks the power
                // coordinator to log the whole ring -- a bonus that only pays
                // off with a USB console attached, since the deferred dump
                // exists precisely because battery operation has no console.
                wqn::runtime::RequestSleepDiagnosticsDump();
                OpenSettingsDialog(state, wqn::SettingsDialog::kSleepDiag);
                return RefreshSchedule::kConfig;
            default:
                return RefreshSchedule::kNone;
        }
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
        case wqn::kSettingsRowWifi:
            OpenSettingsDialog(state, wqn::SettingsDialog::kWifiManage);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowSyncNow:
            wqn::services::RequestSyncNow();
            state->settings.sync_status = "已请求同步";
            state->settings.notice = "已请求同步";
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowAutoSync:
            OpenSettingsDialog(state, wqn::SettingsDialog::kAutoSync);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowBattery:
            // [dev-diag] The raw ADC numbers and the fit formula moved to the
            // second-level dev list (电量原始); the root row keeps the
            // user-facing percent and just reports it in the notice line.
            UpdateSettingsDiagnostics(state);
            state->settings.notice =
                "电量 " + std::to_string(state->settings.diagnostics.battery_percent) + "%";
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowImageRender:
            OpenSettingsDialog(state, wqn::SettingsDialog::kImageRendering);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowVolume:
            OpenSettingsDialog(state, wqn::SettingsDialog::kVolume);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowWordDeck:
            OpenSettingsDialog(state, wqn::SettingsDialog::kDefaultWordDeck);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowAiFollow:
            OpenSettingsDialog(state, wqn::SettingsDialog::kAiFollow);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowVersion:
            UpdateSettingsDiagnostics(state);
            state->settings.notice = "固件 " + state->settings.diagnostics.firmware_version;
            return RefreshSchedule::kConfig;
#if CONFIG_WQN_DEV_MENU_ENABLE
        case wqn::kSettingsRowDevMenu:
            // Snapshot before switching: the dev rows show the git commit, the
            // sync summary and the error/sleep counts, and
            // UpdateSettingsDiagnostics is the only place they are filled. It
            // also runs on the 60s reload, but taking one here keeps a freshly
            // opened list correct.
            UpdateSettingsDiagnostics(state);
            state->settings.view = wqn::SettingsView::kDev;
            state->settings.notice.clear();
            return RefreshSchedule::kConfig;
#endif
        case wqn::kSettingsRowFactoryReset:
            OpenSettingsDialog(state, wqn::SettingsDialog::kFactoryReset);
            return RefreshSchedule::kConfig;
        case wqn::kSettingsRowPowerOff:
            OpenSettingsDialog(state, wqn::SettingsDialog::kPowerOff);
            return RefreshSchedule::kConfig;
        default:
            return RefreshSchedule::kNone;
    }
}

// [detail] Cycle the Agent tier's cloud detail level (0 简要 / 1 标准 / 2 详细,
// the ?detail=N tier). The worker is written first -- it is the authority, and
// its setter also drops the transcript cache key so the next lock/observe
// re-reads the session at the new tier -- then the UI copy is mirrored so the
// same-tick render draws the new glyph instead of waiting up to a poll period
// for the snapshot (the snapshot confirms the mirror right after).
//
// The NVS write is NOT immediate: this is a status-bar value toggle with no
// Confirm, so a write per keypress would put an NVS commit behind every click
// (and a double-click undo would write a value the user just took back). Only
// the intent is recorded here; DispatchAgentDetailPersist submits it once the
// value has been stable for a debounce window, which collapses a run of cycles
// into one write of the value it settles on.
static void CycleAgentDetailLevel(wqn::UiState* state, int direction, int64_t now_ms)
{
    constexpr int kCount = 3;
    const int current = static_cast<int>(state->agent.detail_level);
    const int next = ((current + direction) % kCount + kCount) % kCount;
    state->agent.detail_level = static_cast<uint8_t>(next);
    wqn::SetOpenCodeDetailLevel(state->agent.detail_level);
    state->settings.agent_detail_desired = state->agent.detail_level;
    state->settings.agent_detail_last_change_ms = now_ms;
}

// [shell] Cycle the AI status-bar toggle at `index`. Index 0=tier (handled in
// ApplyStatusBarEditEvent as an immediate switch), 5=trash (also immediate
// there). This fn only cycles the value toggles: 1=thinking, 2=tts, 3=expand on
// STD/Pro, 4=详细程度 on the Agent tier (its 1..3 are immediate actions and
// return before this path).
static void CycleAiStatusBarToggle(wqn::UiState* state, uint8_t index, int64_t now_ms)
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
        // [detail] Agent-only value toggle: the three-step detail tier.
        case 4: CycleAgentDetailLevel(state, +1, now_ms); break;
        default: break;
    }
}

// [shell] Reverse of CycleAiStatusBarToggle (undo the last forward cycle). Used
// when a double-confirm turns the last single-cycle into a "save & exit" instead
// (the optimistic forward cycle is rolled back so the saved value is the one the
// user was looking at before the double-click).
static void CycleAiStatusBarToggleReverse(wqn::UiState* state, uint8_t index, int64_t now_ms)
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
        // [detail] Three states, so the reverse is a real step back rather than
        // a boolean flip -- the undo gesture has to land on the previous tier.
        // Reversing also re-anchors the debounce: if the undo lands back on the
        // persisted value the pump writes nothing at all.
        case 4: CycleAgentDetailLevel(state, -1, now_ms); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// [agent] Agent-tier input
//
// The Agent tier has two modal surfaces the STD/Pro tiers do not have, and both
// must own input outright while they are up -- otherwise the confirm key would
// start a PTT recording in the middle of a decision the user is trying to make.
// Everything they do not claim falls through to the ordinary Agent PTT / scroll
// paths at the bottom of ApplyButtonEvent.
// ---------------------------------------------------------------------------

// Pull the backend's authoritative Agent state into AppState. Every Agent
// action is asynchronous (a worker task performs the HTTP call), so the UI
// state is only ever a snapshot taken after the request was accepted.
static void SyncAgentSnapshot(wqn::UiState* state)
{
    wqn::AgentSessionState snapshot;
    if (wqn::CopyOpenCodeSessionToUi(&snapshot)) {
        state->agent = std::move(snapshot);
    }
}

// [follow] "Has this turn's answer body started?" -- the same predicate the
// per-tick follow step uses (UiRuntime::DispatchAiViewportFollow). The mirror
// side alone is not enough for STD/Pro: its streaming text lives in
// assistant_partial until the seal, so the newest history entry is still the
// previous turn's reply while the body is already on screen.
static bool AnswerBodyStarted(
    const wqn::AiSessionState& ai,
    const std::shared_ptr<const wqn::AiHistorySnapshot>& snapshot)
{
    int32_t probe = 0;
    if (device_ui_internal::GetAiNewestAnswerTopOffsetLines(
            snapshot, ai.expand_content, &probe)) {
        return true;
    }
    return ai.tier != wqn::AiTier::kAgent && !ai.assistant_partial.empty();
}

// Turn navigation: jump the viewport to the previous/next answer. Reuses the
// renderer's own layout pass (GetAiTurnJumpOffsetLines) so the jump target and
// the scroll clamp cannot disagree.
static bool ApplyAgentTurnJump(wqn::UiState* state, int direction)
{
    if (state == nullptr || direction == 0) {
        return false;
    }
    const auto snapshot = wqn::GetAiHistorySnapshot(wqn::AiHistoryChannel::kAgent);
    if (!snapshot || snapshot->messages.empty()) {
        return false;
    }
    int32_t next = 0;
    if (!device_ui_internal::GetAiTurnJumpOffsetLines(
            snapshot, state->ai.expand_content, state->agent.ui.scroll_offset_lines,
            direction, &next)) {
        return false;
    }
    // [follow] Write through the backend, not the UI copy: the next
    // CopyOpenCodeSessionToUi replaces the whole struct and would snap the
    // viewport back to the offset the worker last saw. A turn jump is also a
    // manual viewport move, so it retires the follow -- otherwise the next
    // follow tick would drag the viewport back to the tail.
    int32_t min_scroll = 0;
    int32_t max_scroll = 0;
    device_ui_internal::GetAiScrollBounds(
        snapshot, state->ai.expand_content, &min_scroll, &max_scroll);
    wqn::SetOpenCodeScrollOffsetClamped(next, min_scroll, max_scroll);
    wqn::SetOpenCodeFollowState(false, /*user_moved=*/true);
    state->agent.ui.scroll_offset_lines = next;
    state->agent.follow_active = false;
    state->agent.user_moved = true;
    return true;
}

// Runs the focused option-bar slot. 发送 submits the armed transcript,
// 重新输入 discards it, 同意/拒绝 answer the gateway's permission ask.
static RefreshSchedule ExecuteAgentOption(wqn::UiState* state,
                                          device_ui_internal::AgentOption option,
                                          int64_t now_ms)
{
    switch (option) {
        case device_ui_internal::AgentOption::kSend:
            if (wqn::ConfirmOpenCodePrompt(now_ms) == ESP_OK) {
                ESP_LOGI(kTag, "Agent option: send");
            } else {
                ESP_LOGW(kTag, "Agent option: send rejected (armed state moved on)");
            }
            break;
        case device_ui_internal::AgentOption::kReinput:
            wqn::CancelOpenCodePrompt();
            ESP_LOGI(kTag, "Agent option: reinput");
            break;
        case device_ui_internal::AgentOption::kApprove:
        case device_ui_internal::AgentOption::kDeny:
            if (wqn::ReplyPendingOpenCodePermission(
                    option == device_ui_internal::AgentOption::kApprove) == ESP_OK) {
                ESP_LOGI(kTag, "Agent option: permission %s",
                         option == device_ui_internal::AgentOption::kApprove ? "approve" : "deny");
            } else {
                ESP_LOGW(kTag, "Agent option: permission reply rejected");
            }
            break;
        case device_ui_internal::AgentOption::kCount:
        default:
            return RefreshSchedule::kNone;
    }
    // The pending state is gone the moment the action is accepted, so the focus
    // it indexed into is meaningless. Reset it rather than leaving it pointing
    // at a slot the next state may not have.
    state->agent_option.focused = 0;
    SyncAgentSnapshot(state);
    return RefreshSchedule::kAi;
}

// A question is not one of the two fixed AgentOption slots: its labels are data
// the gateway projected, so it is answered by slot index instead of by option.
// The index is already clamped by AgentQuestionFocusedSlot, so it can only ever
// name an option the bar is actually showing.
static RefreshSchedule ExecuteAgentQuestion(wqn::UiState* state, uint8_t focused)
{
    const int slot =
        device_ui_internal::AgentQuestionFocusedSlot(state->agent, focused);
    if (wqn::ReplyPendingOpenCodeQuestion(slot) == ESP_OK) {
        ESP_LOGI(kTag, "Agent option: question answer slot=%d", slot);
    } else {
        ESP_LOGW(kTag, "Agent option: question reply rejected (ask moved on?)");
        return RefreshSchedule::kNone;
    }
    state->agent_option.focused = 0;
    SyncAgentSnapshot(state);
    return RefreshSchedule::kAi;
}

static RefreshSchedule ApplyAgentOptionBarEvent(
    const wqn::ButtonEvent& event,
    int64_t now_ms,
    wqn::UiState* state,
    device_ui_internal::AgentOptionMode mode)
{
    if (event.button == wqn::ButtonId::kConfirm) {
        // Both the fast kDoublePress and a slow pair of kShortPress execute:
        // the second tap of a slow pair is the decision, and treating it as
        // "first tap" would strand the user on a focused-but-unexecuted bar.
        if (event.type == wqn::ButtonEventType::kShortPress ||
            event.type == wqn::ButtonEventType::kDoublePress) {
            if (mode == device_ui_internal::AgentOptionMode::kQuestion) {
                return ExecuteAgentQuestion(state, state->agent_option.focused);
            }
            return ExecuteAgentOption(
                state, device_ui_internal::AgentFocusedOption(mode, state->agent_option.focused),
                now_ms);
        }
        // Swallow everything else, including the PTT hold edges: recording is
        // not what the confirm key means while a decision is on screen.
        return RefreshSchedule::kNone;
    }
    if (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) {
        if (event.type == wqn::ButtonEventType::kShortPress) {
            // Focus flips between exactly two slots -- except for a question the
            // gateway projected a single option for, where moving the marker
            // would park it on a slot that does not exist.
            const int slots = (mode == device_ui_internal::AgentOptionMode::kQuestion)
                ? device_ui_internal::AgentQuestionSlotCount(state->agent)
                : 2;
            if (slots > 1) {
                state->agent_option.focused ^= 1u;
            }
            return RefreshSchedule::kAi;
        }
        return RefreshSchedule::kNone;
    }
    return RefreshSchedule::kNone;
}

// Session picker: confirm locks the focused session, a second confirm within
// the window re-attaches to its live stream, long-confirm creates a new one.
// Same gesture map the retired standalone page used, so nothing has to be
// relearned.
static RefreshSchedule ApplyAgentPickerEvent(
    const wqn::ButtonEvent& event,
    int64_t now_ms,
    wqn::UiState* state)
{
    constexpr int64_t kAgentPickerWindowMs = 1000;
    if (event.button == wqn::ButtonId::kConfirm) {
        if (event.type == wqn::ButtonEventType::kLongRelease) {
            if (wqn::CreateNewOpenCodeSession() == ESP_OK) {
                ESP_LOGI(kTag, "Agent picker: new session");
            } else {
                ESP_LOGW(kTag, "Agent picker: new session rejected");
            }
            SyncAgentSnapshot(state);
            return RefreshSchedule::kAi;
        }
        if (event.type == wqn::ButtonEventType::kShortPress) {
            if (state->gestures.last_agent_confirm_tap_ms > 0 &&
                now_ms - state->gestures.last_agent_confirm_tap_ms <= kAgentPickerWindowMs) {
                state->gestures.last_agent_confirm_tap_ms = 0;
                if (wqn::ObserveOpenCodeSession() == ESP_OK) {
                    ESP_LOGI(kTag, "Agent picker: observe");
                } else {
                    ESP_LOGW(kTag, "Agent picker: observe rejected");
                }
                SyncAgentSnapshot(state);
                return RefreshSchedule::kAi;
            }
            state->gestures.last_agent_confirm_tap_ms = now_ms;
            if (wqn::LockSelectedOpenCodeSession() == ESP_OK) {
                ESP_LOGI(kTag, "Agent picker: lock session");
                state->agent_option.focused = 0;
            } else {
                ESP_LOGW(kTag, "Agent picker: lock rejected (busy or empty list)");
            }
            SyncAgentSnapshot(state);
            return RefreshSchedule::kAi;
        }
        if (event.type == wqn::ButtonEventType::kDoublePress) {
            state->gestures.last_agent_confirm_tap_ms = 0;
            if (wqn::ObserveOpenCodeSession() == ESP_OK) {
                ESP_LOGI(kTag, "Agent picker: observe (fast)");
            } else {
                ESP_LOGW(kTag, "Agent picker: observe rejected");
            }
            SyncAgentSnapshot(state);
            return RefreshSchedule::kAi;
        }
        return RefreshSchedule::kNone;
    }
    if (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) {
        if (event.type == wqn::ButtonEventType::kShortPress) {
            const int direction = (event.button == wqn::ButtonId::kUp) ? -1 : 1;
            if (wqn::MoveOpenCodeSessionSelection(direction) == ESP_OK) {
                SyncAgentSnapshot(state);
                return RefreshSchedule::kAi;
            }
        }
        return RefreshSchedule::kNone;
    }
    return RefreshSchedule::kNone;
}

// Returns kHandled only when a modal Agent surface consumed the event, and
// reports what that surface actually did through `out_schedule` -- an event the
// surface swallowed without changing anything must not cost a refresh.
enum class AgentInputResult : uint8_t { kFallThrough, kHandled };

static AgentInputResult TryApplyAgentAiButtonEvent(
    const wqn::ButtonEvent& event,
    int64_t event_time_ms,
    wqn::UiState* state,
    RefreshSchedule* out_schedule)
{
    if (state->screen != wqn::UiScreen::kAi ||
        state->ai.tier != wqn::AiTier::kAgent) {
        return AgentInputResult::kFallThrough;
    }
    const device_ui_internal::AgentOptionMode mode =
        device_ui_internal::AgentOptionModeFor(state->agent);
    if (mode != device_ui_internal::AgentOptionMode::kNone) {
        *out_schedule = ApplyAgentOptionBarEvent(event, event_time_ms, state, mode);
        return AgentInputResult::kHandled;
    }
    if (!state->agent.session_locked) {
        *out_schedule = ApplyAgentPickerEvent(event, event_time_ms, state);
        return AgentInputResult::kHandled;
    }
    // [interrupt] A submitted run has no option bar -- there is nothing to
    // decide -- so its cancel gesture has to live here, on long-confirm. It is
    // deliberately not a short press: the confirm key is the PTT gesture on this
    // page, and a short press during a run must stay free for the next prompt.
    // CancelOpenCodePrompt already routes here when no stream is attached yet.
    if (event.button == wqn::ButtonId::kConfirm &&
        event.type == wqn::ButtonEventType::kLongRelease &&
        state->agent.stream_active) {
        wqn::InterruptOpenCodeRun();
        ESP_LOGI(kTag, "Agent run: interrupt requested");
        SyncAgentSnapshot(state);
        *out_schedule = RefreshSchedule::kAi;
        return AgentInputResult::kHandled;
    }
    return AgentInputResult::kFallThrough;
}

// [shell] Status-bar edit mode owns all button input on the AI page while active.
// short-confirm = cycle selected toggle; up/down = move selection (wrap 0..max);
// long-confirm (release) = exit. Edge events (Press/Release) are consumed so
// Flash PTT never fires mid-edit (though edit mode is only entered on Std/Pro).
//
// [agent] The Agent tier reuses the same slot numbering (0 = tier, 1..5 =
// cluster) with one more slot than STD/Pro (which has 0..4 since the follow
// toggle moved to the settings page). Slots 1..3 are immediate actions rather
// than toggles, because they configure the gateway rather than the STD/Pro text
// turn; slot 4 is its detail-tier cycle and slot 5 its trash, where STD/Pro's
// slot 4 is the trash.
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
                // [agent] Arriving on the Agent tier with no locked session shows
                // the picker, so fetch the list now rather than making the user
                // press the session button first. Failure is not fatal: the
                // picker renders the backend's last activity text instead.
                if (next == wqn::AiTier::kAgent && state->agent.current_session_id.empty()) {
                    if (wqn::RequestOpenCodeSessionList() != ESP_OK) {
                        ESP_LOGW(kTag, "AI status-bar: agent session list request failed");
                    }
                }
                // A stale option-bar focus must not survive a tier change: the
                // pending state it indexed into belongs to the previous tier.
                state->agent_option.focused = 0;
                state->status_edit.active = false;
                state->status_edit.last_cycle_ms = 0;
                wqn::RequestForceFullRefresh();
                ESP_LOGI(kTag, "AI status-bar: tier switch -> %d", static_cast<int>(next));
                return RefreshSchedule::kAi;
            }
            // [agent] Slots 1..3 are immediate actions on the Agent tier (open
            // the session picker, jump to the previous/next answer). They run
            // and exit like the tier and trash slots instead of arming a toggle
            // double-confirm, which is what STD/Pro needs for its value toggles.
            if (state->ai.tier == wqn::AiTier::kAgent &&
                state->status_edit.selected >= 1 && state->status_edit.selected <= 3) {
                const int slot = state->status_edit.selected;
                state->status_edit.active = false;
                state->status_edit.last_cycle_ms = 0;
                state->status_edit.last_action_ms = now_ms;
                wqn::RequestForceFullRefresh();
                if (slot == 1) {
                    // [agent] Only open the picker when the list request was
                    // accepted: a picker over an in-flight command could lock a
                    // session the worker is not ready to backfill, and Observe
                    // is only reachable from the picker.
                    if (wqn::RequestOpenCodeSessionList() == ESP_OK) {
                        state->agent.session_locked = false;
                        state->agent.selected_session = 0;
                        ESP_LOGI(kTag, "AI status-bar: agent session picker");
                    } else {
                        ESP_LOGW(kTag, "AI status-bar: agent session list request failed");
                    }
                    return RefreshSchedule::kAi;
                }
                const int direction = (slot == 2) ? -1 : 1;
                if (ApplyAgentTurnJump(state, direction)) {
                    ESP_LOGI(kTag, "AI status-bar: agent turn jump dir=%d -> %ld",
                             direction,
                             static_cast<long>(state->agent.ui.scroll_offset_lines));
                }
                return RefreshSchedule::kAi;
            }
            // [trash] Clear-context action: index 5 on the Agent tier, 4 on
            // STD/Pro (its cluster is one slot shorter). Clear + exit.
            const uint8_t trash_index =
                (state->ai.tier == wqn::AiTier::kAgent) ? 5 : 4;
            if (state->status_edit.selected == trash_index) {
                // [agent] Each tier owns its own history channel, so the trash
                // clears the one the visible tier writes to.
                if (state->ai.tier == wqn::AiTier::kAgent) {
                    wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
                } else {
                    wqn::ClearAiConversationContext();
                }
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
            // cycle forward (optimistic). Applies to the value toggles only
            // (1-3 on STD/Pro, 4 on Agent); the immediate slots return earlier.
            if (state->status_edit.last_cycle_ms > 0 &&
                now_ms - state->status_edit.last_cycle_ms <= kStatusBarEditDblMs) {
                CycleAiStatusBarToggleReverse(state, state->status_edit.selected, now_ms);
                state->status_edit.active = false;
                state->status_edit.last_cycle_ms = 0;
                ESP_LOGI(kTag, "AI status-bar edit: save & exit (double-confirm)");
                return RefreshSchedule::kSelection;
            }
            CycleAiStatusBarToggle(state, state->status_edit.selected, now_ms);
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
            // Flash edit mode has only the tier button (index 0); STD/Pro has
            // 0..4 (tier + four cluster slots) and the Agent tier 0..5 (tier +
            // five, slot 4 being its detail control).
            const int max_idx = state->ai.tier == wqn::AiTier::kFlash ? 0
                : state->ai.tier == wqn::AiTier::kAgent ? 5
                : 4;
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

    // [agent] Modal Agent surfaces take priority over everything below: the
    // option bar (dashed pending bubble) and the session picker both answer the
    // confirm key with a decision, and neither may be interrupted by a PTT hold
    // or a scroll event. The sub-handlers' own schedule is returned verbatim so
    // a swallowed edge (kPress/kRelease while the bar is up) costs no EPD
    // refresh.
    RefreshSchedule agent_schedule = RefreshSchedule::kNone;
    if (TryApplyAgentAiButtonEvent(event, event_time_ms, state, &agent_schedule) ==
        AgentInputResult::kHandled) {
        return agent_schedule;
    }

    const size_t old_page = state->ai.page;
    const wqn::AiTier old_ai_tier = state->ai.tier;

    // [PTT-Filter-Fix] Filter out raw kPress/kRelease edge events for all UI
    // components except Confirm when it owns a PTT gesture. This prevents
    // double-firing / double-paging bugs while retaining low-latency audio.
    // [agent] Agent PTT lives on the AI page as AiTier::kAgent. It keeps the
    // same raw Press/Hold/Release gesture as Flash (hold to record, release to
    // stop and transcribe) because the Agent tier must stop at
    // kAwaitingConfirmation instead of submitting like the STD long-release
    // path does. The two PTT guards are mutually exclusive by construction:
    // only one tier can be selected at a time.
    const bool is_flash_ptt =
        state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        state->ai.tier == wqn::AiTier::kFlash;
    const bool is_agent_ptt =
        state->screen == wqn::UiScreen::kAi &&
        event.button == wqn::ButtonId::kConfirm &&
        state->ai.tier == wqn::AiTier::kAgent;

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
    // hold. Swallow it so one PTT gesture cannot also enter the status-bar edit
    // mode or scroll. [agent] The Agent tier shares this guard with Flash; both
    // own the same raw Hold/Release gesture.
    if (state->screen == wqn::UiScreen::kAi &&
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

    // [agent] The Agent tier scrolls its own history. ScrollOpenCodeResponse
    // owns the offset (the auto-follow policy may also move it) -- so the UI
    // must only ask it to move, never write the offset directly. The phase test
    // is the shared enum rather than AiSessionStatus, which the Agent tier
    // never populates.
    //
    // [scroll-anytime] Only the capture phases own the surface. A run in flight
    // (kSubmitting/kRunning and the asks) and the history backfill (kLoading)
    // stay scrollable: blocking kRunning made the newest reply unreadable for
    // the whole run. Capture is swallowed HERE (return kNone) so the event
    // cannot fall through to HandleUiInput and dirty ai.page.
    if (state->screen == wqn::UiScreen::kAi && state->ai.tier == wqn::AiTier::kAgent &&
        !long_press && event.type == wqn::ButtonEventType::kShortPress &&
        (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower)) {
        if (state->agent.ui.phase == wqn::AiFeaturePhase::kRecording ||
            state->agent.ui.phase == wqn::AiFeaturePhase::kTranscribing) {
            return RefreshSchedule::kNone;
        }
        // Up = older content above; the sign matches ScrollOpenCodeResponse's
        // own convention (positive = older).
        const int direction = (event.button == wqn::ButtonId::kUp) ? 1 : -1;
        // [scroll-clamp] Same bounds pass as the STD/Pro branch below, and for
        // the same reason: the clamp MUST come from the layout that is actually
        // drawn or the viewport desyncs from its own bounds. It matters more
        // here -- min_scroll is NEGATIVE whenever the newest exchange is taller
        // than the viewport, and a floor of 0 makes the tail of the newest reply
        // permanently unreachable. The backend used to clamp at exactly 0.
        const auto snapshot = wqn::GetAiHistorySnapshot(wqn::AiHistoryChannel::kAgent);
        if (snapshot == nullptr || snapshot->messages.empty()) {
            return RefreshSchedule::kNone;  // nothing laid out, nothing to clamp
        }
        int32_t min_scroll = 0;
        int32_t max_scroll = 0;
        device_ui_internal::GetAiScrollBounds(
            snapshot, state->ai.expand_content, &min_scroll, &max_scroll);
        // [follow] A manual scroll is the user taking the viewport back: the
        // follow retires for the rest of the turn and the conditional recenter
        // must leave the viewport where the user put it. The one exception is a
        // Down press that lands back on the tail before the answer body has
        // started -- that is the user asking to watch the run again, so the
        // follow is re-armed (user_moved stays true: they did move it).
        const bool body_started = AnswerBodyStarted(state->ai, snapshot);
        wqn::ScrollOpenCodeResponse(direction, min_scroll, max_scroll);
        SyncAgentSnapshot(state);
        const bool resumed =
            state->ai.auto_follow && direction < 0 &&
            state->agent.ui.scroll_offset_lines == min_scroll && !body_started;
        wqn::SetOpenCodeFollowState(resumed, /*user_moved=*/true);
        state->agent.follow_active = resumed;
        state->agent.user_moved = true;
        ESP_LOGI(kTag, "Agent scroll: %s -> offset=%ld bounds=[%ld,%ld] follow=%d",
                 direction > 0 ? "older" : "newer",
                 static_cast<long>(state->agent.ui.scroll_offset_lines),
                 static_cast<long>(min_scroll),
                 static_cast<long>(max_scroll),
                 resumed ? 1 : 0);
        return RefreshSchedule::kAi;
    }

    // -------------------------------------------------------------------
    // v2 AI scroll: short-press Up/Down on AI page drives the chat
    // viewport instead of paging through the assistant text. We do this
    // explicitly here (instead of letting it fall through to HandleUiInput)
    // so tier switch (kDoublePress) remains untouched and idle stays clean.
    //
    // [scroll-anytime] Only capture owns the surface. kWaitingReply and
    // kStreaming MUST scroll: that is exactly when the reply grows past the
    // bottom of the viewport. Capture is swallowed HERE (return kNone) so the
    // event cannot fall through to HandleUiInput and dirty ai.page.
    //
    // [agent] STD/Pro/Flash only: the Agent tier is handled by its own branch
    // above, which routes through ScrollOpenCodeResponse instead of the
    // shared AiSession offset that AiSessionState::scroll_offset_lines owns.
    // -------------------------------------------------------------------
    if (state->screen == wqn::UiScreen::kAi && state->ai.tier != wqn::AiTier::kAgent &&
        !long_press &&
        (event.type == wqn::ButtonEventType::kShortPress) &&
        (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower)) {
        if (state->ai.status == wqn::AiSessionStatus::kPreparingCapture ||
            state->ai.status == wqn::AiSessionStatus::kListening) {
            return RefreshSchedule::kNone;
        }
        constexpr int32_t kScrollStepRows = 4;  // [scroll-2x] was 2; doubled per request
        const wqn::AiHistoryChannel channel = state->ai.tier == wqn::AiTier::kFlash
            ? wqn::AiHistoryChannel::kFlash
            : wqn::AiHistoryChannel::kStdPro;
        auto snapshot = wqn::GetAiHistorySnapshot(channel);

        // [follow] Whether the viewport is currently following this turn. Read
        // before the scroll: a viewport the follow is holding at the tail must
        // not flash the "已最新" hint -- the hint means "you cannot go further
        // down", while a followed viewport is at the newest content on purpose.
        const bool following = state->ai.auto_follow && state->ai.follow_active;

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
                // [follow] A manual scroll takes the viewport back: retire the
                // follow and let the conditional recenter keep off it.
                wqn::SetAiFollowState(false, /*user_moved=*/true);
                state->ai.follow_active = false;
                state->ai.user_moved = true;
            } else {
                // Down = "newer". Scroll band shifts content up.
                if (current <= min_scroll && !following) {
                    wqn::StampScrollNoOpHint();
                }
                int32_t landed = current - kScrollStepRows;
                if (landed < min_scroll) {
                    landed = min_scroll;
                }
                if (landed > max_scroll) {
                    landed = max_scroll;
                }
                wqn::SetAiScrollOffsetLinesClamped(landed, min_scroll, max_scroll);
                // [follow] A Down that lands on the tail before the answer body
                // has started = "watch the run again": re-arm the follow
                // (user_moved stays true -- they did move the viewport). Any
                // other Down is a manual move and retires it. Flash is excluded:
                // it has no arm path and hides the toggle, so its viewport keeps
                // its old "stays where the user put it" behavior.
                const bool resumed = state->ai.auto_follow &&
                    state->ai.tier != wqn::AiTier::kFlash && landed == min_scroll &&
                    !AnswerBodyStarted(state->ai, snapshot);
                wqn::SetAiFollowState(resumed, /*user_moved=*/true);
                state->ai.follow_active = resumed;
                state->ai.user_moved = true;
            }
        } else if (event.button != wqn::ButtonId::kUp && !following) {
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
    // [scroll-anytime] The old "Block Up/Down while busy on AI page" fallback
    // that stood here was removed with the gate narrowing: both tier branches
    // above now match every short-press Up/Down on the AI page and swallow
    // their own capture phases, so it could never be reached.
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
        // [agent] The OpenCode session list is no longer loaded on screen
        // entry: the picker opens from the AI page's status-bar edit mode and
        // that path issues the request itself.
        if (state->screen == wqn::UiScreen::kTodo) {
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
            state->word_app.mode = wqn::WordAppMode::kHome;
            state->word_app.message = IsWordCloudBusy()
                ? "单词服务忙，请重试"
                : "本轮准备失败，请重试";
        }
        // [persist-worker] The word observation commit (durable outbox append +
        // session-cursor snapshot) no longer runs synchronously here -- it used
        // to block the UI task on foreground storage. PumpWordObservationCommit
        // now hands it to the persist worker; the card stays in kPersisting
        // ("正在保存") until the worker's result is applied (advance card / retry)
        // on the UI task via DispatchWordObservationPersistResult.
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
