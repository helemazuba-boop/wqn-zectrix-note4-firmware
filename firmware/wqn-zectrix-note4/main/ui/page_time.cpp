// Time page rendering: standby clock, countdown config, pomodoro config, timer run.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

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

void DrawProgressBar(int x, int y, int width, int height, int current, int total)
{
    DrawRect(x, y, width, height);
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
    if (selected) {
        FillRect(x, y, width, height, true);
    } else {
        DrawRect(x, y, width, height);
    }
    const bool black_text = !selected;
    // Value rendered from the shared 1bpp 16px digit assets; label stays CJK.
    DrawConfigDigitsCentered(x, y + 7, width, value, black_text);
    DrawCenteredText(x, y + height - 20, width, label, black_text);
}

void DrawActionBox(int x, int y, int width, const std::string& label, bool selected)
{
    if (selected) {
        FillRect(x, y, width, 28, true);
    } else {
        DrawRect(x, y, width, 28);
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
    if (time_app.active_mode == wqn::TimerMode::kCountdown) {
        return std::max(1, time_app.countdown_total_seconds);
    }
    if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        switch (time_app.pomodoro_phase) {
            case wqn::PomodoroPhase::kBreak:
                return std::max(1, time_app.pomodoro_break_minutes * 60);
            case wqn::PomodoroPhase::kLongBreak:
                return std::max(1, time_app.pomodoro_long_break_minutes * 60);
            case wqn::PomodoroPhase::kFocus:
            default:
                return std::max(1, time_app.pomodoro_focus_minutes * 60);
        }
    }
    return 1;
}

esp_err_t RenderTimerRunToEpd(const wqn::TimeAppState& time_app)
{
    const std::string label =
        time_app.active_mode == wqn::TimerMode::kPomodoro ? wqn::PomodoroPhaseLabel(time_app.pomodoro_phase) : "倒计时";
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 68, wqn::kEpdWidth, label), kTag, "draw timer label");
    DrawTimerDigitsArt(110, wqn::FormatTimerDuration(time_app.remaining_seconds));
    if (time_app.status == wqn::TimerStatus::kPaused) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 185, wqn::kEpdWidth, "已暂停"), kTag, "draw paused label");
    } else if (time_app.status == wqn::TimerStatus::kAlerting) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 185, wqn::kEpdWidth, "时间到了"), kTag, "draw alert label");
    } else if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(
                0,
                185,
                wqn::kEpdWidth,
                "第 " + std::to_string(time_app.pomodoro_current_round) + "/" +
                    std::to_string(time_app.pomodoro_rounds) + " 轮"),
            kTag,
            "draw pomodoro round");
    }
    const int total = TimerInitialSeconds(time_app);
    DrawProgressBar(115, 220, 170, 12, total - time_app.remaining_seconds, total);
    return ESP_OK;
}

esp_err_t RenderTimerRunRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule)
{
    ClearRect(kTimerRunRect);
    ESP_RETURN_ON_ERROR(RenderTimerRunToEpd(time_app), kTag, "draw timer run region");
    return RefreshRegion(kTimerRunRect, schedule);
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
        DrawStatusBar(TimeTileTitle(time_app.tile), frame.home);
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
