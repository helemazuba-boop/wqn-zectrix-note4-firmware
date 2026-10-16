// UI state management: load current study state, build the Home summary, and
// detect time-app structural changes.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "services/sync_service.h"
#include "services/connectivity_service.h"
#include "storage.h"
#include "ai_session.h"
#include "note_app.h"
#include "opencode_session.h"
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
           a.pomodoro_current_round == b.pomodoro_current_round &&
           a.session_started_unix_seconds == b.session_started_unix_seconds &&
           a.phase_started_unix_seconds == b.phase_started_unix_seconds &&
           a.phase_ends_unix_seconds == b.phase_ends_unix_seconds &&
           a.paused_at_unix_seconds == b.paused_at_unix_seconds &&
           a.action_armed == b.action_armed &&
           a.action_armed_at_unix_seconds == b.action_armed_at_unix_seconds &&
           std::strcmp(a.task_name, b.task_name) == 0;
}

bool TimeAppStructureChanged(const wqn::TimeAppState& before, const wqn::TimeAppState& after)
{
    return before.tile != after.tile || before.active_mode != after.active_mode || before.status != after.status ||
           before.config_mode != after.config_mode || before.pomodoro_phase != after.pomodoro_phase;
}

void UpdateHomeAiSummary(wqn::UiState* state, int64_t now_ms)
{
    if (state == nullptr) return;
    const auto& agent = state->agent;
    const auto& cached = agent.home;
    const bool current_differs = !agent.current_session_id.empty() &&
        agent.current_session_id != cached.session.id;
    std::string id = current_differs ? agent.current_session_id : cached.session.id;
    std::string title = current_differs ? agent.current_session_title : cached.session.title;
    wqn::HomeAiStatus status = wqn::HomeAiStatus::kIdle;
    if (!id.empty() || cached.failed) status = wqn::HomeAiStatus::kUnknown;
    const bool fresh = cached.loaded && !cached.failed && !current_differs && state->status.wifi_connected &&
        now_ms >= cached.checked_at_ms && now_ms - cached.checked_at_ms <= wqn::kHomeAgentFreshMs;
    if (fresh) {
        switch (cached.session.outcome) {
            case wqn::OpenCodeSessionOutcome::kRunning: status = wqn::HomeAiStatus::kRunning; break;
            case wqn::OpenCodeSessionOutcome::kSucceeded: status = wqn::HomeAiStatus::kComplete; break;
            case wqn::OpenCodeSessionOutcome::kFailed: status = wqn::HomeAiStatus::kFailed; break;
            case wqn::OpenCodeSessionOutcome::kInterrupted: status = wqn::HomeAiStatus::kInterrupted; break;
            case wqn::OpenCodeSessionOutcome::kUnknown: status = wqn::HomeAiStatus::kIdle; break;
        }
    }
    // Only an attached interaction can authoritatively claim a pending ask.
    if (agent.stream_active && !agent.current_session_id.empty()) {
        id = agent.current_session_id; title = agent.current_session_title;
        if (!agent.pending_permission_id.empty()) status = wqn::HomeAiStatus::kPermission;
        else if (!agent.pending_question_id.empty()) status = wqn::HomeAiStatus::kQuestion;
        else if (agent.run_live) status = wqn::HomeAiStatus::kRunning;
    }
    state->home.ai = wqn::MakeHomeAiCard(
        status, id, title, cached.session.updated_at, state->status.paired,
#if CONFIG_WQN_AI_ENABLE
        true,
#else
        false,
#endif
        cached.session.updated_at > 0 && id == state->home_viewed_session_id &&
        cached.session.updated_at == state->home_viewed_result_version);
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
        home.full = battery.full && battery.external_power_present;
    } else {
        home.battery_label = "--%";
    }

    // UI contract: one line only. Source priority is pomodoro > countdown > clock.
    home.primary_time_line = ChooseHomePrimaryTimeLine(state->time_app);
    home.timer_status_only = wqn::TimeAppHasActiveTimer(state->time_app);

    const uint8_t mastered = static_cast<uint8_t>(
        wqn::protocol::problem_study_v1::ProblemStatus::kMastered);
    const auto review_count = std::count_if(
        state->problem_app.pack_index.entries.begin(), state->problem_app.pack_index.entries.end(),
        [mastered](const wqn::ProblemPackIndexEntry& entry) { return entry.status != mastered; });
    home.review_label = "待复习 " + std::to_string(review_count);
    home.todo_label = state->todo.loaded_once
        ? "待办 " + std::to_string(std::max(0, state->todo.total_pending)) : "待办 --";
    if (!state->status.paired && !state->status.claim_code.empty()) {
        home.notice = "配对码 " + state->status.claim_code;
    }
    if (state->problem_app.active && state->problem_app.current_loaded) {
        home.study.title = state->problem_app.set_name.empty() ? "继续错题复习" : state->problem_app.set_name;
        home.study.resumable = true;
    } else if (state->note_app.current_note_loaded &&
               (state->note_app.mode == wqn::NoteAppMode::kNoteView ||
                state->note_app.mode == wqn::NoteAppMode::kNoteImageView)) {
        home.study.title = state->note_app.current_note.title;
        home.study.resumable = true;
    } else if (state->word_app.mode == wqn::WordAppMode::kWordCard &&
               state->word_app.scoped_deck_id.empty() &&
               !state->word_app.scope_reset_pending_valid) {
        home.study.target = wqn::UiScreen::kWord;
        home.study.title = state->word_app.default_deck_title.empty() ? "单词复习" : state->word_app.default_deck_title;
        home.study.progress = wqn::WordAppProgressLabel(state->word_app);
        home.study.resumable = true;
        size_t position = 0, total = 0;
        const char* progress = home.study.progress.c_str();
        if (*progress == '#') ++progress;
        if (std::sscanf(progress, "%zu/%zu", &position, &total) == 2 && total != 0) {
            home.study.progress_percent = static_cast<int>(
                std::min<uint64_t>(100, static_cast<uint64_t>(position) * 100 / total));
        }
    }

    state->home = std::move(home);
    UpdateHomeAiSummary(state, esp_timer_get_time() / 1000);
    wqn::ClampUiSelection(state);
}

bool LoadUiState(wqn::UiState* state, bool restore_screen_from_rtc)
{
    if (state == nullptr) {
        return false;
    }

    if (restore_screen_from_rtc) {
        // Restore the last screen only during startup. A periodic status reload
        // starts from the live AppState and must never navigate the user away.
        // On cold boot or RTC corruption, g_rtc_screen_val may be 0 (= kAi),
        // which is unsafe, or out of range; fall back to kHome in either case.
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
                case wqn::UiScreen::kOpenCode:
                case wqn::UiScreen::kProvisioning:
                    return false;
            }
            return false;
        };
        const int saved_screen = g_rtc_screen_val;
        state->screen = is_restorable_screen(saved_screen)
            ? static_cast<wqn::UiScreen>(saved_screen)
            : wqn::UiScreen::kHome;
        RestoreRetainedTimeApp(&state->time_app);
    }

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
    if (restore_screen_from_rtc) {
        // [ai-follow][detail] Boot seeding for the two settings that live in a
        // worker's own state struct (AiSessionState / OpenCodeSessionState)
        // rather than in AppState: the UI only ever sees a snapshot of them, and
        // only their setters can change them, so the durable values have to be
        // pushed once, here. A periodic reload deliberately does NOT do this --
        // it can run while a save is armed or in flight and would overwrite the
        // user's choice with the value still on flash.
        wqn::SetAiAutoFollow(state->settings.auto_follow);
        state->settings.agent_detail_desired = state->settings.agent_detail_persisted;
        state->agent.detail_level = state->settings.agent_detail_desired;
        wqn::SetOpenCodeDetailLevel(state->settings.agent_detail_desired);
    }
    wqn::SetNoteImageRenderMode(
        &state->note_app, state->settings.image_render_mode);
    wqn::SetProblemImageRenderMode(
        &state->problem_app, state->settings.image_render_mode);
    wqn::ClampUiSelection(state);
    BuildHomeSummary(state);
    return true;
}

}  // namespace device_ui_internal
