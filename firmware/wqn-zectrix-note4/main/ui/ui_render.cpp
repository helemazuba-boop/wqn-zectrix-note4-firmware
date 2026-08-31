// Frame-to-EPD dispatch: chooses which page renderer to call based on screen + schedule.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <string>
#include <vector>

#include "display_service.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

esp_err_t RenderFrameToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    // [epd-owner] Hold the whole-frame lock across the entire clear->draw->
    // refresh sequence below. The individual RefreshEpdFull/RefreshFrame calls
    // only serialize their own transmit; without this outer transaction, idle
    // cleanup or another render could interleave between our draws and the
    // refresh on the shared framebuffer. The recursive mutex lets the inner
    // refresh re-enter on this task; the RAII destructor releases on every
    // early return below.
    wqn::EpdFrameTransaction frame_txn;
    ESP_RETURN_ON_FALSE(
        frame_txn.locked(), ESP_ERR_NO_MEM, kTag,
        "take EPD frame transaction");
    // A background sync wake deliberately defers panel initialization until
    // pixels actually need to change. Initialize before any draw operation;
    // relying on RefreshEpdFull's later lazy init would clear the framebuffer
    // after rendering and transmit a blank/incomplete page.
    ESP_RETURN_ON_ERROR(
        wqn::InitEpdDisplay(), kTag, "initialize EPD before frame render");
    ESP_LOGI(kTag, "RenderFrameToEpd: enter schedule=%s screen=%d", RefreshScheduleName(schedule), static_cast<int>(frame.screen));
    const UBaseType_t hwm_before_render = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(kTag, "RenderFrameToEpd: stack HWM before render: %u bytes free", static_cast<unsigned>(hwm_before_render * sizeof(StackType_t)));
    const bool region_frame_available = wqn::IsEpdFramebufferSynchronized();
    const bool region_schedule =
        schedule == RefreshSchedule::kClock ||
        schedule == RefreshSchedule::kTimer;
    if (region_schedule && !region_frame_available) {
        // [timer-skip] Deep sleep loses the PSRAM previous-frame mirror. A
        // region renderer would otherwise draw onto the freshly-cleared white
        // framebuffer, then the driver's conservative full refresh would
        // erase every untouched part of the physical page. Fall through to
        // the complete page renderer; the driver will establish a new mirror.
        ESP_LOGI(
            kTag,
            "EPD region refresh promoted to complete frame: schedule=%s screen=%d previous-unsynced",
            RefreshScheduleName(schedule), static_cast<int>(frame.screen));
    }
    if (region_frame_available && schedule == RefreshSchedule::kClock) {
        if (frame.screen == wqn::UiScreen::kHome) {
            return wqn::TimeAppHasActiveTimer(frame.time_app)
                ? RenderHomeStatusBarRegion(frame.home, schedule)
                : RenderHomePrimaryRegion(frame.home, schedule);
        }
        if (frame.screen == wqn::UiScreen::kTime && frame.time_app.tile == wqn::TimeTile::kClock &&
            !frame.time_app.config_mode) {
            return RenderTimeClockRegion(schedule, false);
        }
    }

    if (region_frame_available && schedule == RefreshSchedule::kTimer) {
        if (frame.screen == wqn::UiScreen::kHome) {
            return RenderHomePrimaryRegion(frame.home, schedule);
        }
        if (frame.screen == wqn::UiScreen::kTime && !frame.time_app.config_mode &&
            frame.time_app.tile != wqn::TimeTile::kClock) {
            return RenderTimerRunRegion(frame.time_app, schedule);
        }
    }

    if (schedule == RefreshSchedule::kSelection &&
        frame.screen == wqn::UiScreen::kTime && !frame.time_app.config_mode &&
        frame.time_app.tile != wqn::TimeTile::kClock) {
        return RenderTimerActionRegion(frame.time_app, schedule);
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
    if (frame.screen == wqn::UiScreen::kOpenCode) {
        return RenderAgentToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kTodo) {
        return RenderTodoToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kWord) {
        return RenderWordToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kNote) {
        return RenderNoteToEpd(frame, schedule);
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
                wqn::WrapUtf8TextToWidth(line.text, kTextWidth - x, kMaxWrapLines);
            for (const std::string& wrapped_line : wrapped) {
                if (y > wqn::kEpdHeight - 12) {
                    break;
                }
                ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, y, wrapped_line.c_str(), true), kTag, "draw UI wrapped line");
                y += kCjkLineHeight;
            }
        } else {
            const std::string text = LimitForEpd(line.text);
            ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, y, text.c_str(), true), kTag, "draw UI line");
            y += kCjkLineHeight;
        }
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
