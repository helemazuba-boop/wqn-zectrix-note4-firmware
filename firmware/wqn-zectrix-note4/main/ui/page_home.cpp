// Home page rendering: metric cards, task list, primary time region.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>

#include "display_service.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

// [v2] Home content horizontal bounds aligned to the AI page's flush 6px edges
// (kMarginDense): x=6, width 388 (6..394). Vertical layout is unchanged.
constexpr int kHomeContentX = kMarginDense;                       // 6
constexpr int kHomeContentW = wqn::kEpdWidth - 2 * kMarginDense;  // 388

esp_err_t DrawMetricCard(int x, int y, int width, const wqn::HomeMetric& metric)
{
    constexpr int kCardHeight = 55;
    // Metric card uses the rounded container language (same r6 as todo/word cards).
    DrawRoundedRect(x, y, width, kCardHeight, kRoundedOuterRadius);
    ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 8, width, metric.value), kTag, "draw home metric value");
    ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 31, width, metric.label), kTag, "draw home metric label");
    return ESP_OK;
}

esp_err_t DrawHomeTaskRow(int x, int y, int width, int index, const wqn::HomeTask& task, bool selected)
{
    constexpr int kRowHeight = 47;
    // Index gutter: the task number is also the selection marker (same language
    // as settings rows). Selected -> number sits on a reverse-filled rounded
    // block; unselected -> plain number, no block.
    constexpr int kIndexBlockW = 18;
    constexpr int kIndexBlockH = 24;
    constexpr int kIndexBlockYOff = (kRowHeight - kIndexBlockH) / 2;  // 11, vertically centered
    if (selected) {
        // [v2] Task row is a card-style persistent-browse row: rounded double
        // border (the square kInnerBorder is retired). The index block stays a
        // reverse-fill chip (now rounded via kInvert).
        DrawSelectionDecoration(x, y, width, kRowHeight, SelectionStyle::kRoundedInnerBorder);
        // Index block: reverse-fill rounded chip behind the task number.
        DrawSelectionDecoration(x, y + kIndexBlockYOff, kIndexBlockW, kIndexBlockH, SelectionStyle::kInvert);
        // Index number in paper (white) on the block, centered in it.
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(x, y + kIndexBlockYOff + 4, kIndexBlockW, std::to_string(index), false),
            kTag,
            "draw home task index");
    } else {
        // Unselected: plain ink number in its original position (left-aligned).
        ESP_RETURN_ON_ERROR(
            wqn::DrawUtf8Text(x, y + 4, std::to_string(index).c_str(), true),
            kTag,
            "draw home task index");
    }
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 20, y + 3, width - 58, task.title, true), kTag, "draw home task title");
    ESP_RETURN_ON_ERROR(
        DrawClippedText(x + 20, y + 24, width - 58, task.subtitle, true),
        kTag,
        "draw home task subtitle");
    if (!task.tag.empty()) {
        const int tag_width = std::min(48, std::max(32, wqn::MeasureUtf8TextWidth(task.tag.c_str()) + 8));
        DrawChip(x + width - tag_width, y + 4, tag_width, 18, task.tag);
    }
    return ESP_OK;
}

esp_err_t RenderHomePrimaryRegion(const wqn::HomeSummary& home, RefreshSchedule schedule)
{
    ClearRect(kHomePrimaryRect);
    DrawRoundedRect(kHomeContentX, 35, kHomeContentW, 26, kRoundedOuterRadius);
    ESP_RETURN_ON_ERROR(DrawCenteredText(kHomeContentX, 39, kHomeContentW, home.primary_time_line), kTag, "draw home primary time region");
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

    DrawStatusBar("首页", home);

    DrawRoundedRect(kHomeContentX, 35, kHomeContentW, 26, kRoundedOuterRadius);
    ESP_RETURN_ON_ERROR(DrawCenteredText(kHomeContentX, 39, kHomeContentW, home.primary_time_line), kTag, "draw home primary time");

    // Three metric cards distributed across the 388px content width with a
    // kGutterCard gap: width 121 each, x = 6 / 139 / 272.
    constexpr int kCardY = 69;
    constexpr int kCardWidth = 121;
    constexpr int kCardStep = kCardWidth + kGutterCard;  // 133
    ESP_RETURN_ON_ERROR(DrawMetricCard(kHomeContentX, kCardY, kCardWidth, home.review_metric), kTag, "draw review metric");
    ESP_RETURN_ON_ERROR(DrawMetricCard(kHomeContentX + kCardStep, kCardY, kCardWidth, home.todo_metric), kTag, "draw todo metric");
    ESP_RETURN_ON_ERROR(DrawMetricCard(kHomeContentX + 2 * kCardStep, kCardY, kCardWidth, home.word_metric), kTag, "draw word metric");

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(kHomeContentX, 138, "当前进行", true), kTag, "draw home section title");
    const std::string subtitle = wqn::TruncateUtf8TextToWidth(home.current_status, 210);
    const int subtitle_width = wqn::MeasureUtf8TextWidth(subtitle.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(kHomeContentX, wqn::kEpdWidth - subtitle_width - kHomeContentX), 138, subtitle.c_str(), true),
        kTag,
        "draw home section status");

    DrawHorizontalLine(kHomeContentX, 161, kHomeContentW);
    int y = 168;
    const size_t visible_tasks = std::min<size_t>(home.tasks.size(), 2);
    if (visible_tasks == 0) {
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(kHomeContentX, y, "暂无当前任务", true), kTag, "draw home empty");
    }
    for (size_t i = 0; i < visible_tasks; ++i) {
        ESP_RETURN_ON_ERROR(
            DrawHomeTaskRow(kHomeContentX + 4, y, kHomeContentW - 8, static_cast<int>(i + 1), home.tasks[i], i == frame.selected_home_task),
            kTag,
            "draw home task");
        y += 53;
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
