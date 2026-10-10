// Clock utilities: epoch time, build-time seeding, formatted labels, time-app state queries.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <cstdio>
#include <ctime>

#include "power/rtc_timekeep.h"

namespace device_ui_internal {

std::time_t CurrentUnixTime()
{
    std::time_t now = 0;
    std::time(&now);
    return now;
}

bool SystemClockIsReasonable()
{
    return CurrentUnixTime() >= kMinReasonableUnixTime;
}

void SeedClockFromBuildTimeIfNeeded()
{
    (void)wqn::power::timekeep::SeedSystemTimeFromBuildTimeIfNeeded();
}

std::string CurrentDateLabel()
{
    const std::time_t now = CurrentUnixTime();
    if (now < kMinReasonableUnixTime) {
        return "--月--日";
    }

    std::tm time_info = {};
    localtime_r(&now, &time_info);
    static constexpr const char* kWeekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
    };
    char buffer[32] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d月%d日 %s",
        time_info.tm_mon + 1,
        time_info.tm_mday,
        kWeekdays[time_info.tm_wday]);
    return buffer;
}

std::string CurrentClockLabel()
{
    const std::time_t now = CurrentUnixTime();
    if (now < kMinReasonableUnixTime) {
        return "--:--";
    }

    std::tm time_info = {};
    localtime_r(&now, &time_info);
    char buffer[8] = {};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M", &time_info) == 0) {
        return "--:--";
    }
    return buffer;
}

std::string CurrentIsoTimestamp()
{
    const std::time_t now = CurrentUnixTime();
    if (now < kMinReasonableUnixTime) {
        return "";
    }

    std::tm time_info = {};
    gmtime_r(&now, &time_info);
    char buffer[24] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &time_info) == 0) {
        return "";
    }
    return buffer;
}

const char* TimeTileTitle(wqn::TimeTile tile)
{
    switch (tile) {
        case wqn::TimeTile::kCountdown:
            return "倒计时";
        case wqn::TimeTile::kPomodoro:
            return "番茄钟";
        case wqn::TimeTile::kClock:
        default:
            return "时间";
    }
}

std::string TwoDigit(int value)
{
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d", std::max(0, value));
    return buffer;
}

int CountdownStartField()
{
    return wqn::TimeAppStartField(wqn::TimeTile::kCountdown);
}

int PomodoroStartField()
{
    return wqn::TimeAppStartField(wqn::TimeTile::kPomodoro);
}

std::string ChooseHomePrimaryTimeLine(const wqn::TimeAppState& time_app)
{
    const std::string active_timer = wqn::TimeAppPrimaryLine(time_app);
    if (!active_timer.empty()) {
        return active_timer;
    }
    return CurrentClockLabel();
}

void UpdateHomePrimaryTimeLine(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }
    state->home.primary_time_line = ChooseHomePrimaryTimeLine(state->time_app);
}

bool ShouldRefreshTimeTick(const wqn::UiState& state)
{
    if (state.screen != wqn::UiScreen::kHome && state.screen != wqn::UiScreen::kTime) {
        return false;
    }
    return wqn::TimeAppHasActiveTimer(state.time_app);
}

bool ScreenUsesClockMinute(const wqn::UiState& state)
{
    if (state.screen == wqn::UiScreen::kHome) {
        // The home primary line becomes a static timer-status sentence while
        // a timer runs, but the status-bar wall clock must keep its normal
        // minute cadence.
        return true;
    }
    if (state.screen == wqn::UiScreen::kTodo) {
        return true;
    }
    return state.screen == wqn::UiScreen::kTime && state.time_app.tile == wqn::TimeTile::kClock &&
           !state.time_app.config_mode;
}

}  // namespace device_ui_internal
