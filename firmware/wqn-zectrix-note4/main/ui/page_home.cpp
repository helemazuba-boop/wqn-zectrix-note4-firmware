// Home page rendering: metric cards, task list, primary time region.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

#include "epd_display.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr int kWrappedBodyMaxLines = 4;

esp_err_t DrawMetricCard(int x, int y, int width, const wqn::HomeMetric& metric)
{
    constexpr int kCardHeight = 55;
    DrawRect(x, y, width, kCardHeight);
    ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 8, width, metric.value), kTag, "draw home metric value");
    ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 31, width, metric.label), kTag, "draw home metric label");
    return ESP_OK;
}

esp_err_t DrawHomeTaskRow(int x, int y, int width, int index, const wqn::HomeTask& task, bool selected)
{
    constexpr int kRowHeight = 47;
    if (selected) {
        FillRect(x - 4, y - 2, width + 8, kRowHeight, true);
    }
    const bool black_text = !selected;
    const int text_width = width - 58;
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(x, y + 4, std::to_string(index).c_str(), black_text),
        kTag,
        "draw home task index");
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 20, y + 3, text_width, task.title, black_text), kTag, "draw home task title");
    ESP_RETURN_ON_ERROR(
        DrawClippedText(x + 20, y + 24, text_width, task.subtitle, black_text),
        kTag,
        "draw home task subtitle");
    if (!task.tag.empty()) {
        const int tag_width = std::min(48, std::max(32, wqn::MeasureUtf8TextWidth(task.tag.c_str()) + 8));
        DrawRect(x + width - tag_width, y + 4, tag_width, 18);
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(x + width - tag_width, y + 5, tag_width, task.tag, black_text),
            kTag,
            "draw home task tag");
    }
    return ESP_OK;
}

esp_err_t RenderHomePrimaryRegion(const wqn::HomeSummary& home, RefreshSchedule schedule)
{
    ClearRect(kHomePrimaryRect);
    DrawRect(10, 35, 380, 26);
    ESP_RETURN_ON_ERROR(DrawCenteredText(10, 39, 380, home.primary_time_line), kTag, "draw home primary time region");
    return RefreshRegion(kHomePrimaryRect, schedule);
}

UiRect ConfigRefreshRect(const wqn::TimeAppState& time_app)
{
    return time_app.tile == wqn::TimeTile::kPomodoro ? kPomodoroConfigRect : kCountdownConfigRect;
}

esp_err_t RenderHomeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    ESP_LOGI(kTag, "RenderHomeToEpd: enter schedule=%s", RefreshScheduleName(schedule));
    const wqn::HomeSummary& home = frame.home;
    wqn::ClearEpdFramebuffer(true);

    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 6, "首页", true), kTag, "draw home title");
    std::string status = home.wifi_label;
    if (!home.battery_label.empty()) {
        status += "  " + home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true),
        kTag,
        "draw home status");

    DrawRect(10, 35, 380, 26);
    ESP_RETURN_ON_ERROR(DrawCenteredText(10, 39, 380, home.primary_time_line), kTag, "draw home primary time");

    constexpr int kCardY = 69;
    constexpr int kCardWidth = 121;
    ESP_RETURN_ON_ERROR(DrawMetricCard(10, kCardY, kCardWidth, home.review_metric), kTag, "draw review metric");
    ESP_RETURN_ON_ERROR(DrawMetricCard(139, kCardY, kCardWidth, home.todo_metric), kTag, "draw todo metric");
    ESP_RETURN_ON_ERROR(DrawMetricCard(269, kCardY, kCardWidth, home.word_metric), kTag, "draw word metric");

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 138, "当前进行", true), kTag, "draw home section title");
    const std::string subtitle = wqn::TruncateUtf8TextToWidth(home.current_status, 210);
    const int subtitle_width = wqn::MeasureUtf8TextWidth(subtitle.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - subtitle_width - 10), 138, subtitle.c_str(), true),
        kTag,
        "draw home section status");

    DrawHorizontalLine(10, 161, 380);
    int y = 168;
    const size_t visible_tasks = std::min<size_t>(home.tasks.size(), 2);
    if (visible_tasks == 0) {
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, y, "暂无当前任务", true), kTag, "draw home empty");
    }
    for (size_t i = 0; i < visible_tasks; ++i) {
        ESP_RETURN_ON_ERROR(
            DrawHomeTaskRow(14, y, 372, static_cast<int>(i + 1), home.tasks[i], i == frame.selected_home_task),
            kTag,
            "draw home task");
        y += 53;
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
