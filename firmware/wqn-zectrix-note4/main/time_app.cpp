#include "time_app.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {

constexpr int kCountdownMaxSeconds = 7200;
constexpr int kPomodoroMinValue = 1;
constexpr int kPomodoroMaxValue = 99;
constexpr int kActionArmedTimeoutSeconds = 5;
constexpr int64_t kMinReasonableUnixSeconds = 1704067200;  // 2024-01-01 UTC

int ClampInt(int value, int min_value, int max_value)
{
    return std::min(max_value, std::max(min_value, value));
}

int64_t CurrentUnixSeconds()
{
    return static_cast<int64_t>(std::time(nullptr));
}

void SetActionArmed(wqn::TimeAppState* state, bool armed)
{
    state->action_armed = armed;
    state->action_armed_at_unix_seconds = armed ? CurrentUnixSeconds() : 0;
}

wqn::TimeTile ActiveTimerTile(const wqn::TimeAppState& state)
{
    return state.active_mode == wqn::TimerMode::kPomodoro
        ? wqn::TimeTile::kPomodoro
        : wqn::TimeTile::kCountdown;
}

void RevealActiveTimer(wqn::TimeAppState* state)
{
    if (state->active_mode != wqn::TimerMode::kNone) {
        state->tile = ActiveTimerTile(*state);
    }
    state->config_mode = false;
    state->is_editing = false;
    SetActionArmed(state, false);
}

void ArmPhaseTiming(wqn::TimeAppState* state, int duration_seconds, bool reset_start)
{
    const int64_t now = CurrentUnixSeconds();
    if (reset_start || state->phase_started_unix_seconds <= 0) {
        state->phase_started_unix_seconds = now;
    }
    if (state->session_started_unix_seconds <= 0) {
        state->session_started_unix_seconds = state->phase_started_unix_seconds;
    }
    state->phase_ends_unix_seconds = now + std::max(0, duration_seconds);
    state->paused_at_unix_seconds = 0;
    state->last_tick_ms = 0;
    SetActionArmed(state, false);
}

int SecondsBeforeCurrentPhase(const wqn::TimeAppState& state)
{
    if (state.active_mode != wqn::TimerMode::kPomodoro) {
        return 0;
    }
    const int completed_rounds = std::max(0, state.pomodoro_current_round - 1);
    int seconds = completed_rounds *
        (state.pomodoro_focus_minutes + state.pomodoro_break_minutes) * 60;
    if (state.pomodoro_phase != wqn::PomodoroPhase::kFocus) {
        seconds += state.pomodoro_focus_minutes * 60;
    }
    return seconds;
}

bool AlignWallClockTiming(wqn::TimeAppState* state, int64_t now_unix_seconds)
{
    if (now_unix_seconds < kMinReasonableUnixSeconds ||
        state->status != wqn::TimerStatus::kRunning ||
        state->remaining_seconds <= 0) {
        return false;
    }
    const int total_seconds = wqn::TimeAppPhaseTotalSeconds(*state);
    const int elapsed_seconds = std::clamp(
        total_seconds - state->remaining_seconds,
        0,
        total_seconds);
    const int64_t expected_end = now_unix_seconds + state->remaining_seconds;
    if (state->phase_ends_unix_seconds < kMinReasonableUnixSeconds) {
        state->phase_ends_unix_seconds = expected_end;
        state->phase_started_unix_seconds = now_unix_seconds - elapsed_seconds;
        state->session_started_unix_seconds =
            state->phase_started_unix_seconds - SecondsBeforeCurrentPhase(*state);
        return true;
    }

    const int64_t correction = expected_end - state->phase_ends_unix_seconds;
    if (correction >= -2 && correction <= 2) {
        return false;
    }
    state->phase_ends_unix_seconds = expected_end;
    if (state->phase_started_unix_seconds >= kMinReasonableUnixSeconds) {
        state->phase_started_unix_seconds += correction;
    } else {
        state->phase_started_unix_seconds = now_unix_seconds - elapsed_seconds;
    }
    if (state->session_started_unix_seconds >= kMinReasonableUnixSeconds) {
        state->session_started_unix_seconds += correction;
    } else {
        state->session_started_unix_seconds =
            state->phase_started_unix_seconds - SecondsBeforeCurrentPhase(*state);
    }
    return true;
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
    SetActionArmed(state, false);
}

void ExitToClock(wqn::TimeAppState* state)
{
    state->tile = wqn::TimeTile::kClock;
    state->config_mode = false;
    state->is_editing = false;
    state->active_field = 0;
    SetActionArmed(state, false);
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
    state->session_started_unix_seconds = 0;
    state->config_mode = false;
    state->is_editing = false;
    state->active_field = 0;
    ArmPhaseTiming(state, state->remaining_seconds, true);
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
    state->session_started_unix_seconds = 0;
    state->config_mode = false;
    state->is_editing = false;
    state->active_field = 0;
    ArmPhaseTiming(state, state->remaining_seconds, true);
}

void ToggleRunPause(wqn::TimeAppState* state)
{
    if (state->status == wqn::TimerStatus::kRunning) {
        state->status = wqn::TimerStatus::kPaused;
        state->paused_at_unix_seconds = CurrentUnixSeconds();
        SetActionArmed(state, false);
    } else if (state->status == wqn::TimerStatus::kPaused) {
        state->status = wqn::TimerStatus::kRunning;
        ArmPhaseTiming(state, state->remaining_seconds, false);
    }
}

void StopTimer(wqn::TimeAppState* state)
{
    state->status = wqn::TimerStatus::kIdle;
    state->active_mode = wqn::TimerMode::kNone;
    state->remaining_seconds = 0;
    state->last_tick_ms = 0;
    state->session_started_unix_seconds = 0;
    state->phase_started_unix_seconds = 0;
    state->phase_ends_unix_seconds = 0;
    state->paused_at_unix_seconds = 0;
    SetActionArmed(state, false);
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
        ArmPhaseTiming(state, state->remaining_seconds, true);
        return;
    }

    if (state->pomodoro_current_round >= state->pomodoro_rounds) {
        StopTimer(state);
        return;
    }

    ++state->pomodoro_current_round;
    state->pomodoro_phase = wqn::PomodoroPhase::kFocus;
    state->remaining_seconds = state->pomodoro_focus_minutes * 60;
    ArmPhaseTiming(state, state->remaining_seconds, true);
}

void ExecutePrimaryAction(wqn::TimeAppState* state)
{
    SetActionArmed(state, false);
    if (state->status == wqn::TimerStatus::kPaused) {
        ToggleRunPause(state);
        return;
    }
    if (state->status != wqn::TimerStatus::kRunning) {
        return;
    }
    if (state->active_mode != wqn::TimerMode::kPomodoro ||
        state->pomodoro_phase == wqn::PomodoroPhase::kFocus) {
        ToggleRunPause(state);
        return;
    }
    if (state->pomodoro_phase == wqn::PomodoroPhase::kLongBreak) {
        StopTimer(state);
        return;
    }
    // A short break uses the reference design's explicit “跳过” action.
    AdvancePomodoroPhase(state);
}

bool TickTimeAppAt(
    wqn::TimeAppState* state,
    int64_t now_ms,
    int64_t now_unix_seconds)
{
    if (state == nullptr) {
        return false;
    }
    bool changed = false;
    if (state->action_armed && state->action_armed_at_unix_seconds > 0 &&
        now_unix_seconds - state->action_armed_at_unix_seconds >=
            kActionArmedTimeoutSeconds) {
        SetActionArmed(state, false);
        changed = true;
    }
    if (state->status != wqn::TimerStatus::kRunning) {
        return changed;
    }
    if (state->last_tick_ms == 0) {
        state->last_tick_ms = now_ms;
        return AlignWallClockTiming(state, now_unix_seconds) || changed;
    }
    const int elapsed_seconds =
        static_cast<int>((now_ms - state->last_tick_ms) / 1000);
    if (elapsed_seconds <= 0) {
        return AlignWallClockTiming(state, now_unix_seconds) || changed;
    }
    state->last_tick_ms += static_cast<int64_t>(elapsed_seconds) * 1000;
    state->remaining_seconds =
        std::max(0, state->remaining_seconds - elapsed_seconds);
    changed = true;
    AlignWallClockTiming(state, now_unix_seconds);
    if (state->remaining_seconds == 0) {
        state->status = wqn::TimerStatus::kAlerting;
        RevealActiveTimer(state);
    }
    return changed;
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

    if (input == TimeInput::kLongConfirm) {
        if (state->status == TimerStatus::kRunning || state->status == TimerStatus::kPaused ||
            state->status == TimerStatus::kAlerting) {
            StopTimer(state);
            return true;
        }
        ExitToClock(state);
        return true;
    }

    // A running timer may be deliberately backgrounded on the clock tile.
    // The first navigation/Confirm only reveals the correct active-mode page;
    // it must never arm or execute an invisible action.
    if (TimeAppHasActiveTimer(*state) && state->tile == TimeTile::kClock &&
        (input == TimeInput::kUp || input == TimeInput::kDown || input == TimeInput::kConfirm)) {
        RevealActiveTimer(state);
        return true;
    }

    if (state->status == TimerStatus::kAlerting) {
        if (input == TimeInput::kConfirm) {
            if (!state->action_armed) {
                SetActionArmed(state, true);
            } else if (state->active_mode == TimerMode::kPomodoro) {
                SetActionArmed(state, false);
                state->status = TimerStatus::kRunning;
                AdvancePomodoroPhase(state);
            } else {
                StopTimer(state);
            }
        } else if (state->action_armed) {
            SetActionArmed(state, false);
        }
        return before.action_armed != state->action_armed || before.status != state->status ||
               before.active_mode != state->active_mode || before.pomodoro_phase != state->pomodoro_phase;
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

    if (state->action_armed &&
        (input == TimeInput::kUp || input == TimeInput::kDown ||
         input == TimeInput::kLongUp || input == TimeInput::kLongDown)) {
        SetActionArmed(state, false);
        return true;
    }

    if (input == TimeInput::kConfirm) {
        if (state->status == TimerStatus::kIdle) {
            EnterConfig(state);
        } else if (!state->action_armed) {
            SetActionArmed(state, true);
        } else {
            ExecutePrimaryAction(state);
        }
    } else if (state->tile == TimeTile::kCountdown && input == TimeInput::kDown) {
        ExitToClock(state);
    } else if (state->tile == TimeTile::kPomodoro && input == TimeInput::kUp) {
        ExitToClock(state);
    }

    return before.tile != state->tile || before.status != state->status || before.config_mode != state->config_mode ||
           before.is_editing != state->is_editing || before.active_field != state->active_field ||
           before.remaining_seconds != state->remaining_seconds || before.action_armed != state->action_armed;
}

bool TickTimeApp(TimeAppState* state, int64_t now_ms)
{
    return TickTimeAppAt(state, now_ms, CurrentUnixSeconds());
}

bool TimeAppHasActiveTimer(const TimeAppState& state)
{
    return state.active_mode != TimerMode::kNone &&
           (state.status == TimerStatus::kRunning || state.status == TimerStatus::kPaused ||
            state.status == TimerStatus::kAlerting);
}

bool TimeAppIsEditingValue(const TimeAppState& state)
{
    return state.config_mode && state.is_editing;
}

void DisarmTimeAppAction(TimeAppState* state)
{
    if (state != nullptr) {
        SetActionArmed(state, false);
    }
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
        if (state.active_mode == TimerMode::kCountdown) {
            return "倒计时到点";
        }
        if (state.pomodoro_phase == PomodoroPhase::kFocus) {
            return "番茄钟第 " + std::to_string(state.pomodoro_current_round) + " 轮专注完成";
        }
        return state.pomodoro_phase == PomodoroPhase::kLongBreak
            ? "番茄钟本组完成"
            : "番茄钟短休息结束";
    }
    if (state.active_mode == TimerMode::kPomodoro && TimeAppHasActiveTimer(state)) {
        if (state.status == TimerStatus::kPaused) {
            return state.pomodoro_phase == PomodoroPhase::kFocus
                ? "番茄钟第 " + std::to_string(state.pomodoro_current_round) + " 轮已暂停"
                : state.pomodoro_phase == PomodoroPhase::kLongBreak
                    ? "番茄钟长休息已暂停"
                    : "番茄钟短休息已暂停";
        }
        switch (state.pomodoro_phase) {
            case PomodoroPhase::kBreak:
                return "番茄钟短休息中";
            case PomodoroPhase::kLongBreak:
                return "番茄钟长休息中";
            case PomodoroPhase::kFocus:
            default:
                return "番茄钟第 " + std::to_string(state.pomodoro_current_round) + "/" +
                       std::to_string(state.pomodoro_rounds) + " 轮专注中";
        }
    }
    if (state.active_mode == TimerMode::kCountdown && TimeAppHasActiveTimer(state)) {
        return state.status == TimerStatus::kPaused ? "倒计时已暂停" : "倒计时进行中";
    }
    return "";
}

bool PomodoroGroupComplete(const TimeAppState& state)
{
    return state.active_mode == TimerMode::kPomodoro &&
           state.status == TimerStatus::kAlerting &&
           state.pomodoro_phase == PomodoroPhase::kLongBreak &&
           state.pomodoro_current_round >= state.pomodoro_rounds;
}

int PomodoroGroupTotalSeconds(const TimeAppState& state)
{
    const int rounds = std::max(1, state.pomodoro_rounds);
    return rounds * state.pomodoro_focus_minutes * 60 +
           std::max(0, rounds - 1) * state.pomodoro_break_minutes * 60 +
           state.pomodoro_long_break_minutes * 60;
}

int TimeAppPhaseTotalSeconds(const TimeAppState& state)
{
    if (state.active_mode == TimerMode::kCountdown) {
        return std::max(1, state.countdown_total_seconds);
    }
    if (state.active_mode == TimerMode::kPomodoro) {
        switch (state.pomodoro_phase) {
            case PomodoroPhase::kBreak:
                return std::max(1, state.pomodoro_break_minutes * 60);
            case PomodoroPhase::kLongBreak:
                return std::max(1, state.pomodoro_long_break_minutes * 60);
            case PomodoroPhase::kFocus:
            default:
                return std::max(1, state.pomodoro_focus_minutes * 60);
        }
    }
    return 1;
}

int TimeAppVisualProgressSeconds(const TimeAppState& state)
{
    const int total = TimeAppPhaseTotalSeconds(state);
    const int elapsed = std::clamp(total - state.remaining_seconds, 0, total);
    if (state.status != TimerStatus::kRunning) {
        return elapsed;
    }
    // At most eight running updates per phase and never more often than once
    // per minute. The logical timer still advances every second.
    const int step_seconds = std::max(60, (total + 7) / 8);
    return std::min(total, elapsed / step_seconds * step_seconds);
}

int TimeAppVisualProgressBucket(const TimeAppState& state)
{
    const int total = TimeAppPhaseTotalSeconds(state);
    const int step_seconds = std::max(60, (total + 7) / 8);
    return TimeAppVisualProgressSeconds(state) / step_seconds;
}

void TimeAppVisibleRoundWindow(
    const TimeAppState& state,
    int max_visible_rounds,
    int* first_round,
    int* last_round)
{
    const int rounds = std::max(1, state.pomodoro_rounds);
    const int visible = std::clamp(max_visible_rounds, 1, rounds);
    const int current_index =
        std::clamp(state.pomodoro_current_round - 1, 0, rounds - 1);
    const int start = rounds <= visible
        ? 0
        : std::clamp(current_index - 1, 0, rounds - visible);
    if (first_round != nullptr) {
        *first_round = start + 1;
    }
    if (last_round != nullptr) {
        *last_round = start + visible;
    }
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

bool RunTimeAppStateSelfTest()
{
    TimeAppState countdown;
    countdown.tile = TimeTile::kCountdown;
    countdown.active_mode = TimerMode::kCountdown;
    countdown.status = TimerStatus::kRunning;
    countdown.remaining_seconds = 120;
    if (!HandleTimeAppInput(&countdown, TimeInput::kConfirm) || !countdown.action_armed) {
        return false;
    }
    if (!HandleTimeAppInput(&countdown, TimeInput::kConfirm) ||
        countdown.status != TimerStatus::kPaused || countdown.action_armed) {
        return false;
    }
    countdown.action_armed = true;
    countdown.action_armed_at_unix_seconds = 100;
    if (!TickTimeAppAt(
            &countdown,
            1000,
            100 + kActionArmedTimeoutSeconds) ||
        countdown.action_armed) {
        return false;
    }

    TimeAppState hidden;
    hidden.tile = TimeTile::kClock;
    hidden.active_mode = TimerMode::kPomodoro;
    hidden.status = TimerStatus::kAlerting;
    hidden.pomodoro_phase = PomodoroPhase::kFocus;
    if (!HandleTimeAppInput(&hidden, TimeInput::kConfirm) ||
        hidden.tile != TimeTile::kPomodoro || hidden.action_armed) {
        return false;
    }

    TimeAppState short_break;
    short_break.tile = TimeTile::kPomodoro;
    short_break.active_mode = TimerMode::kPomodoro;
    short_break.status = TimerStatus::kRunning;
    short_break.pomodoro_phase = PomodoroPhase::kBreak;
    short_break.pomodoro_current_round = 2;
    short_break.pomodoro_rounds = 4;
    if (!HandleTimeAppInput(&short_break, TimeInput::kConfirm) ||
        !HandleTimeAppInput(&short_break, TimeInput::kConfirm) ||
        short_break.pomodoro_phase != PomodoroPhase::kFocus ||
        short_break.pomodoro_current_round != 3) {
        return false;
    }

    TimeAppState long_break = short_break;
    long_break.status = TimerStatus::kRunning;
    long_break.pomodoro_phase = PomodoroPhase::kLongBreak;
    long_break.pomodoro_current_round = long_break.pomodoro_rounds;
    if (!HandleTimeAppInput(&long_break, TimeInput::kConfirm) ||
        !HandleTimeAppInput(&long_break, TimeInput::kConfirm) ||
        long_break.status != TimerStatus::kIdle) {
        return false;
    }

    TimeAppState corrected_clock;
    corrected_clock.tile = TimeTile::kCountdown;
    corrected_clock.active_mode = TimerMode::kCountdown;
    corrected_clock.status = TimerStatus::kRunning;
    corrected_clock.countdown_total_seconds = 300;
    corrected_clock.remaining_seconds = 120;
    corrected_clock.last_tick_ms = 1000;
    constexpr int64_t kTestNow = 1800000000;
    if (!TickTimeAppAt(&corrected_clock, 2000, kTestNow) ||
        corrected_clock.remaining_seconds != 119 ||
        corrected_clock.phase_ends_unix_seconds != kTestNow + 119 ||
        corrected_clock.phase_started_unix_seconds != kTestNow - 181) {
        return false;
    }
    const int64_t corrected_end = corrected_clock.phase_ends_unix_seconds;
    if (!TickTimeAppAt(&corrected_clock, 3000, kTestNow + 3601) ||
        corrected_clock.remaining_seconds != 118 ||
        corrected_clock.phase_ends_unix_seconds != corrected_end + 3600) {
        return false;
    }

    TimeAppState visual_progress;
    visual_progress.active_mode = TimerMode::kPomodoro;
    visual_progress.status = TimerStatus::kRunning;
    visual_progress.pomodoro_phase = PomodoroPhase::kFocus;
    visual_progress.pomodoro_focus_minutes = 25;
    visual_progress.remaining_seconds = 1500;
    if (TimeAppVisualProgressBucket(visual_progress) != 0) {
        return false;
    }
    visual_progress.remaining_seconds = 1312;
    if (TimeAppVisualProgressBucket(visual_progress) != 1) {
        return false;
    }

    TimeAppState many_rounds;
    many_rounds.pomodoro_rounds = 99;
    int first_round = 0;
    int last_round = 0;
    many_rounds.pomodoro_current_round = 1;
    TimeAppVisibleRoundWindow(many_rounds, 4, &first_round, &last_round);
    if (first_round != 1 || last_round != 4) {
        return false;
    }
    many_rounds.pomodoro_current_round = 50;
    TimeAppVisibleRoundWindow(many_rounds, 4, &first_round, &last_round);
    if (first_round != 49 || last_round != 52) {
        return false;
    }
    many_rounds.pomodoro_current_round = 99;
    TimeAppVisibleRoundWindow(many_rounds, 4, &first_round, &last_round);
    if (first_round != 96 || last_round != 99) {
        return false;
    }
    return true;
}

}  // namespace wqn
