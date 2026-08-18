// Time page rendering: standby clock, countdown config, pomodoro config, timer run.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

#include "display_service.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

void DrawStatusBar(const char* title, const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    wqn::DrawUtf8Text(10, 6, title, true);
    // Right-edge icon cluster (wifi + battery), then time to its left.
    const int icons_left = DrawStatusBarIcons(wqn::kEpdWidth - 10, 6, home);
    const std::string time_str = CurrentClockLabel();
    const int time_w = wqn::MeasureUtf8TextWidth(time_str.c_str());
    const int time_x = icons_left - 6 - time_w;
    if (time_x >= 10) {
        wqn::DrawUtf8Text(time_x, 6, time_str.c_str(), true);
    }
}

void DrawClockStatusBar(const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    wqn::DrawUtf8Text(10, 6, TimeTileTitle(wqn::TimeTile::kClock), true);
    DrawStatusBarIcons(wqn::kEpdWidth - 10, 6, home);
}

void DrawTimerStatusBar(const char* title, const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    wqn::DrawUtf8Text(10, 6, title, true);
    // A running timer page is intentionally event-driven. Omitting the wall
    // clock avoids leaving a minute label on screen after it becomes stale;
    // the fixed, second-accurate phase endpoint is the hero instead.
    DrawStatusBarIcons(wqn::kEpdWidth - 10, 6, home);
}

void DrawProgressBar(int x, int y, int width, int height, int current, int total)
{
    // [v2] Rounded container outline (progress bar is a content container).
    // Shared by the timer page and the settings storage/NVS/PSRAM bars.
    DrawRoundedRect(x, y, width, height, kChipRadius);
    if (total <= 0) {
        return;
    }
    const int filled = std::min(width - 2, std::max(0, (width - 2) * current / total));
    if (filled > 0) {
        FillRect(x + 1, y + 1, filled, height - 2, true);
    }
}

void DrawConfigBox(int x, int y, int width, int height, const std::string& value, const std::string& label, bool selected)
{
    // [v2] Focus decoration: rounded reverse-fill (kInvert) when selected --
    // config fields are will-execute/instant-edit targets. Unselected is a
    // rounded container outline (the square DrawRect box is retired).
    if (selected) {
        DrawSelectionDecoration(x, y, width, height, SelectionStyle::kInvert);
    } else {
        DrawRoundedRect(x, y, width, height, kChipRadius);
    }
    const bool black_text = !selected;
    // Value rendered from the shared 1bpp 16px digit assets; label stays CJK.
    DrawConfigDigitsCentered(x, y + 7, width, value, black_text);
    DrawCenteredText(x, y + height - 20, width, label, black_text);
}

void DrawActionBox(int x, int y, int width, const std::string& label, bool selected)
{
    constexpr int kActionBoxHeight = 28;
    // [v2] Focus: rounded reverse-fill (kInvert) when selected (action buttons
    // execute-on-confirm); plain rounded outline otherwise.
    if (selected) {
        DrawSelectionDecoration(x, y, width, kActionBoxHeight, SelectionStyle::kInvert);
    } else {
        DrawRoundedRect(x, y, width, kActionBoxHeight, kChipRadius);
    }
    DrawCenteredText(x, y + 6, width, label, !selected);
}

esp_err_t RenderClockStandbyContent()
{
    DrawStandbyClockDigits(80, CurrentClockLabel());
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 174, wqn::kEpdWidth, CurrentDateLabel()), kTag, "draw clock date");
    return ESP_OK;
}

esp_err_t RenderTimeClockRegion(RefreshSchedule schedule, bool include_date)
{
    (void)include_date;
    ClearRect(kTimeStandbyRect);
    ESP_RETURN_ON_ERROR(RenderClockStandbyContent(), kTag, "draw clock standby region");
    return RefreshRegion(kTimeStandbyRect, schedule);
}

esp_err_t RenderCountdownConfigToEpd(const wqn::TimeAppState& time_app)
{
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 83, wqn::kEpdWidth, "倒计时设置"), kTag, "draw countdown config title");
    const int y = 116;
    DrawConfigBox(102, y, 56, 62, TwoDigit(time_app.countdown_hours), "时", time_app.active_field == 0);
    DrawConfigDigitsCentered(164, y + 22, 10, ":", true);
    DrawConfigBox(178, y, 56, 62, TwoDigit(time_app.countdown_minutes), "分", time_app.active_field == 1);
    DrawConfigDigitsCentered(240, y + 22, 10, ":", true);
    DrawConfigBox(254, y, 56, 62, TwoDigit(time_app.countdown_seconds), "秒", time_app.active_field == 2);
    DrawActionBox(130, 205, 68, "开始", time_app.active_field == CountdownStartField());
    DrawActionBox(212, 205, 68, "退出", time_app.active_field == CountdownStartField() + 1);
    if (time_app.is_editing) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 248, wqn::kEpdWidth, "正在调整"), kTag, "draw editing hint");
    }
    return ESP_OK;
}

esp_err_t RenderPomodoroConfigToEpd(const wqn::TimeAppState& time_app)
{
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 66, wqn::kEpdWidth, "番茄钟设置"), kTag, "draw pomodoro config title");
    DrawConfigBox(108, 95, 88, 52, std::to_string(time_app.pomodoro_rounds), "轮数", time_app.active_field == 0);
    DrawConfigBox(
        204,
        95,
        88,
        52,
        std::to_string(time_app.pomodoro_focus_minutes),
        "专注",
        time_app.active_field == 1);
    DrawConfigBox(
        108,
        155,
        88,
        52,
        std::to_string(time_app.pomodoro_break_minutes),
        "休息",
        time_app.active_field == 2);
    DrawConfigBox(
        204,
        155,
        88,
        52,
        std::to_string(time_app.pomodoro_long_break_minutes),
        "长休息",
        time_app.active_field == 3);
    DrawActionBox(130, 226, 68, "开始", time_app.active_field == PomodoroStartField());
    DrawActionBox(212, 226, 68, "退出", time_app.active_field == PomodoroStartField() + 1);
    return ESP_OK;
}

int TimerInitialSeconds(const wqn::TimeAppState& time_app)
{
    return wqn::TimeAppPhaseTotalSeconds(time_app);
}

std::string FormatUnixClock(int64_t unix_seconds, bool include_seconds = false)
{
    if (unix_seconds <= 0) {
        return include_seconds ? "--:--:--" : "--:--";
    }
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm local = {};
    localtime_r(&value, &local);
    char buffer[12] = {};
    if (std::strftime(
            buffer,
            sizeof(buffer),
            include_seconds ? "%H:%M:%S" : "%H:%M",
            &local) == 0) {
        return include_seconds ? "--:--:--" : "--:--";
    }
    return buffer;
}

bool TimesAreOnDifferentDays(int64_t start_seconds, int64_t end_seconds)
{
    if (start_seconds <= 0 || end_seconds <= 0) {
        return false;
    }
    const std::time_t start = static_cast<std::time_t>(start_seconds);
    const std::time_t endpoint = static_cast<std::time_t>(end_seconds);
    std::tm start_local = {};
    std::tm endpoint_local = {};
    localtime_r(&start, &start_local);
    localtime_r(&endpoint, &endpoint_local);
    return start_local.tm_year != endpoint_local.tm_year ||
           start_local.tm_yday != endpoint_local.tm_yday;
}

std::string FormatUnixDate(int64_t unix_seconds)
{
    if (unix_seconds <= 0) {
        return "-- --";
    }
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm local = {};
    localtime_r(&value, &local);
    char buffer[8] = {};
    if (std::strftime(buffer, sizeof(buffer), "%m-%d", &local) == 0) {
        return "-- --";
    }
    return buffer;
}

std::string FormatUnixDateClock(int64_t unix_seconds)
{
    if (unix_seconds <= 0) {
        return "-- -- --:--";
    }
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm local = {};
    localtime_r(&value, &local);
    char buffer[16] = {};
    if (std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &local) == 0) {
        return "-- -- --:--";
    }
    return buffer;
}

std::string TimerStatusChipLabel(const wqn::TimeAppState& time_app)
{
    if (time_app.status == wqn::TimerStatus::kPaused) {
        return "暂停";
    }
    if (time_app.status == wqn::TimerStatus::kAlerting) {
        return "完成";
    }
    if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        if (time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus) {
            return "第 " + std::to_string(time_app.pomodoro_current_round) + " / " +
                   std::to_string(time_app.pomodoro_rounds);
        }
        return time_app.pomodoro_phase == wqn::PomodoroPhase::kLongBreak
            ? "长休息"
            : "短休息";
    }
    return "进行中";
}

std::string TimerHeroStateLabel(const wqn::TimeAppState& time_app)
{
    if (time_app.status == wqn::TimerStatus::kPaused) {
        return time_app.active_mode == wqn::TimerMode::kPomodoro
            ? "番茄钟暂停"
            : "已暂停";
    }
    if (time_app.status == wqn::TimerStatus::kAlerting) {
        if (time_app.active_mode == wqn::TimerMode::kCountdown) {
            return "倒计时完成";
        }
        return PomodoroGroupComplete(time_app)
            ? "本组完成"
            : (time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus
                ? "专注完成"
                : "休息结束");
    }
    if (time_app.active_mode == wqn::TimerMode::kCountdown) {
        return "专注进行中";
    }
    switch (time_app.pomodoro_phase) {
        case wqn::PomodoroPhase::kBreak:
            return "休息中";
        case wqn::PomodoroPhase::kLongBreak:
            return "长休息中";
        case wqn::PomodoroPhase::kFocus:
        default:
            return "专注中";
    }
}

std::string TimerHeroValue(const wqn::TimeAppState& time_app)
{
    if (time_app.status == wqn::TimerStatus::kPaused) {
        if (time_app.remaining_seconds >= 60) {
            return std::to_string(std::max(1, (time_app.remaining_seconds + 59) / 60)) + " MIN";
        }
        return std::to_string(std::max(0, time_app.remaining_seconds)) + " SEC";
    }
    if (PomodoroGroupComplete(time_app)) {
        return std::to_string(time_app.pomodoro_rounds) + " × " +
               std::to_string(time_app.pomodoro_focus_minutes);
    }
    if (time_app.status == wqn::TimerStatus::kAlerting) {
        return FormatUnixClock(
            time_app.phase_ends_unix_seconds,
            TimerInitialSeconds(time_app) < 60);
    }
    return FormatUnixClock(
        time_app.phase_ends_unix_seconds,
        TimerInitialSeconds(time_app) < 60);
}

std::string TimerHeroCaption(const wqn::TimeAppState& time_app)
{
    if (time_app.status == wqn::TimerStatus::kPaused) {
        return time_app.active_mode == wqn::TimerMode::kCountdown ? "剩余" : "本轮剩余";
    }
    if (PomodoroGroupComplete(time_app)) {
        return "FOCUS COMPLETE";
    }
    if (time_app.status == wqn::TimerStatus::kAlerting) {
        return "完成时刻";
    }
    if (TimesAreOnDifferentDays(
            time_app.phase_started_unix_seconds,
            time_app.phase_ends_unix_seconds)) {
        return FormatUnixDate(time_app.phase_ends_unix_seconds) + "  次日结束";
    }
    if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        return time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus
            ? "本轮结束"
            : "休息结束";
    }
    return "结束";
}

std::string TimerActionLabel(const wqn::TimeAppState& time_app)
{
    if (time_app.status == wqn::TimerStatus::kRunning) {
        if (time_app.active_mode == wqn::TimerMode::kPomodoro &&
            time_app.pomodoro_phase == wqn::PomodoroPhase::kBreak) {
            return "跳过";
        }
        if (time_app.active_mode == wqn::TimerMode::kPomodoro &&
            time_app.pomodoro_phase == wqn::PomodoroPhase::kLongBreak) {
            return "结束";
        }
        return "暂停";
    }
    if (time_app.status == wqn::TimerStatus::kPaused) {
        return "继续";
    }
    if (time_app.active_mode == wqn::TimerMode::kCountdown) {
        return "返回";
    }
    if (PomodoroGroupComplete(time_app)) {
        return "完成";
    }
    if (time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus) {
        return time_app.pomodoro_current_round >= time_app.pomodoro_rounds
            ? "开始长休"
            : "开始休息";
    }
    return time_app.pomodoro_current_round >= time_app.pomodoro_rounds
        ? "完成"
        : "下一轮";
}

void DrawTimeRailNode(int center_x, int center_y, bool filled)
{
    constexpr int kDiameter = 12;
    const int x = center_x - kDiameter / 2;
    const int y = center_y - kDiameter / 2;
    if (filled) {
        FillRoundedRect(x, y, kDiameter, kDiameter, kDiameter / 2);
    } else {
        DrawRoundedRect(x, y, kDiameter, kDiameter, kDiameter / 2);
    }
}

void DrawPauseRailNode(int center_x, int center_y)
{
    constexpr int kDiameter = 14;
    const int x = center_x - kDiameter / 2;
    const int y = center_y - kDiameter / 2;
    DrawRoundedRect(x, y, kDiameter, kDiameter, kDiameter / 2);
    FillRect(center_x - 3, center_y - 4, 2, 8, true);
    FillRect(center_x + 1, center_y - 4, 2, 8, true);
}

void DrawBoundaryNode(int center_x, int center_y, bool filled)
{
    constexpr int kDiameter = 10;
    const int x = center_x - kDiameter / 2;
    const int y = center_y - kDiameter / 2;
    if (filled) {
        FillRoundedRect(x, y, kDiameter, kDiameter, kDiameter / 2);
    } else {
        DrawRoundedRect(x, y, kDiameter, kDiameter, kDiameter / 2);
    }
}

void DrawNowMark(int center_x, int center_y, bool paused)
{
    if (paused) {
        DrawPauseRailNode(center_x, center_y);
    } else {
        constexpr int kDiameter = 14;
        const int x = center_x - kDiameter / 2;
        const int y = center_y - kDiameter / 2;
        FillRoundedRect(x, y, kDiameter, kDiameter, kDiameter / 2);
    }
}

void DrawDashedRail(int x, int y, int width, int thickness)
{
    constexpr int kDash = 7;
    constexpr int kGap = 4;
    const int right = x + std::max(0, width);
    for (int dash_x = x; dash_x < right; dash_x += kDash + kGap) {
        const int dash_width = std::min(kDash, right - dash_x);
        for (int row = 0; row < thickness; ++row) {
            DrawHorizontalLine(dash_x, y + row, dash_width);
        }
    }
}

void DrawTimeRailSegment(int left, int right, int y, bool is_break, bool elapsed)
{
    const int width = std::max(0, right - left);
    if (width <= 0) {
        return;
    }
    if (is_break) {
        DrawDashedRail(left, y - (elapsed ? 1 : 0), width, elapsed ? 3 : 2);
    } else if (elapsed) {
        FillRect(left, y - 1, width, 4, true);
    } else {
        DrawHorizontalLine(left, y, width);
        DrawHorizontalLine(left, y + 1, width);
    }
}

esp_err_t DrawPhaseTimeline(const wqn::TimeAppState& time_app)
{
    constexpr int kRailLeft = 38;
    constexpr int kRailRight = 362;
    constexpr int kRailY = 165;
    constexpr int kRailWidth = kRailRight - kRailLeft;
    const bool done = time_app.status == wqn::TimerStatus::kAlerting;
    const bool final_group = PomodoroGroupComplete(time_app);
    const bool is_break = !final_group &&
        time_app.active_mode == wqn::TimerMode::kPomodoro &&
        time_app.pomodoro_phase != wqn::PomodoroPhase::kFocus;
    const int total = final_group
        ? PomodoroGroupTotalSeconds(time_app)
        : TimerInitialSeconds(time_app);
    const int progress_seconds = done
        ? total
        : wqn::TimeAppVisualProgressSeconds(time_app);
    const int marker_x = kRailLeft +
        std::min(kRailWidth, kRailWidth * progress_seconds / std::max(1, total));

    DrawTimeRailSegment(kRailLeft, marker_x, kRailY, is_break, true);
    DrawTimeRailSegment(marker_x, kRailRight, kRailY, is_break, false);
    DrawTimeRailNode(kRailLeft, kRailY, true);
    DrawTimeRailNode(kRailRight, kRailY, done);
    if (!done) {
        if (time_app.status == wqn::TimerStatus::kPaused) {
            DrawPauseRailNode(marker_x, kRailY);
        } else {
            DrawTimeRailNode(marker_x, kRailY, true);
        }
    }

    const int64_t left_time = final_group
        ? time_app.session_started_unix_seconds
        : time_app.phase_started_unix_seconds;
    const int64_t right_time = time_app.phase_ends_unix_seconds;
    const bool date_qualified = final_group &&
        TimesAreOnDifferentDays(left_time, right_time);
    const bool show_seconds = !final_group && total < 60;
    const std::string left_label = date_qualified
        ? FormatUnixDateClock(left_time)
        : FormatUnixClock(left_time, show_seconds);
    const std::string right_label = date_qualified
        ? FormatUnixDateClock(right_time)
        : FormatUnixClock(right_time, show_seconds);
    ESP_RETURN_ON_ERROR(
        DrawClippedText(20, 141, 145, left_label),
        kTag,
        "draw timer start time");
    const int right_label_width =
        wqn::MeasureUtf8TextWidth(right_label.c_str());
    ESP_RETURN_ON_ERROR(
        DrawClippedText(
            std::max(235, 380 - right_label_width),
            141,
            145,
            right_label),
        kTag,
        "draw timer endpoint time");
    ESP_RETURN_ON_ERROR(DrawClippedText(20, 177, 70, "开始"), kTag, "draw timer start label");
    if (!done) {
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(
                155,
                177,
                90,
                time_app.status == wqn::TimerStatus::kPaused ? "暂停于" : "现在"),
            kTag,
            "draw timer current label");
    }
    const std::string endpoint_meta = done ? "完成" : "结束";
    const int endpoint_meta_width =
        wqn::MeasureUtf8TextWidth(endpoint_meta.c_str());
    ESP_RETURN_ON_ERROR(
        DrawClippedText(
            std::max(310, 380 - endpoint_meta_width),
            177,
            70,
            endpoint_meta),
        kTag,
        "draw timer endpoint label");
    return ESP_OK;
}

struct PomodoroTrackSegment {
    int round = 1;
    int seconds = 1;
    bool is_break = false;
    bool complete = false;
    bool current = false;
};

struct PomodoroTrackWindow {
    std::array<PomodoroTrackSegment, 8> segments = {};
    size_t count = 0;
    int first_round = 1;
    int last_round = 1;
};

PomodoroTrackWindow BuildVisiblePomodoroSegments(
    const wqn::TimeAppState& time_app)
{
    constexpr int kVisibleRounds = 4;
    const int rounds = std::max(1, time_app.pomodoro_rounds);
    PomodoroTrackWindow window;
    wqn::TimeAppVisibleRoundWindow(
        time_app,
        kVisibleRounds,
        &window.first_round,
        &window.last_round);
    const int start = window.first_round - 1;
    const int end = window.last_round;
    for (int round_index = start; round_index < end; ++round_index) {
        const int round = round_index + 1;
        const bool before_current = round < time_app.pomodoro_current_round;
        const bool current_round = round == time_app.pomodoro_current_round;
        const bool focus_complete = before_current ||
            (current_round && time_app.pomodoro_phase != wqn::PomodoroPhase::kFocus) ||
            (current_round && time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus &&
             time_app.status == wqn::TimerStatus::kAlerting);
        const bool focus_current = current_round &&
            time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus &&
            time_app.status != wqn::TimerStatus::kAlerting;
        window.segments[window.count++] = PomodoroTrackSegment{
            round,
            std::max(1, time_app.pomodoro_focus_minutes * 60),
            false,
            focus_complete,
            focus_current};

        const bool final_round = round == rounds;
        const bool break_complete = before_current ||
            (current_round && time_app.pomodoro_phase != wqn::PomodoroPhase::kFocus &&
             time_app.status == wqn::TimerStatus::kAlerting);
        const bool break_current = current_round &&
            time_app.pomodoro_phase != wqn::PomodoroPhase::kFocus &&
            time_app.status != wqn::TimerStatus::kAlerting;
        window.segments[window.count++] = PomodoroTrackSegment{
            round,
            std::max(
                1,
                (final_round
                    ? time_app.pomodoro_long_break_minutes
                    : time_app.pomodoro_break_minutes) * 60),
            true,
            break_complete,
            break_current};
    }
    return window;
}

void DrawContinuationDots(int center_x, int y, bool elapsed)
{
    for (int offset = -5; offset <= 5; offset += 5) {
        if (elapsed) {
            FillRect(center_x + offset, y - 1, 3, 3, true);
        } else {
            DrawRect(center_x + offset, y - 1, 3, 3);
        }
    }
}

void DrawRoundProgress(const wqn::TimeAppState& time_app)
{
    const PomodoroTrackWindow window = BuildVisiblePomodoroSegments(time_app);
    if (window.count == 0) {
        return;
    }
    const bool left_continues = window.first_round > 1;
    const bool right_continues = window.last_round < time_app.pomodoro_rounds;
    const int left = left_continues ? 52 : 38;
    const int right = right_continues ? 348 : 362;
    const int width = right - left;
    constexpr int kY = 237;

    int64_t total_seconds = 0;
    for (size_t index = 0; index < window.count; ++index) {
        total_seconds += window.segments[index].seconds;
    }
    if (total_seconds <= 0) {
        return;
    }

    const bool done_all = PomodoroGroupComplete(time_app);

    // 1. Draw rail segments
    int segment_left = left;
    int64_t cumulative_seconds = 0;
    int current_marker_x = -1;
    const bool current_is_paused = time_app.status == wqn::TimerStatus::kPaused;

    for (size_t index = 0; index < window.count; ++index) {
        const PomodoroTrackSegment& segment = window.segments[index];
        cumulative_seconds += segment.seconds;
        int segment_right = index + 1 == window.count
            ? right
            : left + static_cast<int>(width * cumulative_seconds / total_seconds);
        segment_right = std::max(segment_left + 1, segment_right);
        segment_right = std::min(right, segment_right);

        if (done_all || segment.complete) {
            DrawTimeRailSegment(segment_left, segment_right, kY, segment.is_break, true);
        } else if (segment.current) {
            const int progress = wqn::TimeAppVisualProgressSeconds(time_app);
            current_marker_x = segment_left +
                (segment_right - segment_left) * progress / std::max(1, segment.seconds);
            current_marker_x = std::max(segment_left, std::min(segment_right, current_marker_x));
            DrawTimeRailSegment(segment_left, current_marker_x, kY, segment.is_break, true);
            DrawTimeRailSegment(current_marker_x, segment_right, kY, segment.is_break, false);
        } else {
            DrawTimeRailSegment(segment_left, segment_right, kY, segment.is_break, false);
        }
        segment_left = segment_right;
    }

    // 2. Draw boundary nodes
    int64_t finished_seconds = 0;
    if (done_all) {
        finished_seconds = total_seconds;
    } else {
        for (size_t index = 0; index < window.count; ++index) {
            const PomodoroTrackSegment& segment = window.segments[index];
            if (segment.complete) {
                finished_seconds += segment.seconds;
            } else if (segment.current) {
                finished_seconds += wqn::TimeAppVisualProgressSeconds(time_app);
                break;
            } else {
                break;
            }
        }
    }

    DrawBoundaryNode(left, kY, finished_seconds >= 0);

    cumulative_seconds = 0;
    for (size_t index = 0; index < window.count; ++index) {
        const PomodoroTrackSegment& segment = window.segments[index];
        cumulative_seconds += segment.seconds;
        int bx = index + 1 == window.count
            ? right
            : left + static_cast<int>(width * cumulative_seconds / total_seconds);
        bx = std::min(right, bx);
        DrawBoundaryNode(bx, kY, cumulative_seconds <= finished_seconds);
    }

    // 3. Draw active cursor nowmark
    if (!done_all && current_marker_x >= 0) {
        DrawNowMark(current_marker_x, kY, current_is_paused);
    }

    if (left_continues) {
        DrawContinuationDots(30, kY, window.first_round <= time_app.pomodoro_current_round);
    }
    if (right_continues) {
        DrawContinuationDots(365, kY, false);
    }
}

std::string PomodoroTrackLeftLabel(const wqn::TimeAppState& time_app)
{
    if (time_app.pomodoro_rounds <= 4) {
        return "整组时间轴";
    }
    constexpr int kVisibleRounds = 4;
    int first_round = 1;
    int last_round = 1;
    wqn::TimeAppVisibleRoundWindow(
        time_app,
        kVisibleRounds,
        &first_round,
        &last_round);
    return "轮次 " + std::to_string(first_round) + "-" +
           std::to_string(last_round) + " / " +
           std::to_string(time_app.pomodoro_rounds);
}

std::string PomodoroTrackRightLabel(const wqn::TimeAppState& time_app)
{
    if (PomodoroGroupComplete(time_app)) {
        return std::to_string(time_app.pomodoro_rounds) + " / " +
               std::to_string(time_app.pomodoro_rounds) + " 已完成";
    }
    if (time_app.status == wqn::TimerStatus::kPaused) {
        return "第 " + std::to_string(time_app.pomodoro_current_round) + " 轮 / 暂停";
    }
    if (time_app.pomodoro_phase == wqn::PomodoroPhase::kFocus) {
        return "第 " + std::to_string(time_app.pomodoro_current_round) + " 轮 / 共 " +
               std::to_string(time_app.pomodoro_rounds) + " 轮";
    }
    return time_app.pomodoro_phase == wqn::PomodoroPhase::kLongBreak
        ? "长休息 " + std::to_string(time_app.pomodoro_long_break_minutes) + " min"
        : "短休息 " + std::to_string(time_app.pomodoro_break_minutes) + " min";
}

std::string TimerFooterMeta(const wqn::TimeAppState& time_app)
{
    if (time_app.active_mode == wqn::TimerMode::kCountdown) {
        if (time_app.status == wqn::TimerStatus::kPaused) {
            return "时间轴冻结";
        }
        if (time_app.status == wqn::TimerStatus::kAlerting) {
            return "记录已完成";
        }
        const int mins = std::max(1, (TimerInitialSeconds(time_app) + 59) / 60);
        return "本次 " + std::to_string(mins) + " min";
    }
    if (PomodoroGroupComplete(time_app)) {
        const int total_sec = PomodoroGroupTotalSeconds(time_app);
        const int hours = total_sec / 3600;
        const int mins = (total_sec % 3600) / 60;
        if (hours > 0) {
            return std::to_string(hours) + " h " + std::to_string(mins) + " min";
        }
        return std::to_string(mins) + " min";
    }
    if (time_app.status == wqn::TimerStatus::kPaused) {
        return "第 " + std::to_string(time_app.pomodoro_current_round) + " 轮被冻结";
    }
    if (time_app.pomodoro_phase == wqn::PomodoroPhase::kBreak) {
        return "下一轮：第 " + std::to_string(time_app.pomodoro_current_round + 1) + " 轮";
    }
    if (time_app.pomodoro_phase == wqn::PomodoroPhase::kLongBreak) {
        return std::to_string(time_app.pomodoro_rounds) + " 轮专注已完成";
    }
    const int focus_min = std::max(1, time_app.pomodoro_focus_minutes);
    return "本轮 " + std::to_string(focus_min) + " min";
}

constexpr int kActionX = 310;
constexpr int kActionY = 267;
constexpr int kActionW = 70;
constexpr int kActionH = 25;

esp_err_t DrawTimerAction(const wqn::TimeAppState& time_app)
{
    const std::string action = TimerActionLabel(time_app);
    if (time_app.action_armed) {
        DrawSelectionDecoration(kActionX, kActionY, kActionW, kActionH, SelectionStyle::kInvert);
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(kActionX, kActionY + 5, kActionW, action, false),
            kTag,
            "draw armed timer action");
    } else {
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(kActionX, kActionY + 5, kActionW, action),
            kTag,
            "draw passive timer action");
        const int underline_w = std::min(kActionW - 12, std::max(22, wqn::MeasureUtf8TextWidth(action.c_str())));
        DrawHorizontalLine(kActionX + (kActionW - underline_w) / 2, kActionY + kActionH - 1, underline_w);
    }
    return ESP_OK;
}

esp_err_t DrawTimerFooter(const wqn::TimeAppState& time_app)
{
    ESP_RETURN_ON_ERROR(
        DrawClippedText(20, 273, 260, TimerFooterMeta(time_app)),
        kTag,
        "draw timer footer meta");
    return DrawTimerAction(time_app);
}

esp_err_t DrawCountdownDetails(const wqn::TimeAppState& time_app)
{
    DrawHorizontalLine(20, 208, 170);
    DrawHorizontalLine(210, 208, 170);

    std::string d1_k;
    std::string d1_v;
    if (time_app.status == wqn::TimerStatus::kRunning) {
        d1_k = "剩余";
        if (time_app.countdown_total_seconds < 60) {
            d1_v = std::to_string(std::max(0, time_app.remaining_seconds)) + " 秒";
        } else {
            const int remaining_min = (time_app.remaining_seconds + 59) / 60;
            d1_v = "约 " + std::to_string(std::max(1, remaining_min)) + " 分";
        }
    } else if (time_app.status == wqn::TimerStatus::kPaused) {
        d1_k = "已进行";
        if (time_app.countdown_total_seconds < 60) {
            const int elapsed_sec = time_app.countdown_total_seconds - time_app.remaining_seconds;
            d1_v = std::to_string(std::max(0, elapsed_sec)) + " 秒";
        } else {
            const int elapsed_min = (time_app.countdown_total_seconds - time_app.remaining_seconds) / 60;
            d1_v = std::to_string(std::max(0, elapsed_min)) + " 分";
        }
    } else {
        d1_k = "总时长";
        if (time_app.countdown_total_seconds < 60) {
            d1_v = std::to_string(std::max(1, time_app.countdown_total_seconds)) + " 秒";
        } else {
            d1_v = std::to_string(std::max(1, time_app.countdown_total_seconds / 60)) + " 分";
        }
    }

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(20, 215, d1_k.c_str(), true), kTag, "draw detail1 label");
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(20, 235, d1_v.c_str(), true), kTag, "draw detail1 value");

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(210, 215, "任务", true), kTag, "draw task label");
    const char* task = (time_app.task_name[0] != '\0') ? time_app.task_name : "自由专注";
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(210, 235, task, true), kTag, "draw task value");
    return ESP_OK;
}

esp_err_t RenderTimerRunToEpd(const wqn::TimeAppState& time_app)
{
    DrawStatusChip(306, 37, 76, 22, kChipRadius, TimerStatusChipLabel(time_app));
    ESP_RETURN_ON_ERROR(
        DrawCenteredText(80, 43, 240, TimerHeroStateLabel(time_app)),
        kTag,
        "draw timer hero state");
    DrawTimerDigitsArt(68, TimerHeroValue(time_app));
    ESP_RETURN_ON_ERROR(
        DrawCenteredText(0, 122, wqn::kEpdWidth, TimerHeroCaption(time_app)),
        kTag,
        "draw timer hero caption");
    ESP_RETURN_ON_ERROR(DrawPhaseTimeline(time_app), kTag, "draw timer phase timeline");

    if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        const std::string track_left = PomodoroTrackLeftLabel(time_app);
        const std::string track_right = PomodoroTrackRightLabel(time_app);
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(20, 207, track_left.c_str(), true), kTag, "draw pomodoro group label");
        const int rounds_w = wqn::MeasureUtf8TextWidth(track_right.c_str());
        ESP_RETURN_ON_ERROR(
            wqn::DrawUtf8Text(std::max(20, 380 - rounds_w), 207, track_right.c_str(), true),
            kTag,
            "draw pomodoro round count");
        DrawRoundProgress(time_app);
    } else {
        ESP_RETURN_ON_ERROR(DrawCountdownDetails(time_app), kTag, "draw countdown details");
    }
    ESP_RETURN_ON_ERROR(DrawTimerFooter(time_app), kTag, "draw timer footer");
    return ESP_OK;
}

esp_err_t RenderTimerRunRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule)
{
    ClearRect(kTimerRunRect);
    ESP_RETURN_ON_ERROR(RenderTimerRunToEpd(time_app), kTag, "draw timer run region");
    return RefreshRegion(kTimerRunRect, schedule);
}

esp_err_t RenderTimerActionRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule)
{
    constexpr UiRect kTimerActionRect = {304, 265, 80, 30, "timer-action"};
    ClearRect(kTimerActionRect);
    ESP_RETURN_ON_ERROR(DrawTimerAction(time_app), kTag, "draw timer action region");
    // Selection flips use the stable full-frame partial waveform. The panel's
    // local window path has a documented BUSY-wedge history for selection
    // changes, even when the dirty pixels are this small.
    return RefreshStableRegion(kTimerActionRect, schedule);
}

esp_err_t RenderTimeConfigRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule)
{
    const UiRect rect = ConfigRefreshRect(time_app);
    ClearRect(rect);
    if (time_app.tile == wqn::TimeTile::kCountdown) {
        ESP_RETURN_ON_ERROR(RenderCountdownConfigToEpd(time_app), kTag, "draw countdown config region");
    } else {
        ESP_RETURN_ON_ERROR(RenderPomodoroConfigToEpd(time_app), kTag, "draw pomodoro config region");
    }
    return RefreshRegion(rect, schedule);
}

esp_err_t RenderTimeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::TimeAppState& time_app = frame.time_app;
    wqn::ClearEpdFramebuffer(true);

    if (time_app.tile == wqn::TimeTile::kClock) {
        DrawClockStatusBar(frame.home);
        ESP_RETURN_ON_ERROR(RenderClockStandbyContent(), kTag, "draw clock standby");
    } else {
        const bool active_timer = !time_app.config_mode && wqn::TimeAppHasActiveTimer(time_app);
        if (active_timer) {
            DrawTimerStatusBar(TimeTileTitle(time_app.tile), frame.home);
        } else {
            DrawStatusBar(TimeTileTitle(time_app.tile), frame.home);
        }
        if (time_app.config_mode && time_app.tile == wqn::TimeTile::kCountdown) {
            ESP_RETURN_ON_ERROR(RenderCountdownConfigToEpd(time_app), kTag, "draw countdown config");
        } else if (time_app.config_mode && time_app.tile == wqn::TimeTile::kPomodoro) {
            ESP_RETURN_ON_ERROR(RenderPomodoroConfigToEpd(time_app), kTag, "draw pomodoro config");
        } else {
            ESP_RETURN_ON_ERROR(RenderTimerRunToEpd(time_app), kTag, "draw running timer");
        }
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
