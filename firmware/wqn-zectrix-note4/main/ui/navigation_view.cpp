#include "ui_internal.h"
#include "assets/navigation_icons.h"

#include <cmath>
#include <cstring>
#include <memory>

#include "esp_heap_caps.h"

namespace device_ui_internal {

static_assert(sizeof(kNavigationIcons) / sizeof(kNavigationIcons[0]) ==
              wqn::kNavigationPages.size());

esp_err_t DrawNavigationOverlay(const wqn::UiFrame& frame)
{
    if (!frame.navigation.visible) return ESP_OK;
    FillRoundedRect(kNavigationX, kNavigationY, kNavigationWidth,
                    kNavigationHeight, kNavigationRadius, false);
    DrawRoundedRect(kNavigationX, kNavigationY, kNavigationWidth,
                    kNavigationHeight, kNavigationRadius);
    for (size_t i = 0; i < wqn::kNavigationPages.size(); ++i) {
        const int x = kNavigationItemX + static_cast<int>(i) * kNavigationItemStep;
        const bool selected = i == frame.navigation.selected;
        if (selected) {
            DrawSelectionDecoration(x, kNavigationItemY, kNavigationItemWidth,
                                    kNavigationItemHeight, SelectionStyle::kInvert);
        }
        DrawWqnBitmapAsset(x + 12, kNavigationIconY, *kNavigationIcons[i], !selected);
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(x, kNavigationLabelY, kNavigationItemWidth,
                             wqn::kNavigationPages[i].label, !selected),
            "wqn_nav", "draw navigation label");
        if (wqn::kNavigationPages[i].screen == frame.screen) {
            FillRoundedRect(x + 22, kNavigationCurrentY, 4, 3, 1);
        }
    }
    return ESP_OK;
}

esp_err_t RefreshNavigationGrayImage(const wqn::UiFrame& frame, const uint8_t* gray4, size_t size)
{
    if (!frame.navigation.visible) return wqn::RefreshEpdGray16(gray4, size);
    ESP_RETURN_ON_FALSE(gray4 != nullptr && size == wqn::kEpdGray4PayloadSize,
                        ESP_ERR_INVALID_ARG, "wqn_nav", "invalid gray navigation image");
    // Gray images bypass the mono framebuffer. Keep the immutable source and
    // compose into one temporary PSRAM copy; release it after the owner refresh.
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> composed(
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        &heap_caps_free);
    ESP_RETURN_ON_FALSE(composed != nullptr, ESP_ERR_NO_MEM, "wqn_nav", "allocate gray navigation image");
    std::memcpy(composed.get(), gray4, size);
    // Only the overlay belongs in this scratch mono mask. The old page may
    // leave ink in the rounded transparent corners, so start from paper.
    wqn::ClearEpdFramebuffer(true);
    ESP_RETURN_ON_ERROR(DrawNavigationOverlay(frame), "wqn_nav", "draw gray navigation");
    for (int yy = 0; yy < kNavigationHeight; ++yy) {
        const int edge = std::min(yy, kNavigationHeight - 1 - yy);
        const int r = kNavigationRadius;
        const int dy = r - edge;
        const int inset = edge < r
            ? r - static_cast<int>(std::sqrt(static_cast<double>(r * r - dy * dy)) + 0.5)
            : 0;
        for (int x = kNavigationX; x < kNavigationX + kNavigationWidth; ++x) {
            const int y = kNavigationY + yy;
            const bool ink = wqn::ReadEpdPixel(x, y);
            // The midpoint-circle outline can extend one pixel beyond the
            // rounded fill. Include its ink while preserving gray corners.
            if (!ink && (x < kNavigationX + inset ||
                         x >= kNavigationX + kNavigationWidth - inset)) continue;
            const size_t pixel = static_cast<size_t>(y) * wqn::kEpdWidth + x;
            uint8_t& byte = composed.get()[pixel / 2];
            const uint8_t level = ink ? 0 : 15;
            byte = (pixel & 1U) != 0 ? (byte & 0xf0) | level : (byte & 0x0f) | (level << 4);
        }
    }
    return wqn::RefreshEpdGray16(composed.get(), size);
}

}  // namespace device_ui_internal
