// Low-level drawing primitives + UiRect + refresh region helpers.
// Extracted from device_ui.cpp to make the rendering layer inspectable in isolation.

#include "ui_internal.h"

#include <algorithm>

#include "epd_display.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

void DrawHorizontalLine(int x, int y, int width)
{
    for (int xx = 0; xx < width; ++xx) {
        wqn::DrawEpdPixel(x + xx, y, true);
    }
}

void DrawVerticalLine(int x, int y, int height)
{
    for (int yy = 0; yy < height; ++yy) {
        wqn::DrawEpdPixel(x, y + yy, true);
    }
}

void DrawRect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    DrawHorizontalLine(x, y, width);
    DrawHorizontalLine(x, y + height - 1, width);
    DrawVerticalLine(x, y, height);
    DrawVerticalLine(x + width - 1, y, height);
}

void FillRect(int x, int y, int width, int height, bool black)
{
    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            wqn::DrawEpdPixel(x + xx, y + yy, black);
        }
    }
}

void ClearRect(const UiRect& rect)
{
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(0, rect.y);
    const int x1 = std::min(wqn::kEpdWidth, rect.x + rect.width);
    const int y1 = std::min(wqn::kEpdHeight, rect.y + rect.height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    FillRect(x0, y0, x1 - x0, y1 - y0, false);
}

void DrawSegment(int x, int y, int width, int height)
{
    FillRect(x, y, width, height, true);
}

esp_err_t RefreshRegion(const UiRect& rect, RefreshSchedule schedule)
{
    ESP_LOGI(
        kTag,
        "EPD UI region refresh: schedule=%s region=%s rect=x%d y%d w%d h%d",
        RefreshScheduleName(schedule),
        rect.name,
        rect.x,
        rect.y,
        rect.width,
        rect.height);
    return wqn::RefreshEpdFull(true, false);
}

esp_err_t RefreshStableRegion(const UiRect& rect, RefreshSchedule schedule)
{
    ESP_LOGI(
        kTag,
        "EPD UI stable region refresh: schedule=%s region=%s rect=x%d y%d w%d h%d",
        RefreshScheduleName(schedule),
        rect.name,
        rect.x,
        rect.y,
        rect.width,
        rect.height);
    return wqn::RefreshEpdFull(false, false);
}

esp_err_t RefreshFrame(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    ESP_LOGI(kTag, "RefreshFrame: enter schedule=%s screen=%d last_rendered_screen=%d",
             RefreshScheduleName(schedule), static_cast<int>(frame.screen),
             static_cast<int>(g_last_rendered_screen));
    const bool allow_local_partial =
        schedule == RefreshSchedule::kClock || schedule == RefreshSchedule::kTimer ||
        schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig;
    // [power-fix] Force a full refresh when:
    //   (a) the panel is being shown a genuinely different screen, OR
    //   (b) the producer asked for kCommit (used for screen transitions,
    //       time-app layout changes like Clock<->CountdownConfig, todo /
    //       word cloud-result updates, factory-reset, review-queue submit).
    //       These cases redraw the entire framebuffer, so a partial pipeline
    //       with the dirty-rect search still ends up sending the whole
    //       framebuffer and accumulating ghosting. The full waveform clears
    //       whatever was on the panel before.
    //   The kImmediate branch is intentionally absent here -- on RTC wake the
    //   CRC fast path inside RefreshEpdFull() suppresses the SPI transfer
    //   when nothing changed.
    const bool force_full_refresh =
        frame.screen != g_last_rendered_screen ||
        schedule == RefreshSchedule::kCommit;
    if (frame.screen != g_last_rendered_screen) {
        ESP_LOGI(kTag, "EPD: screen change detected (%d -> %d), forcing full refresh",
                 static_cast<int>(g_last_rendered_screen), static_cast<int>(frame.screen));
    }
    g_last_rendered_screen = frame.screen;
    ESP_LOGI(kTag,
             "RefreshFrame: schedule=%s allow_local=%d force_full=%d screen=%d tier=%d",
             RefreshScheduleName(schedule), allow_local_partial ? 1 : 0,
             force_full_refresh ? 1 : 0,
             static_cast<int>(frame.screen),
             static_cast<int>(frame.ai.tier));
    return wqn::RefreshEpdFull(allow_local_partial, force_full_refresh);
}

}  // namespace device_ui_internal
