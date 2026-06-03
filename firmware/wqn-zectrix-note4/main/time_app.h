#pragma once

#include <cstdint>
#include <string>

namespace wqn {

enum class TimeTile {
    kClock,
    kCountdown,
    kPomodoro,
};

enum class TimerMode {
    kNone,
    kCountdown,
    kPomodoro,
};

enum class TimerStatus {
    kIdle,
    kRunning,
    kPaused,
    kAlerting,
};

enum class PomodoroPhase {
    kFocus,
    kBreak,
    kLongBreak,
};

enum class TimeInput {
    kUp,
    kDown,
    kConfirm,
    kLongUp,
    kLongDown,
    kLongConfirm,
};

struct TimeAppState {
    TimeTile tile = TimeTile::kClock;
    TimerMode active_mode = TimerMode::kNone;
    TimerStatus status = TimerStatus::kIdle;
    PomodoroPhase pomodoro_phase = PomodoroPhase::kFocus;

    bool config_mode = false;
    bool is_editing = false;
    int active_field = 0;

    int countdown_hours = 0;
    int countdown_minutes = 5;
    int countdown_seconds = 0;
    int countdown_total_seconds = 300;

    int pomodoro_rounds = 4;
    int pomodoro_focus_minutes = 25;
    int pomodoro_break_minutes = 5;
    int pomodoro_long_break_minutes = 15;
    int pomodoro_current_round = 1;

    int remaining_seconds = 0;
    int64_t last_tick_ms = 0;
};

void ResetTimeApp(TimeAppState* state);
bool HandleTimeAppInput(TimeAppState* state, TimeInput input);
bool TickTimeApp(TimeAppState* state, int64_t now_ms);
bool TimeAppHasActiveTimer(const TimeAppState& state);
bool TimeAppIsEditingValue(const TimeAppState& state);
std::string TimeAppPrimaryLine(const TimeAppState& state);
std::string FormatTimerDuration(int total_seconds);
const char* PomodoroPhaseLabel(PomodoroPhase phase);

}  // namespace wqn
