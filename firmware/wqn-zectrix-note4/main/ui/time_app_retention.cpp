// RTC slow-memory persistence for the active countdown / pomodoro state.

#include "ui_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>

#include "esp_attr.h"

namespace device_ui_internal {
namespace {

constexpr uint32_t kRetainedTimeAppMagic = 0x57514E54;  // WQNT
constexpr uint32_t kRetainedTimeAppVersion = 3;

struct RetainedTimeAppState {
    uint32_t magic;
    uint32_t version;
    int32_t tile;
    int32_t active_mode;
    int32_t status;
    int32_t pomodoro_phase;
    int32_t countdown_hours;
    int32_t countdown_minutes;
    int32_t countdown_seconds;
    int32_t countdown_total_seconds;
    int32_t pomodoro_rounds;
    int32_t pomodoro_focus_minutes;
    int32_t pomodoro_break_minutes;
    int32_t pomodoro_long_break_minutes;
    int32_t pomodoro_current_round;
    int32_t remaining_seconds;
    int64_t session_started_unix_seconds;
    int64_t phase_started_unix_seconds;
    int64_t phase_ends_unix_seconds;
    int64_t paused_at_unix_seconds;
    char task_name[32];
    uint32_t checksum;
};

RTC_DATA_ATTR RetainedTimeAppState g_rtc_time_app = {};

uint32_t RetainedTimeAppChecksum(const RetainedTimeAppState& retained)
{
    constexpr uint32_t kFnvOffset = 2166136261U;
    constexpr uint32_t kFnvPrime = 16777619U;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&retained);
    uint32_t hash = kFnvOffset;
    for (size_t index = 0; index < offsetof(RetainedTimeAppState, checksum); ++index) {
        hash = (hash ^ bytes[index]) * kFnvPrime;
    }
    return hash;
}

bool RetainedTimeAppLooksValid(const RetainedTimeAppState& retained)
{
    const bool enum_values_valid =
        retained.tile >= static_cast<int32_t>(wqn::TimeTile::kClock) &&
        retained.tile <= static_cast<int32_t>(wqn::TimeTile::kPomodoro) &&
        retained.active_mode >= static_cast<int32_t>(wqn::TimerMode::kCountdown) &&
        retained.active_mode <= static_cast<int32_t>(wqn::TimerMode::kPomodoro) &&
        retained.status >= static_cast<int32_t>(wqn::TimerStatus::kRunning) &&
        retained.status <= static_cast<int32_t>(wqn::TimerStatus::kAlerting) &&
        retained.pomodoro_phase >= static_cast<int32_t>(wqn::PomodoroPhase::kFocus) &&
        retained.pomodoro_phase <= static_cast<int32_t>(wqn::PomodoroPhase::kLongBreak);
    const bool ranges_valid =
        retained.countdown_total_seconds >= 1 && retained.countdown_total_seconds <= 7200 &&
        retained.pomodoro_rounds >= 1 && retained.pomodoro_rounds <= 99 &&
        retained.pomodoro_focus_minutes >= 1 && retained.pomodoro_focus_minutes <= 99 &&
        retained.pomodoro_break_minutes >= 1 && retained.pomodoro_break_minutes <= 99 &&
        retained.pomodoro_long_break_minutes >= 1 && retained.pomodoro_long_break_minutes <= 99 &&
        retained.pomodoro_current_round >= 1 &&
        retained.pomodoro_current_round <= retained.pomodoro_rounds &&
        retained.remaining_seconds >= 0 && retained.remaining_seconds <= 7200;
    return retained.magic == kRetainedTimeAppMagic &&
           retained.version == kRetainedTimeAppVersion && enum_values_valid && ranges_valid &&
           retained.checksum == RetainedTimeAppChecksum(retained);
}

}  // namespace

void RetainTimeAppState(const wqn::TimeAppState& state)
{
    if (!wqn::TimeAppHasActiveTimer(state)) {
        g_rtc_time_app = {};
        return;
    }
    RetainedTimeAppState retained = {};
    retained.magic = kRetainedTimeAppMagic;
    retained.version = kRetainedTimeAppVersion;
    retained.tile = static_cast<int32_t>(state.tile);
    retained.active_mode = static_cast<int32_t>(state.active_mode);
    retained.status = static_cast<int32_t>(state.status);
    retained.pomodoro_phase = static_cast<int32_t>(state.pomodoro_phase);
    retained.countdown_hours = state.countdown_hours;
    retained.countdown_minutes = state.countdown_minutes;
    retained.countdown_seconds = state.countdown_seconds;
    retained.countdown_total_seconds = state.countdown_total_seconds;
    retained.pomodoro_rounds = state.pomodoro_rounds;
    retained.pomodoro_focus_minutes = state.pomodoro_focus_minutes;
    retained.pomodoro_break_minutes = state.pomodoro_break_minutes;
    retained.pomodoro_long_break_minutes = state.pomodoro_long_break_minutes;
    retained.pomodoro_current_round = state.pomodoro_current_round;
    retained.remaining_seconds = state.remaining_seconds;
    retained.session_started_unix_seconds = state.session_started_unix_seconds;
    retained.phase_started_unix_seconds = state.phase_started_unix_seconds;
    retained.phase_ends_unix_seconds = state.phase_ends_unix_seconds;
    retained.paused_at_unix_seconds = state.paused_at_unix_seconds;
    std::snprintf(retained.task_name, sizeof(retained.task_name), "%s", state.task_name);
    retained.checksum = RetainedTimeAppChecksum(retained);
    g_rtc_time_app = retained;
}

bool RestoreRetainedTimeApp(wqn::TimeAppState* state)
{
    if (state == nullptr || !RetainedTimeAppLooksValid(g_rtc_time_app)) {
        return false;
    }
    const RetainedTimeAppState retained = g_rtc_time_app;
    wqn::TimeAppState restored;
    restored.tile = static_cast<wqn::TimeTile>(retained.tile);
    restored.active_mode = static_cast<wqn::TimerMode>(retained.active_mode);
    restored.status = static_cast<wqn::TimerStatus>(retained.status);
    restored.pomodoro_phase = static_cast<wqn::PomodoroPhase>(retained.pomodoro_phase);
    restored.countdown_hours = retained.countdown_hours;
    restored.countdown_minutes = retained.countdown_minutes;
    restored.countdown_seconds = retained.countdown_seconds;
    restored.countdown_total_seconds = retained.countdown_total_seconds;
    restored.pomodoro_rounds = retained.pomodoro_rounds;
    restored.pomodoro_focus_minutes = retained.pomodoro_focus_minutes;
    restored.pomodoro_break_minutes = retained.pomodoro_break_minutes;
    restored.pomodoro_long_break_minutes = retained.pomodoro_long_break_minutes;
    restored.pomodoro_current_round = retained.pomodoro_current_round;
    restored.remaining_seconds = retained.remaining_seconds;
    restored.session_started_unix_seconds = retained.session_started_unix_seconds;
    restored.phase_started_unix_seconds = retained.phase_started_unix_seconds;
    restored.phase_ends_unix_seconds = retained.phase_ends_unix_seconds;
    restored.paused_at_unix_seconds = retained.paused_at_unix_seconds;
    std::snprintf(restored.task_name, sizeof(restored.task_name), "%s", retained.task_name);
    restored.last_tick_ms = 0;
    wqn::DisarmTimeAppAction(&restored);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (restored.status == wqn::TimerStatus::kRunning &&
        now >= kMinReasonableUnixTime && restored.phase_ends_unix_seconds > 0) {
        const int64_t remaining = restored.phase_ends_unix_seconds - now;
        if (remaining <= 0) {
            restored.remaining_seconds = 0;
            restored.status = wqn::TimerStatus::kAlerting;
            restored.tile = restored.active_mode == wqn::TimerMode::kPomodoro
                ? wqn::TimeTile::kPomodoro
                : wqn::TimeTile::kCountdown;
        } else {
            restored.remaining_seconds =
                static_cast<int>(std::min<int64_t>(remaining, 7200));
        }
    }
    *state = restored;
    RetainTimeAppState(*state);
    return true;
}

bool HasRetainedTimeApp()
{
    return RetainedTimeAppLooksValid(g_rtc_time_app);
}

}  // namespace device_ui_internal
