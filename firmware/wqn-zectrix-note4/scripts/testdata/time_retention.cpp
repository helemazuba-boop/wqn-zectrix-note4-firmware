#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "time_app.h"

// Include the complete production retention translation unit so checksum
// corruption can be injected into its actual retained record, without hooks
// in product code. Its UI include is a port; TimeAppState is the real header.
#include "retention.cpp"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static time_t wall = 1792627200;
extern "C" time_t __wrap_time(time_t* out) { if(out)*out=wall; return wall; }
using namespace wqn;
using namespace device_ui_internal;

static TimeAppState Running()
{
    TimeAppState s;
    s.tile = TimeTile::kCountdown;
    s.active_mode = TimerMode::kCountdown;
    s.status = TimerStatus::kRunning;
    s.remaining_seconds = 120;
    s.countdown_total_seconds = 300;
    s.session_started_unix_seconds = s.phase_started_unix_seconds = wall - 180;
    s.phase_ends_unix_seconds = wall + 120;
    s.last_tick_ms = 1000;
    std::snprintf(s.task_name, sizeof(s.task_name), "retained task");
    return s;
}
static void ConfirmTwice(TimeAppState* s)
{
    CHECK(HandleTimeAppInput(s, TimeInput::kConfirm));
    CHECK(s->action_armed);
    CHECK(HandleTimeAppInput(s, TimeInput::kConfirm));
    CHECK(!s->action_armed);
}
static void ClockCases()
{
    CHECK(RunTimeAppStateSelfTest());
    for (int64_t correction : {int64_t{3600}, int64_t{-3600}}) {
        wall = 1792627200;
        auto s = Running();
        wall += correction + 1;
        CHECK(TickTimeApp(&s, 2000));
        CHECK(s.remaining_seconds == 119 && s.status == TimerStatus::kRunning);
        CHECK(s.phase_ends_unix_seconds == wall + 119);
        CHECK(s.phase_started_unix_seconds == wall - 181);
        ConfirmTwice(&s);
        CHECK(s.status == TimerStatus::kPaused);
        const int paused_remaining = s.remaining_seconds;
        wall += correction;
        TickTimeApp(&s, 3000);
        CHECK(s.remaining_seconds == paused_remaining && s.status == TimerStatus::kPaused);
        ConfirmTwice(&s);
        CHECK(s.status == TimerStatus::kRunning && s.phase_ends_unix_seconds == wall + paused_remaining);
        TickTimeApp(&s, 4000);
        ++wall;
        TickTimeApp(&s, 5000);
        CHECK(s.remaining_seconds == paused_remaining - 1);
    }
    wall = 0;
    auto s = Running();
    s.phase_ends_unix_seconds = 0;
    wall = 1792627200;
    CHECK(TickTimeApp(&s, 2000) && s.remaining_seconds == 119);
    CHECK(s.phase_ends_unix_seconds == wall + 119);
}
static void RetentionCases()
{
    wall = 1792627200;
    auto s = Running();
    s.action_armed = true;
    RetainTimeAppState(s);
    CHECK(HasRetainedTimeApp());
    TimeAppState restored;
    wall += 30;
    CHECK(RestoreRetainedTimeApp(&restored));
    CHECK(restored.remaining_seconds == 90 && restored.last_tick_ms == 0);
    CHECK(!restored.action_armed && std::string(restored.task_name) == "retained task");
    CHECK(TickTimeApp(&restored, 1000) == false);
    ++wall;
    CHECK(TickTimeApp(&restored, 2000) && restored.remaining_seconds == 89);
    RetainTimeAppState(restored);
    wall += 100;
    CHECK(RestoreRetainedTimeApp(&restored) && restored.status == TimerStatus::kAlerting);
    CHECK(restored.remaining_seconds == 0 && restored.tile == TimeTile::kCountdown);
    wall = 1792627200;
    s = Running(); s.status = TimerStatus::kPaused;
    RetainTimeAppState(s); wall += 3600;
    CHECK(RestoreRetainedTimeApp(&restored));
    CHECK(restored.remaining_seconds == 120 && restored.status == TimerStatus::kPaused);
    ConfirmTwice(&restored);
    CHECK(restored.phase_ends_unix_seconds == wall + 120);
    wall = 1792627200;
    s = Running(); RetainTimeAppState(s);
    wall = 0;
    CHECK(RestoreRetainedTimeApp(&restored) && restored.remaining_seconds == 120);
    wall = 1792627200;
    s = Running(); RetainTimeAppState(s);
    g_rtc_time_app.checksum ^= 1;
    CHECK(!HasRetainedTimeApp() && !RestoreRetainedTimeApp(&restored));
    s.remaining_seconds = 8000; RetainTimeAppState(s);
    CHECK(!HasRetainedTimeApp());
    RetainTimeAppState({});
    CHECK(!HasRetainedTimeApp() && !RestoreRetainedTimeApp(&restored));
    CHECK(!RestoreRetainedTimeApp(nullptr));

    // A backwards wall clock (including a fallback build seed) cannot grant
    // more timer budget than the value that was saved before sleep.
    wall = 1792627200;
    s = Running(); RetainTimeAppState(s); wall -= 3600;
    CHECK(RestoreRetainedTimeApp(&restored));
    CHECK(restored.remaining_seconds == 120);
    CHECK(TickTimeApp(&restored, 1000));  // Rebase timestamps to this wall clock.
    CHECK(restored.phase_ends_unix_seconds == wall + 120);
}
static void PomodoroCases()
{
    wall = 1792627200;
    TimeAppState s;
    s.tile = TimeTile::kPomodoro; s.config_mode = true;
    s.pomodoro_rounds = 2; s.pomodoro_focus_minutes = 1;
    s.pomodoro_break_minutes = 1; s.pomodoro_long_break_minutes = 1;
    s.active_field = TimeAppStartField(s.tile);
    CHECK(HandleTimeAppInput(&s, TimeInput::kConfirm));
    CHECK(s.status == TimerStatus::kRunning && s.remaining_seconds == 60);
    for (int phase = 0; phase < 4; ++phase) {
        const auto expected_phase = phase == 1 ? PomodoroPhase::kBreak
            : phase == 3 ? PomodoroPhase::kLongBreak : PomodoroPhase::kFocus;
        CHECK(s.pomodoro_phase == expected_phase);
        CHECK(s.pomodoro_current_round == (phase < 2 ? 1 : 2));
        TickTimeApp(&s, 1000);
        wall += 30;
        TickTimeApp(&s, 31000);
        CHECK(s.remaining_seconds == 30);
        RetainTimeAppState(s); wall += 31;
        CHECK(RestoreRetainedTimeApp(&s));
        CHECK(s.status == TimerStatus::kAlerting && s.remaining_seconds == 0);
        CHECK(PomodoroGroupComplete(s) == (phase == 3));
        wall += 10;
        TickTimeApp(&s, 1000);
        CHECK(s.status == TimerStatus::kAlerting);  // Explicit confirm advances.
        ConfirmTwice(&s);
        if (phase != 3) CHECK(s.status == TimerStatus::kRunning && s.remaining_seconds == 60);
    }
    CHECK(s.status == TimerStatus::kIdle && s.active_mode == TimerMode::kNone);
    RetainTimeAppState(s); CHECK(!HasRetainedTimeApp());
}
int main()
{
    ClockCases(); RetentionCases(); PomodoroCases();
    std::printf("Timer/wall-clock/retention: %u checks PASS\n",checks);
}
