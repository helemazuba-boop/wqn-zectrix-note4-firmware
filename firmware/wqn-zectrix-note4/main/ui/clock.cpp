// Clock utilities: epoch time, build-time seeding, formatted labels, time-app state queries.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

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

int MonthIndexFromBuildDate(const char* month)
{
    static constexpr const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int i = 0; i < 12; ++i) {
        if (strncmp(month, kMonths[i], 3) == 0) {
            return i;
        }
    }
    return 0;
}

void SeedClockFromBuildTimeIfNeeded()
{
    setenv("TZ", "CST-8", 1);
    tzset();
    if (SystemClockIsReasonable()) {
        return;
    }

    char month[4] = {};
    int day = 1;
    int year = 2026;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
        std::sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        ESP_LOGW(kTag, "build time parse failed; clock remains invalid");
        return;
    }

    std::tm build_tm = {};
    build_tm.tm_year = year - 1900;
    build_tm.tm_mon = MonthIndexFromBuildDate(month);
    build_tm.tm_mday = day;
    build_tm.tm_hour = hour;
    build_tm.tm_min = minute;
    build_tm.tm_sec = second;
    build_tm.tm_isdst = -1;
    const std::time_t build_time = std::mktime(&build_tm);
    if (build_time < kMinReasonableUnixTime) {
        ESP_LOGW(kTag, "build time is not reasonable; clock remains invalid");
        return;
    }
    const timeval tv = {
        .tv_sec = build_time,
        .tv_usec = 0,
    };
    settimeofday(&tv, nullptr);
    ESP_LOGI(kTag, "clock seeded from build time: %04d-%02d-%02d %02d:%02d:%02d CST", year, build_tm.tm_mon + 1, day, hour, minute, second);
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
