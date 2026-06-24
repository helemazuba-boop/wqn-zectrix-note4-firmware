// Frame-to-EPD dispatch: chooses which page renderer to call based on screen + schedule.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <string>
#include <vector>

#include "epd_display.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr int kEpdTextWidth = wqn::kEpdWidth - 12;
constexpr size_t kWrappedBodyMaxLines = 4;

esp_err_t RenderFrameToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    ESP_LOGI(kTag, "RenderFrameToEpd: enter schedule=%s screen=%d", RefreshScheduleName(schedule), static_cast<int>(frame.screen));
    const UBaseType_t hwm_before_render = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(kTag, "RenderFrameToEpd: stack HWM before render: %u bytes free", static_cast<unsigned>(hwm_before_render * sizeof(StackType_t)));
    g_last_rendered_screen = frame.screen;
    if (schedule == RefreshSchedule::kClock) {
        if (frame.screen == wqn::UiScreen::kHome) {
            return RenderHomePrimaryRegion(frame.home, schedule);
        }
        if (frame.screen == wqn::UiScreen::kTime && frame.time_app.tile == wqn::TimeTile::kClock &&
            !frame.time_app.config_mode) {
            return RenderTimeClockRegion(schedule, false);
        }
    }

    if (schedule == RefreshSchedule::kTimer) {
        if (frame.screen == wqn::UiScreen::kHome) {
            return RenderHomePrimaryRegion(frame.home, schedule);
        }
        if (frame.screen == wqn::UiScreen::kTime && !frame.time_app.config_mode &&
            frame.time_app.tile != wqn::TimeTile::kClock) {
            return RenderTimerRunRegion(frame.time_app, schedule);
        }
    }

    if (schedule == RefreshSchedule::kConfig && frame.screen == wqn::UiScreen::kTime && frame.time_app.config_mode) {
        return RenderTimeConfigRegion(frame.time_app, schedule);
    }

    if (frame.screen == wqn::UiScreen::kHome) {
        return RenderHomeToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kTime) {
        return RenderTimeToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kAi) {
        return RenderAiToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kTodo) {
        return RenderTodoToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kWord) {
        return RenderWordToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kSettings) {
        return RenderSettingsToEpd(frame, schedule);
    }

    wqn::ClearEpdFramebuffer(true);

    int y = 6;
    for (const wqn::UiLine& line : frame.lines) {
        if (y > wqn::kEpdHeight - 12) {
            break;
        }

        const bool selected = line.style == wqn::UiTextStyle::kSelected;
        const int x = selected ? 6 : 0;
        if (line.style == wqn::UiTextStyle::kWrappedBody) {
            const std::vector<std::string> wrapped =
                wqn::WrapUtf8TextToWidth(line.text, kEpdTextWidth - x, kWrappedBodyMaxLines);
            for (const std::string& wrapped_line : wrapped) {
                if (y > wqn::kEpdHeight - 12) {
                    break;
                }
                ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, y, wrapped_line.c_str(), true), kTag, "draw UI wrapped line");
                y += 18;
            }
        } else {
            const std::string text = LimitForEpd(line.text);
            ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, y, text.c_str(), true), kTag, "draw UI line");
            y += 18;
        }
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
