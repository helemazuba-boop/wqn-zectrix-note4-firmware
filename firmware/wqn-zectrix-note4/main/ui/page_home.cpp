// Home reference A: clock, Agent task, then a concrete study continuation.
#include "ui_internal.h"
#include "display_service.h"

#include <algorithm>

namespace device_ui_internal {
namespace {

void HomeText(int x, int y, int width, const std::string& text, int size = 16,
              bool bold = false, bool black = true)
{
    // Clip in the same font used for rendering; the body fallback's 5x7 Latin
    // metrics are deliberately not used for these mixed-language headings.
    std::string clipped = text;
    const auto fits = [width, size, bold](const std::string& value) {
        return wqn::MeasureTextWithFont(wqn::PrimaryUiFont(), value.c_str()) * size / 16 +
            (bold ? 1 : 0) <= width;
    };
    if (!fits(clipped)) {
        while (!clipped.empty() && !fits(clipped + "...")) {
            size_t end = clipped.size() - 1;
            while (end != 0 && (static_cast<unsigned char>(clipped[end]) & 0xc0) == 0x80) --end;
            clipped.resize(end);
        }
        clipped += "...";
    }
    wqn::DrawTextWithFontSized(x, y, width, wqn::PrimaryUiFont(), clipped.c_str(), size, bold, black);
}

void DrawHomeClock(const wqn::HomeSummary& home)
{
    const std::string clock = CurrentClockLabel();
    if (clock.find('-') == std::string::npos) DrawDigit48Run(kHomeLeft, kHomeClockY, clock);
    else HomeText(kHomeLeft, kHomeClockY, 230, clock, 48);
    const std::string date = CurrentDateLabel();
    const size_t split = date.find(' ');
    HomeText(kHomeDateX, 58, 104, date.substr(0, split), 14);
    if (split != std::string::npos) HomeText(kHomeDateX, 78, 104, date.substr(split + 1), 14);
    if (home.timer_status_only) HomeText(kHomeDateX, 96, 104, home.primary_time_line, 12);
}

void DrawHomeHeader(const wqn::HomeSummary& home)
{
    wqn::DrawUtf8Text(kHomeLeft, 6, "WQN / NOTE 04", true);
    DrawStatusBarIcons(384, 7, home);
    DrawHorizontalLine(kHomeLeft, 31, kHomeWidth);
}

}  // namespace

esp_err_t RenderHomePrimaryRegion(const wqn::HomeSummary& home, RefreshSchedule schedule)
{
    ClearRect(kHomePrimaryRect);
    DrawHomeClock(home);
    DrawHorizontalLine(kHomeLeft, kHomeAiDividerY, kHomeWidth);
    return RefreshRegion(kHomePrimaryRect, schedule);
}

esp_err_t RenderHomeStatusBarRegion(const wqn::HomeSummary& home, RefreshSchedule schedule)
{
    constexpr UiRect region = {0, 0, wqn::kEpdWidth, 114, "home-header-clock"};
    ClearRect(region);
    DrawHomeHeader(home);
    DrawHomeClock(home);
    DrawHorizontalLine(kHomeLeft, kHomeAiDividerY, kHomeWidth);
    return RefreshRegion(region, schedule);
}

UiRect ConfigRefreshRect(const wqn::TimeAppState& time_app)
{
    return time_app.tile == wqn::TimeTile::kPomodoro ? kPomodoroConfigRect : kCountdownConfigRect;
}

esp_err_t RenderHomeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const auto& home = frame.home;
    wqn::ClearEpdFramebuffer(true);
    DrawHomeHeader(home);
    DrawHomeClock(home);
    DrawHorizontalLine(kHomeLeft, kHomeAiDividerY, kHomeWidth);
    if (frame.selected_home_task == 0) FillRect(7, 124, 3, 90, true);
    else FillRect(7, 232, 3, 42, true);

    wqn::DrawUtf8Text(kHomeLeft, kHomeAiCaptionY,
                     home.ai.session_id.empty() ? "01 / AI" : "01 / AI AGENT", true);
    const int badge_width = std::min(120, wqn::MeasureTextWithFont(
        wqn::PrimaryUiFont(), home.ai.badge.c_str()) + 12);
    FillRect(384 - badge_width, 120, badge_width, 24, true);
    HomeText(390 - badge_width, 124, badge_width - 12, home.ai.badge, 16, false, false);
    HomeText(kHomeLeft, kHomeAiTitleY, kHomeWidth, home.ai.title, 24, true);
    HomeText(kHomeLeft, kHomeAiActionY, kHomeWidth - 28, home.ai.action, 14);
    for (int i = 0; i < 5; ++i) {
        wqn::DrawEpdPixel(373 + i, 195 + i, true);
        wqn::DrawEpdPixel(373 + i, 203 - i, true);
    }
    DrawHorizontalLine(kHomeLeft, kHomeStudyDividerY, kHomeWidth);
    HomeText(kHomeLeft, kHomeStudyCaptionY, 300,
             home.study.resumable ? "02 / 继续学习" : "02 / 学习内容", 12);
    HomeText(kHomeLeft, kHomeStudyTitleY, kHomeWidth - 86, home.study.title, 20);
    HomeText(310, 252, 74, home.study.progress, 12);
    if (home.study.progress_percent >= 0) {
        FillRect(kHomeLeft, 274, kHomeWidth * std::clamp(home.study.progress_percent, 0, 100) / 100, 2, true);
    }
    HomeText(kHomeLeft, kHomeFooterY, 230,
             home.notice.empty() ? home.review_label : home.notice, 12);
    const int todo_width = wqn::MeasureTextWithFont(wqn::PrimaryUiFont(), home.todo_label.c_str()) * 12 / 16;
    HomeText(384 - todo_width, kHomeFooterY, todo_width + 1, home.todo_label, 12);
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
