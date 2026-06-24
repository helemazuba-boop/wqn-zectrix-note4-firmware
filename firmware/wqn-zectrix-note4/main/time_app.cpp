#include "time_app.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr int kCountdownMaxSeconds = 7200;
constexpr int kPomodoroMinValue = 1;
constexpr int kPomodoroMaxValue = 99;

int ClampInt(int value, int min_value, int max_value)
{
    return std::min(max_value, std::max(min_value, value));
}

int CountdownSecondsFromFields(const wqn::TimeAppState& state)
{
    return state.countdown_hours * 3600 + state.countdown_minutes * 60 + state.countdown_seconds;
}

void NormalizeCountdownFields(wqn::TimeAppState* state)
{
    if (state == nullptr) {
        return;
    }
    state->countdown_total_seconds = ClampInt(CountdownSecondsFromFields(*state), 1, kCountdownMaxSeconds);
    int seconds = state->countdown_total_seconds;
    state->countdown_hours = seconds / 3600;
    seconds %= 3600;
    state->countdown_minutes = seconds / 60;
    state->countdown_seconds = seconds % 60;
}

int NumericFieldCount(wqn::TimeTile tile)
{
    switch (tile) {
        case wqn::TimeTile::kCountdown:
            return 3;
        case wqn::TimeTile::kPomodoro:
            return 4;
        case wqn::TimeTile::kClock:
        default:
            return 0;
    }
}

int ActionFieldCount()
{
    return 2;
}

int MaxFieldIndex(wqn::TimeTile tile)
{
    const int numeric = NumericFieldCount(tile);
    return numeric > 0 ? numeric + ActionFieldCount() - 1 : 0;
}

int StartFieldIndex(wqn::TimeTile tile)
{
    return NumericFieldCount(tile);
}

int ExitFieldIndex(wqn::TimeTile tile)
{
    return NumericFieldCount(tile) + 1;
}

void EnterConfig(wqn::TimeAppState* state)
{
    state->config_mode = true;
    state->is_editing = false;
    state->active_field = 0;
}

void ExitToClock(wqn::TimeAppState* state)
{
    state->tile = wqn::TimeTile::kClock;
    state->config_mode = false;
    state->is_editing = false;
    state->active_field = 0;
    if (state->status != wqn::TimerStatus::kRunning && state->status != wqn::TimerStatus::kPaused) {
        state->status = wqn::TimerStatus::kIdle;
        state->active_mode = wqn::TimerMode::kNone;
        state->remaining_seconds = 0;
    }
}

void StartCountdown(wqn::TimeAppState* state)
{
    NormalizeCountdownFields(state);
    state->active_mode = wqn::TimerMode::kCountdown;
    state->status = wqn::TimerStatus::kRunning;
    state->remaining_seconds = state->countdown_total_seconds;
    state->config_mode = false;
    state->is_editing = false;
    state->active_field = 0;
    state->last_tick_ms = 0;
}

void StartPomodoro(wqn::TimeAppState* state)
{
    state->pomodoro_rounds = ClampInt(state->pomodoro_rounds, kPomodoroMinValue, kPomodoroMaxValue);
    state->pomodoro_focus_minutes = ClampInt(state->pomodoro_focus_minutes, kPomodoroMinValue, kPomodoroMaxValue);
    state->pomodoro_break_minutes = ClampInt(state->pomodoro_break_minutes, kPomodoroMinValue, kPomodoroMaxValue);
    state->pomodoro_long_break_minutes =
        ClampInt(state->pomodoro_long_break_minutes, kPomodoroMinValue, kPomodoroMaxValue);
    state->active_mode = wqn::TimerMode::kPomodoro;
    state->status = wqn::TimerStatus::kRunning;
    state->pomodoro_phase = wqn::PomodoroPhase::kFocus;
    state->pomodoro_current_round = 1;
    state->remaining_seconds = state->pomodoro_focus_minutes * 60;
    state->config_mode = false;
    state->is_editing = false;
    state->active_field = 0;
    state->last_tick_ms = 0;
}

void ToggleRunPause(wqn::TimeAppState* state)
{
    if (state->status == wqn::TimerStatus::kRunning) {
        state->status = wqn::TimerStatus::kPaused;
    } else if (state->status == wqn::TimerStatus::kPaused) {
        state->status = wqn::TimerStatus::kRunning;
        state->last_tick_ms = 0;
    }
}

void StopTimer(wqn::TimeAppState* state)
{
    state->status = wqn::TimerStatus::kIdle;
    state->active_mode = wqn::TimerMode::kNone;
    state->remaining_seconds = 0;
    state->last_tick_ms = 0;
    ExitToClock(state);
}

void AdjustConfigValue(wqn::TimeAppState* state, int delta)
{
    if (state == nullptr || !state->config_mode || !state->is_editing) {
        return;
    }
    if (state->tile == wqn::TimeTile::kCountdown) {
        switch (state->active_field) {
            case 0:
                state->countdown_hours = ClampInt(state->countdown_hours + delta, 0, 2);
                break;
            case 1:
                state->countdown_minutes = ClampInt(state->countdown_minutes + delta, 0, 59);
                break;
            case 2:
                state->countdown_seconds = ClampInt(state->countdown_seconds + delta, 0, 59);
                break;
            default:
                break;
        }
        NormalizeCountdownFields(state);
        return;
    }

    if (state->tile == wqn::TimeTile::kPomodoro) {
        switch (state->active_field) {
            case 0:
                state->pomodoro_rounds = ClampInt(state->pomodoro_rounds + delta, kPomodoroMinValue, kPomodoroMaxValue);
                break;
            case 1:
                state->pomodoro_focus_minutes =
                    ClampInt(state->pomodoro_focus_minutes + delta, kPomodoroMinValue, kPomodoroMaxValue);
                break;
            case 2:
                state->pomodoro_break_minutes =
                    ClampInt(state->pomodoro_break_minutes + delta, kPomodoroMinValue, kPomodoroMaxValue);
                break;
            case 3:
                state->pomodoro_long_break_minutes =
                    ClampInt(state->pomodoro_long_break_minutes + delta, kPomodoroMinValue, kPomodoroMaxValue);
                break;
            default:
                break;
        }
    }
}

void MoveActiveField(wqn::TimeAppState* state, int delta)
{
    if (state == nullptr || !state->config_mode || state->is_editing) {
        return;
    }
    const int max_index = MaxFieldIndex(state->tile);
    if (max_index <= 0) {
        state->active_field = 0;
        return;
    }
    state->active_field += delta;
    if (state->active_field < 0) {
        state->active_field = max_index;
    } else if (state->active_field > max_index) {
        state->active_field = 0;
    }
}

void AdvancePomodoroPhase(wqn::TimeAppState* state)
{
    if (state == nullptr) {
        return;
    }
    if (state->pomodoro_phase == wqn::PomodoroPhase::kFocus) {
        const bool last_round = state->pomodoro_current_round >= state->pomodoro_rounds;
        state->pomodoro_phase = last_round ? wqn::PomodoroPhase::kLongBreak : wqn::PomodoroPhase::kBreak;
        state->remaining_seconds =
            (last_round ? state->pomodoro_long_break_minutes : state->pomodoro_break_minutes) * 60;
        state->last_tick_ms = 0;
        return;
    }

    if (state->pomodoro_current_round >= state->pomodoro_rounds) {
        StopTimer(state);
        return;
    }

    ++state->pomodoro_current_round;
    state->pomodoro_phase = wqn::PomodoroPhase::kFocus;
    state->remaining_seconds = state->pomodoro_focus_minutes * 60;
    state->last_tick_ms = 0;
}

}  // namespace

namespace wqn {

void ResetTimeApp(TimeAppState* state)
{
    if (state == nullptr) {
        return;
    }
    *state = TimeAppState{};
    NormalizeCountdownFields(state);
}

bool HandleTimeAppInput(TimeAppState* state, TimeInput input)
{
    if (state == nullptr) {
        return false;
    }

    const TimeAppState before = *state;

    if (state->status == TimerStatus::kAlerting) {
        if (state->active_mode == TimerMode::kPomodoro) {
            state->status = TimerStatus::kRunning;
            AdvancePomodoroPhase(state);
        } else {
            StopTimer(state);
        }
        return true;
    }

    if (input == TimeInput::kLongConfirm) {
        if (state->status == TimerStatus::kRunning || state->status == TimerStatus::kPaused ||
            state->status == TimerStatus::kAlerting) {
            StopTimer(state);
            return true;
        }
        ExitToClock(state);
        return true;
    }

    if (input == TimeInput::kLongDown &&
        (state->status == TimerStatus::kRunning || state->status == TimerStatus::kPaused)) {
        StopTimer(state);
        return true;
    }

    if (state->tile == TimeTile::kClock) {
        if (input == TimeInput::kUp) {
            state->tile = TimeTile::kCountdown;
            if (state->status == TimerStatus::kIdle) {
                EnterConfig(state);
            }
        } else if (input == TimeInput::kDown) {
            state->tile = TimeTile::kPomodoro;
            if (state->status == TimerStatus::kIdle) {
                EnterConfig(state);
            }
        }
        return before.tile != state->tile || before.config_mode != state->config_mode;
    }

    if (state->config_mode) {
        if (input == TimeInput::kUp || input == TimeInput::kDown || input == TimeInput::kLongUp ||
            input == TimeInput::kLongDown) {
            const int direction = (input == TimeInput::kUp || input == TimeInput::kLongUp) ? 1 : -1;
            const int step = (input == TimeInput::kLongUp || input == TimeInput::kLongDown) ? 5 : 1;
            if (state->is_editing) {
                AdjustConfigValue(state, direction * step);
            } else {
                MoveActiveField(state, direction);
            }
        } else if (input == TimeInput::kConfirm) {
            if (state->is_editing) {
                state->is_editing = false;
            } else if (state->active_field < NumericFieldCount(state->tile)) {
                state->is_editing = true;
            } else if (state->active_field == StartFieldIndex(state->tile)) {
                if (state->tile == TimeTile::kCountdown) {
                    StartCountdown(state);
                } else if (state->tile == TimeTile::kPomodoro) {
                    StartPomodoro(state);
                }
            } else if (state->active_field == ExitFieldIndex(state->tile)) {
                ExitToClock(state);
            }
        }
        return true;
    }

    if (input == TimeInput::kConfirm) {
        if (state->status == TimerStatus::kIdle) {
            EnterConfig(state);
        } else {
            ToggleRunPause(state);
        }
    } else if (state->tile == TimeTile::kCountdown && input == TimeInput::kDown) {
        ExitToClock(state);
    } else if (state->tile == TimeTile::kPomodoro && input == TimeInput::kUp) {
        ExitToClock(state);
    }

    return before.tile != state->tile || before.status != state->status || before.config_mode != state->config_mode ||
           before.is_editing != state->is_editing || before.active_field != state->active_field ||
           before.remaining_seconds != state->remaining_seconds;
}

bool TickTimeApp(TimeAppState* state, int64_t now_ms)
{
    if (state == nullptr || state->status != TimerStatus::kRunning) {
        return false;
    }
    if (state->last_tick_ms == 0) {
        state->last_tick_ms = now_ms;
        return false;
    }
    const int elapsed_seconds = static_cast<int>((now_ms - state->last_tick_ms) / 1000);
    if (elapsed_seconds <= 0) {
        return false;
    }
    state->last_tick_ms += static_cast<int64_t>(elapsed_seconds) * 1000;
    state->remaining_seconds = std::max(0, state->remaining_seconds - elapsed_seconds);
    if (state->remaining_seconds == 0) {
        state->status = TimerStatus::kAlerting;
    }
    return true;
}

bool TimeAppHasActiveTimer(const TimeAppState& state)
{
    return state.status == TimerStatus::kRunning || state.status == TimerStatus::kPaused ||
           state.status == TimerStatus::kAlerting;
}

bool TimeAppIsEditingValue(const TimeAppState& state)
{
    return state.config_mode && state.is_editing;
}

std::string FormatTimerDuration(int total_seconds)
{
    total_seconds = std::max(0, total_seconds);
    const int hours = total_seconds / 3600;
    const int minutes = (total_seconds % 3600) / 60;
    const int seconds = total_seconds % 60;
    char buffer[16] = {};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    }
    return buffer;
}

std::string TimeAppPrimaryLine(const TimeAppState& state)
{
    if (state.status == TimerStatus::kAlerting) {
        return state.active_mode == TimerMode::kPomodoro ? "番茄钟到点" : "倒计时到点";
    }
    if (state.active_mode == TimerMode::kPomodoro && TimeAppHasActiveTimer(state)) {
        return std::string(PomodoroPhaseLabel(state.pomodoro_phase)) + " " + FormatTimerDuration(state.remaining_seconds);
    }
    if (state.active_mode == TimerMode::kCountdown && TimeAppHasActiveTimer(state)) {
        return "倒计时 " + FormatTimerDuration(state.remaining_seconds);
    }
    return "";
}

const char* PomodoroPhaseLabel(PomodoroPhase phase)
{
    switch (phase) {
        case PomodoroPhase::kFocus:
            return "专注";
        case PomodoroPhase::kBreak:
            return "休息";
        case PomodoroPhase::kLongBreak:
            return "长休";
    }
    return "专注";
}

}  // namespace wqn
