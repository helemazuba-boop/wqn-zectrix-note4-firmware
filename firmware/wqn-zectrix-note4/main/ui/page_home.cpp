// Home reference A: clock, Agent task, then a concrete study continuation.
#include "ui_internal.h"
#include "display_service.h"
#include "ui_fonts.h"

#include <algorithm>

namespace device_ui_internal {
namespace {

const lv_font_t* HomeFont(int size)
{
    if (size == 12) return &wqn_ui_sans_12;
    if (size == 20) return &wqn_ui_sans_20;
    if (size == 24) return &wqn_ui_sans_24;
    if (size == 48) return &wqn_ui_latin_48;
    return wqn::PrimaryUiFont();
}

void HomeText(int x, int y, int width, const std::string& text, int size = 16,
              bool black = true)
{
    // y is always the font line-box top, including the native large fonts.
    const lv_font_t* font = HomeFont(size);
    std::string clipped = text;
    // Session titles and notices occupy one row; the native renderer supports
    // newlines, so do not let cloud text turn them into unbounded paragraphs.
    std::replace_if(clipped.begin(), clipped.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }, ' ');
    const auto fits = [width, font](const std::string& value) {
        return wqn::MeasureTextWithFont(font, value.c_str()) <= width;
    };
    if (!fits(clipped)) {
        while (!clipped.empty() && !fits(clipped + "...")) {
            size_t end = clipped.size() - 1;
            while (end != 0 && (static_cast<unsigned char>(clipped[end]) & 0xc0) == 0x80) --end;
            clipped.resize(end);
        }
        clipped += "...";
    }
    if (fits(clipped)) wqn::DrawTextWithFont(x, y, font, clipped.c_str(), black);
}

void DrawHomeClock(const wqn::HomeSummary& home)
{
    const std::string clock = CurrentClockLabel();
    if (clock.find('-') == std::string::npos) DrawDigit48Run(kHomeLeft, kHomeClockY, clock);
    else HomeText(kHomeLeft, kHomeClockY, 230, clock, 48);
    const std::string date = CurrentDateLabel();
    const size_t split = date.find(' ');
    HomeText(kHomeDateX, 46, 104, date.substr(0, split));
    if (split != std::string::npos) HomeText(kHomeDateX, 66, 104, date.substr(split + 1));
    if (home.timer_status_only) HomeText(kHomeDateX, 86, 104, home.primary_time_line, 12);
}

void DrawHomeHeader(const wqn::HomeSummary& home)
{
    wqn::DrawUtf8Text(kStatusBarTitleX, kStatusBarTitleY, "WQN / NOTE 04", true);
    DrawStatusBarIcons(kScreenWidth - kStatusBarRightInset, kStatusBarIconY, home);
    DrawHorizontalLine(0, kStatusBarDividerY, kScreenWidth);
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
    constexpr UiRect region = {0, 0, wqn::kEpdWidth, kHomeAiDividerY + 2, "home-header-clock"};
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
    if (frame.selected_home_task == 0) FillRect(7, 114, 3, 90, true);
    else FillRect(7, 222, 3, 42, true);

    HomeText(kHomeLeft, kHomeAiCaptionY, 240,
             home.ai.session_id.empty() ? "01 / AI" : "01 / AI AGENT", 12);
    const int badge_width = std::min(120, wqn::MeasureTextWithFont(
        wqn::PrimaryUiFont(), home.ai.badge.c_str()) + 12);
    FillRect(384 - badge_width, 110, badge_width, 24, true);
    HomeText(390 - badge_width, 114, badge_width - 12, home.ai.badge, 16, false);
    HomeText(kHomeLeft, kHomeAiTitleY, kHomeWidth, home.ai.title, 24);
    HomeText(kHomeLeft, kHomeAiActionY, kHomeWidth - 28, home.ai.action);
    for (int i = 0; i < 5; ++i) {
        wqn::DrawEpdPixel(373 + i, 185 + i, true);
        wqn::DrawEpdPixel(373 + i, 193 - i, true);
    }
    DrawHorizontalLine(kHomeLeft, kHomeStudyDividerY, kHomeWidth);
    HomeText(kHomeLeft, kHomeStudyCaptionY, 300,
             home.study.resumable ? "02 / 继续学习" : "02 / 学习内容", 12);
    HomeText(kHomeLeft, kHomeStudyTitleY, kHomeWidth - 86, home.study.title, 20);
    HomeText(310, 242, 74, home.study.progress, 12);
    if (home.study.progress_percent >= 0) {
        FillRect(kHomeLeft, 264, kHomeWidth * std::clamp(home.study.progress_percent, 0, 100) / 100, 2, true);
    }
    HomeText(kHomeLeft, kHomeFooterY, 230,
             home.notice.empty() ? home.review_label : home.notice, 12);
    const int todo_width = wqn::MeasureTextWithFont(HomeFont(12), home.todo_label.c_str());
    HomeText(384 - todo_width, kHomeFooterY, todo_width + 1, home.todo_label, 12);
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
