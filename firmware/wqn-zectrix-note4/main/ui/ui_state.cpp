// UI state management: load current study state, build the Home summary, and
// detect time-app structural changes.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include "esp_log.h"
#include "services/sync_service.h"
#include "services/connectivity_service.h"
#include "storage.h"
#include "note_app.h"
#include "word_app.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

bool SameTimeAppState(const wqn::TimeAppState& a, const wqn::TimeAppState& b)
{
    return a.tile == b.tile && a.active_mode == b.active_mode && a.status == b.status &&
           a.pomodoro_phase == b.pomodoro_phase && a.config_mode == b.config_mode &&
           a.is_editing == b.is_editing && a.active_field == b.active_field &&
           a.remaining_seconds == b.remaining_seconds && a.countdown_hours == b.countdown_hours &&
           a.countdown_minutes == b.countdown_minutes && a.countdown_seconds == b.countdown_seconds &&
           a.countdown_total_seconds == b.countdown_total_seconds && a.pomodoro_rounds == b.pomodoro_rounds &&
           a.pomodoro_focus_minutes == b.pomodoro_focus_minutes &&
           a.pomodoro_break_minutes == b.pomodoro_break_minutes &&
           a.pomodoro_long_break_minutes == b.pomodoro_long_break_minutes &&
           a.pomodoro_current_round == b.pomodoro_current_round;
}

bool TimeAppStructureChanged(const wqn::TimeAppState& before, const wqn::TimeAppState& after)
{
    return before.tile != after.tile || before.active_mode != after.active_mode || before.status != after.status ||
           before.config_mode != after.config_mode || before.pomodoro_phase != after.pomodoro_phase;
}

void BuildHomeSummary(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }

    wqn::HomeSummary home;
    home.wifi_label = state->status.wifi_connected ? "WiFi" : "离线";
    home.wifi_connected = state->status.wifi_connected;
    home.wifi_rssi = wqn::services::GetConnectivityRssi();
    BatteryReading battery = {};
    if (ReadBatteryStatus(&battery)) {
        home.battery_label = BatteryLabel(battery);
        home.battery_percent = battery.percent;
        home.charging = battery.charging;
        home.full = battery.full;
    } else {
        home.battery_label = "--%";
    }

    // UI contract: one line only. Source priority is pomodoro > countdown > clock.
    home.primary_time_line = ChooseHomePrimaryTimeLine(state->time_app);

    const size_t problem_count = state->problem_app.pack_index.entries.size();
    const uint8_t mastered = static_cast<uint8_t>(
        wqn::protocol::problem_study_v1::ProblemStatus::kMastered);
    const size_t review_count = static_cast<size_t>(std::count_if(
        state->problem_app.pack_index.entries.begin(),
        state->problem_app.pack_index.entries.end(),
        [mastered](const wqn::ProblemPackIndexEntry& entry) {
            return entry.status != mastered;
        }));
    const size_t problem_set_count = state->problem_app.pack_index.sets.size();
    home.review_metric.value = std::to_string(review_count);
    home.review_metric.label = "今日复习";
    home.todo_metric.value = std::to_string(std::max(0, state->todo.total_pending));
    home.todo_metric.label = "今日 Todo";
    home.word_metric.value = wqn::WordAppProgressLabel(state->word_app);
    home.word_metric.label = "单词进度";
    if (!state->status.paired && !state->status.claim_code.empty()) {
        home.current_status = "配对码 " + state->status.claim_code + " · 请在网页确认";
    } else {
        home.current_status =
            "本地 " + std::to_string(problem_count) + " 题 · 待上传 " +
            std::to_string(state->problem_app.outbox.pending_count);
    }

    home.tasks.clear();
    if (problem_count > 0) {
        home.tasks.push_back(wqn::HomeTask{
            "错题复习",
            std::to_string(problem_set_count) + " 个错题集 · " +
                std::to_string(review_count) + " 题待复习",
            "错题"});
    } else {
        home.tasks.push_back(wqn::HomeTask{
            "同步错题后开始复习", "当前没有本地错题集", "错题"});
    }
    home.tasks.push_back(wqn::HomeTask{"单词复习", wqn::WordAppStatusLine(state->word_app), "单词"});

    state->home = std::move(home);
    wqn::ClampUiSelection(state);
}

bool LoadUiState(wqn::UiState* state)
{
    if (state == nullptr) {
        return false;
    }

    // Restore last screen from RTC slow memory (survives deep sleep). On cold boot
    // or RTC corruption, g_rtc_screen_val may be 0 (= kAi) which is unsafe, or
    // contain an out-of-range value; fall back to kHome in either case.
    auto is_restorable_screen = [](int value) {
        switch (static_cast<wqn::UiScreen>(value)) {
            case wqn::UiScreen::kTodo:
            case wqn::UiScreen::kSettings:
            case wqn::UiScreen::kHome:
            case wqn::UiScreen::kTime:
            case wqn::UiScreen::kWord:
            case wqn::UiScreen::kNote:
                return true;
            case wqn::UiScreen::kAi:
            case wqn::UiScreen::kProvisioning:
                return false;
        }
        return false;
    };
    const int saved_screen = g_rtc_screen_val;
    state->screen = is_restorable_screen(saved_screen)
        ? static_cast<wqn::UiScreen>(saved_screen)
        : wqn::UiScreen::kHome;

    esp_err_t result = wqn::InitWordApp(&state->word_app);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "init word app failed: %s", esp_err_to_name(result));
    }

    // Initialize Note eagerly at boot (mirrors Word above). Otherwise the first
    // note interaction pays the synchronous pack-index build (SHA verify + JSONL
    // scan + sort) + outbox snapshot + session load on the UI thread, stalling
    // the first Up/Down/Confirm after entering the note page for seconds.
    result = wqn::InitNoteApp(&state->note_app);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "init note app failed: %s", esp_err_to_name(result));
    }

    // The problem layer rides on the note screen; init it eagerly for the
    // same first-interaction-stall reason (index build + outbox snapshot).
    result = wqn::InitProblemApp(&state->problem_app);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "init problem app failed: %s", esp_err_to_name(result));
    }
    if (!state->problem_app.pack_index.sets.empty()) {
        std::vector<wqn::NoteProblemSetRow> rows;
        rows.reserve(state->problem_app.pack_index.sets.size());
        for (const wqn::ProblemPackSet& set : state->problem_app.pack_index.sets) {
            wqn::NoteProblemSetRow row;
            row.set_id = set.set_id;
            row.name = set.name;
            row.entry_count = set.entry_count;
            rows.push_back(std::move(row));
        }
        wqn::ApplyNoteProblemSetRows(&state->note_app, std::move(rows));
    }

    // Mixed [词] rows + the settings row value both mirror the word deck
    // catalog loaded by InitWordApp above.
    RebuildNoteWordDeckRows(state);
    state->settings.default_word_deck_title = state->word_app.default_deck_title;

    std::string token;
    result = wqn::LoadAccessToken(&token);
    if (result == ESP_OK && !token.empty() && wqn::IsValidAccessToken(token)) {
        state->status.paired = true;
        state->status.token_mask = wqn::MaskTokenForLog(token);
        state->status.claim_code.clear();
    } else {
        state->status.paired = false;
        state->status.token_mask.clear();
        wqn::services::SyncSnapshot sync_snapshot;
        wqn::services::GetSyncSnapshot(&sync_snapshot);
        state->status.claim_code = sync_snapshot.claim_code;
    }

#if CONFIG_WQN_WIFI_STA_ENABLE
    state->status.wifi_enabled = true;
    state->status.wifi_connected = wqn::services::IsConnectivityOnline();
#else
    state->status.wifi_enabled = false;
    state->status.wifi_connected = false;
#endif

    UpdateSettingsDiagnostics(state);
    wqn::SetNoteImageRenderMode(
        &state->note_app, state->settings.image_render_mode);
    wqn::SetProblemImageRenderMode(
        &state->problem_app, state->settings.image_render_mode);
    wqn::ClampUiSelection(state);
    BuildHomeSummary(state);
    return true;
}

}  // namespace device_ui_internal
