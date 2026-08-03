// Low-level drawing primitives + UiRect + refresh region helpers.
// Extracted from device_ui.cpp to make the rendering layer inspectable in isolation.

#include "ui_internal.h"

#include <algorithm>
#include <cmath>

#include "display_service.h"

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

void DrawWqnBitmapAsset(int x, int y, const WqnBitmapAsset& asset, bool black)
{
    for (uint8_t yy = 0; yy < asset.height; ++yy) {
        for (uint8_t xx = 0; xx < asset.width; ++xx) {
            if (WqnBitmapPixel(asset, xx, yy)) {
                wqn::DrawEpdPixel(x + xx, y + yy, black);
            }
        }
    }
}

// [rounded] 1bpp rounded-rect outline. Corners use the midpoint-circle octant
// walk mirrored to all four corners (same idea as DrawTimelineNode's disc, but
// boundary-only 1px arcs); straight spans connect them. radius<=0 degrades to
// DrawRect; radius is clamped to half the smaller dimension. This is the
// product-wide card/dialog boundary style (user decision: rounded + 2px
// concentric double-line = persistent selection, overriding the §13.4 default
// sharp-corner language). Typical use: DrawRoundedRect(x,y,w,h,6) for a card,
// plus DrawRoundedRect(x+2,y+2,w-4,h-4,4) when selected.
void DrawRoundedRect(int x, int y, int width, int height, int radius)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (radius <= 0) {
        DrawRect(x, y, width, height);
        return;
    }
    const int r = std::min(radius, std::min(width, height) / 2);
    // Straight spans between the corner arcs (1px each).
    DrawHorizontalLine(x + r, y, width - 2 * r);              // top
    DrawHorizontalLine(x + r, y + height - 1, width - 2 * r); // bottom
    DrawVerticalLine(x, y + r, height - 2 * r);               // left
    DrawVerticalLine(x + width - 1, y + r, height - 2 * r);   // right
    // Corner arcs: midpoint circle over one octant, mirrored to four corners.
    const int cx_l = x + r;
    const int cx_r = x + width - 1 - r;
    const int cy_t = y + r;
    const int cy_b = y + height - 1 - r;
    int dx = r;
    int dy = 0;
    int err = 1 - dx;
    while (dx >= dy) {
        wqn::DrawEpdPixel(cx_r + dx, cy_t - dy, true);  // top-right
        wqn::DrawEpdPixel(cx_r + dy, cy_t - dx, true);
        wqn::DrawEpdPixel(cx_l - dx, cy_t - dy, true);  // top-left
        wqn::DrawEpdPixel(cx_l - dy, cy_t - dx, true);
        wqn::DrawEpdPixel(cx_l - dx, cy_b + dy, true);  // bottom-left
        wqn::DrawEpdPixel(cx_l - dy, cy_b + dx, true);
        wqn::DrawEpdPixel(cx_r + dx, cy_b + dy, true);  // bottom-right
        wqn::DrawEpdPixel(cx_r + dy, cy_b + dx, true);
        ++dy;
        if (err < 0) {
            err += 2 * dy + 1;
        } else {
            --dx;
            err += 2 * (dy - dx) + 1;
        }
    }
}

// [rowfill] Filled rounded rect (reverse-fill with rounded corners). This is
// the density-list row selection block (SelectionStyle::kRowFill): the row is
// ink-filled, the caller draws its content in paper color on top. Corners use
// the same midpoint-circle walk as DrawRoundedRect but fill the interior rows
// between the left/right arc spans. radius<=0 degrades to FillRect.
void FillRoundedRect(int x, int y, int width, int height, int radius)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (radius <= 0) {
        FillRect(x, y, width, height, true);
        return;
    }
    const int r = std::min(radius, std::min(width, height) / 2);
    // For each scanline row, compute the horizontal extent of the rounded
    // shape and fill it. Straight middle rows fill full width; the top/bottom
    // r rows are clipped by the corner circle.
    for (int yy = 0; yy < height; ++yy) {
        int inset = 0;
        // Distance of this row from the nearest top/bottom edge (0-based).
        const int edge = std::min(yy, height - 1 - yy);
        if (edge < r) {
            // Row inside a corner arc: how far the circle pulls the edge in.
            // (r - edge) is the vertical distance from the arc center row.
            const int dy = r - edge;
            // inset = r - round(sqrt(r^2 - dy^2)); classic circle clip.
            const int span = static_cast<int>(std::sqrt(static_cast<double>(r * r - dy * dy)) + 0.5);
            inset = r - span;
        }
        DrawHorizontalLine(x + inset, y + yy, width - 2 * inset);
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

// [L3-semantics] Named wrappers for FillRect(..., black=true) so the 4 distinct
// intents of "black fill" are retrievable at call sites. L2 will migrate the
// existing FillRect(true) callers (selection / progress / role bar / activity
// dot) to these. See docs/13-ui-design-language.md §13.4.6.
void DrawSelectedFill(int x, int y, int width, int height) { FillRect(x, y, width, height, true); }  // reverse-fill selection
void DrawProgressFill(int x, int y, int width, int height) { FillRect(x, y, width, height, true); }  // progress bar fill
void DrawRoleBar(int x, int y, int width, int height) { FillRect(x, y, width, height, true); }       // role marker bar (AI assistant)
void DrawActivityDot(int x, int y, int size) { FillRect(x, y, size, size, true); }                    // activity indicator dot

// [L3-doc] RefreshRegion/RefreshStableRegion: the UiRect is logged + feeds
// FrameSignature dedup, but the actual refresh scope is decided by
// RefreshEpdFull's internal FindDirtyRect (dirty bits vs g_previous_framebuffer).
// Passing a region does NOT hard-clip the panel refresh to that rect; callers
// should treat this as "request a refresh, hint at what changed". True
// region-constrained local refresh is deferred to L4.
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
    // [epd-wedge-fix] kSelection deliberately does NOT allow the windowed
    // local partial: every BUSY wedge across all HIL sessions (5+ multi-second
    // stalls) was an 'EPD local partial failed' on a selection flip, while
    // full-frame partials never wedged -- and both cost the same on this panel
    // (LP 366-772ms vs FFP 408-772ms; the DRF waveform dominates). Clock/timer
    // ticks keep the local path: tiny diffs, power-sensitive, never wedged.
    const bool allow_local_partial =
        schedule == RefreshSchedule::kClock || schedule == RefreshSchedule::kTimer ||
        schedule == RefreshSchedule::kConfig;
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
        schedule == RefreshSchedule::kCommit ||
        frame.prefer_full_refresh;  // [full-refresh-fix] honor producer's full-refresh request (was dead code -> ghosting on tier switch / provision exit)
    if (frame.screen != g_last_rendered_screen) {
        ESP_LOGI(kTag, "EPD: screen change detected (%d -> %d), forcing full refresh",
                 static_cast<int>(g_last_rendered_screen), static_cast<int>(frame.screen));
    }
    ESP_LOGI(kTag,
             "RefreshFrame: schedule=%s allow_local=%d force_full=%d screen=%d tier=%d",
             RefreshScheduleName(schedule), allow_local_partial ? 1 : 0,
             force_full_refresh ? 1 : 0,
             static_cast<int>(frame.screen),
             static_cast<int>(frame.ai.tier));
    const esp_err_t result =
        wqn::RefreshEpdFull(allow_local_partial, force_full_refresh);
    if (result == ESP_OK) {
        g_last_rendered_screen = frame.screen;
    }
    return result;
}

}  // namespace device_ui_internal
