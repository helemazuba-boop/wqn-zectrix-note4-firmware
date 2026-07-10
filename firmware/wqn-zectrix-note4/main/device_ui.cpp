// Device UI entry points: StartDeviceUiIfEnabled() + DeviceUiTask().
// All other responsibilities (rendering, refresh, cloud, state, input) live in main/ui/.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "ai_session.h"
#include "button_input.h"
#include "config.h"
#include "device_ui.h"
#include "driver/gpio.h"
#include "epd_display.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "flash_session.h"
#include "power_manager.h"
#include "ui/ui_internal.h"
#include "ui_model.h"
#include "wqn_api.h"

namespace device_ui_internal {
// Last screen survives deep sleep via RTC slow memory. Initialized to kHome (=3)
// rather than 0 so cold boot / RTC corruption can't accidentally land on kAi.
// LoadUiState() validates the restored value before use.
RTC_DATA_ATTR int g_rtc_screen_val = static_cast<int>(wqn::UiScreen::kHome);

// [timer-skip] Last successfully-rendered (screen, clock-label) pair, stored
// in RTC slow memory. On a timer wake-up we compare the freshly-rendered
// frame's clock label + screen against these and skip the entire refresh
// pipeline if both match -- the panel already shows this content.
// Initialized to a sentinel screen value so the first wake-up after a cold
// boot / RTC reset is never mistakenly skipped.
RTC_DATA_ATTR int g_rtc_last_rendered_screen_id = -1;
RTC_DATA_ATTR char g_rtc_last_rendered_clock[8] = {};
} // namespace device_ui_internal

namespace {

// [timer-skip] Compute the screen-id that should participate in the
// timer-wakeup skip decision. Configurable / editing screens must never be
// skipped because their state can change between refreshes without touching
// the clock label.
bool ScreenSupportsTimerSkip(wqn::UiScreen screen)
{
    return screen == wqn::UiScreen::kHome ||
           screen == wqn::UiScreen::kTime ||
           screen == wqn::UiScreen::kWord ||
           screen == wqn::UiScreen::kTodo;
}

bool TrySkipInitRefresh(wqn::UiScreen screen, const char* clock_label)
{
    using device_ui_internal::g_rtc_last_rendered_screen_id;
    using device_ui_internal::g_rtc_last_rendered_clock;
    // [timer-skip] ESP32-S3's built-in USB Serial/JTAG controller triggers
    // a USB_UART_CHIP_RESET on every deep-sleep wake-up. That reset clears
    // esp_sleep_get_wakeup_cause() (it returns ESP_SLEEP_WAKEUP_UNDEFINED),
    // but RTC slow memory is preserved across that reset -- so the cached
    // (screen, clock) values written by RecordInitRefresh() are still valid.
    //
    // We use esp_sleep_get_ext1_wakeup_status() to distinguish user-input
    // wake-ups (button presses on GPIO 0/18) from automatic timer wake-ups
    // (PCF8563 INT on GPIO 5). When the chip reset hides the cause, EXT1
    // status also reads 0 -- in that case we still try the skip because the
    // framebuffer is identical by definition (the panel cannot have changed
    // while the CPU was asleep).
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const uint64_t ext1_status = esp_sleep_get_ext1_wakeup_status();
    const bool user_input_wakeup =
        cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1 ||
        cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_UART ||
        cause == ESP_SLEEP_WAKEUP_TOUCHPAD || cause == ESP_SLEEP_WAKEUP_ULP;
    // If EXT1 status is non-zero, only treat it as a user-input wake-up if
    // one of the button pins (GPIO 0 / GPIO 18) is set. A bare GPIO 5 hit
    // (PCF8563 INT) is a timer wake-up and is eligible for the skip.
    const bool button_pin_set =
        (ext1_status & ((1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_18))) != 0;
    ESP_LOGI("wqn_ui",
             "TrySkipInitRefresh: cause=%d ext1_status=0x%llx screen=%d clock=%s "
             "rtc_screen=%d rtc_clock=\"%s\" user_input_wakeup=%d button_pin_set=%d",
             static_cast<int>(cause),
             static_cast<unsigned long long>(ext1_status),
             static_cast<int>(screen), clock_label ? clock_label : "(null)",
             g_rtc_last_rendered_screen_id, g_rtc_last_rendered_clock,
             user_input_wakeup ? 1 : 0, button_pin_set ? 1 : 0);
    if (user_input_wakeup || button_pin_set) {
        return false;
    }
    if (!ScreenSupportsTimerSkip(screen)) {
        return false;
    }
    if (clock_label == nullptr || clock_label[0] == '\0') {
        return false;
    }
    if (g_rtc_last_rendered_screen_id < 0) {
        // True cold boot / RTC never programmed -- nothing valid to compare.
        return false;
    }
    if (static_cast<int>(screen) != g_rtc_last_rendered_screen_id) {
        return false;
    }
    if (std::strcmp(clock_label, g_rtc_last_rendered_clock) != 0) {
        return false;
    }
    ESP_LOGI("wqn_ui", "Init refresh skipped: panel already shows this exact frame (screen=%d clock=%s cause=%d ext1=0x%llx)",
             static_cast<int>(screen), clock_label, static_cast<int>(cause),
             static_cast<unsigned long long>(ext1_status));
    return true;
}

void RecordInitRefresh(wqn::UiScreen screen, const char* clock_label)
{
    using device_ui_internal::g_rtc_last_rendered_screen_id;
    using device_ui_internal::g_rtc_last_rendered_clock;
    if (!ScreenSupportsTimerSkip(screen) || clock_label == nullptr || clock_label[0] == '\0') {
        return;
    }
    g_rtc_last_rendered_screen_id = static_cast<int>(screen);
    std::strncpy(g_rtc_last_rendered_clock, clock_label, sizeof(g_rtc_last_rendered_clock) - 1);
    g_rtc_last_rendered_clock[sizeof(g_rtc_last_rendered_clock) - 1] = '\0';
}

// [timer-skip] Main-loop minute-tick gate. Returns true when the minute has
// not advanced since the last successful refresh and ScreenUsesClockMinute
// would otherwise issue a kClock schedule. By suppressing the schedule here
// we avoid the full-refresh forced by g_previous_framebuffer_synced=false
// after every deep-sleep wake-up.
bool ShouldSkipMinuteTickRefresh(wqn::UiScreen screen, const char* clock_label)
{
    using device_ui_internal::g_rtc_last_rendered_screen_id;
    using device_ui_internal::g_rtc_last_rendered_clock;
    if (clock_label == nullptr || clock_label[0] == '\0') {
        return false;
    }
    if (!ScreenSupportsTimerSkip(screen)) {
        return false;
    }
    if (g_rtc_last_rendered_screen_id < 0) {
        return false;
    }
    if (static_cast<int>(screen) != g_rtc_last_rendered_screen_id) {
        return false;
    }
    return std::strcmp(clock_label, g_rtc_last_rendered_clock) == 0;
}

constexpr char kTag[] = "wqn_ui";
constexpr TickType_t kUiPollDelayTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kUiIdlePollDelayTicks = pdMS_TO_TICKS(500);
constexpr TickType_t kStatusRefreshDelayTicks = pdMS_TO_TICKS(60000);

using device_ui_internal::BuildHomeSummary;
using device_ui_internal::CheckBatteryProtection;
using device_ui_internal::CurrentClockLabel;
using device_ui_internal::FrameSignature;
using device_ui_internal::LoadUiState;
using device_ui_internal::RefreshSchedule;
using device_ui_internal::RequestEpdUiRefresh;
using device_ui_internal::ScreenUsesClockMinute;
using device_ui_internal::SeedClockFromBuildTimeIfNeeded;
using device_ui_internal::ShouldRefreshTimeTick;
using device_ui_internal::StrongerSchedule;
using device_ui_internal::UpdateHomePrimaryTimeLine;
using device_ui_internal::ApplyButtonEvent;
using device_ui_internal::ApplyTodoCloudResult;
using device_ui_internal::ApplyWordCloudResult;
using device_ui_internal::IsTodoCloudBusy;
using device_ui_internal::IsWordCloudBusy;
using device_ui_internal::g_todo_cloud_busy;
using device_ui_internal::g_todo_result_queue;
using device_ui_internal::g_refresh_busy;
using device_ui_internal::g_rtc_screen_val;
using device_ui_internal::g_word_cloud_busy;
using device_ui_internal::g_word_result_queue;

// Local copy — only DeviceUiTask reads/writes this; ui_refresh.cpp's
// g_last_active_us is the cross-file symbol declared in ui_internal.h
// (currently only forwarded, no definition site yet).
int64_t g_last_active_us_local = 0;

void DeviceUiTask(void*)
{
    ESP_LOGI(kTag, "device UI task started");
    // Skip the wake-up timer case: we just woke from RTC and the user did
    // not interact with the device, so recording activity here would
    // defeat the idle-off timer the moment the user stops touching the
    // keypad after a long sleep.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        wqn::NoteUserActivity();
    }
    SeedClockFromBuildTimeIfNeeded();

    esp_err_t result = wqn::InitButtonInput();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "button input init failed: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    result = wqn::InitEpdDisplay();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "EPD display init failed: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "EPD display init OK, preparing first frame");

    wqn::UiState state;
    ESP_LOGI(kTag, "DeviceUiTask: calling LoadUiState");
    LoadUiState(&state);
    ESP_LOGI(kTag, "DeviceUiTask: LoadUiState done");
    // [power-fix] Do NOT seed g_last_rendered_screen from state.screen here:
    // a deep-sleep wake-up that goes through USB_UART_CHIP_RESET loses this
    // RAM variable, and unconditionally re-writing it from the freshly loaded
    // UI state would later trip the "screen change detected" branch in
    // RefreshFrame() and force a full refresh on every wake-up. Treat -1 as
    // "unknown", which forces the first refresh to be full but keeps subsequent
    // wake-ups inside the same screen and clock label on the RTC-CRC fast path.
    CheckBatteryProtection();
    ESP_LOGI(kTag, "DeviceUiTask: CheckBatteryProtection done");
    std::string last_clock_label = CurrentClockLabel();
    std::string last_frame_signature;
    {
        const wqn::UiFrame frame = wqn::RenderUiFrame(state);
        last_frame_signature = FrameSignature(frame);
        RefreshSchedule init_schedule = RefreshSchedule::kImmediate;
        // [epd-crc-bypass-fix] On timer wake-up we want to skip the
        // un-conditional full refresh, but only if the current screen can
        // tolerate a partial pipeline. Screens like AI / Word / Todo have
        // scrolling overlays or partial-screen markers that would ghost if
        // we only refreshed the clock region, so fall back to kImmediate.
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
            (state.screen == wqn::UiScreen::kTime ||
             state.screen == wqn::UiScreen::kHome)) {
            init_schedule = RefreshSchedule::kClock;
        }
        // [timer-skip] If a timer wake-up finds the freshly-rendered frame's
        // (screen, clock label) identical to the last successful render --
        // e.g. a per-minute RTC tick that fires within the same minute --
        // skip the refresh pipeline entirely. The panel already shows this
        // content, so we avoid any SPI traffic and any flicker.
        const char* const init_clock = last_clock_label.c_str();
        const bool skip_init_refresh = TrySkipInitRefresh(state.screen, init_clock);
        ESP_LOGI(kTag, "Requesting initial EPD refresh: signature_len=%zu schedule=%s skip=%d",
                 last_frame_signature.size(), device_ui_internal::RefreshScheduleName(init_schedule),
                 skip_init_refresh ? 1 : 0);
        if (!skip_init_refresh) {
            const bool req_ok = RequestEpdUiRefresh(frame, last_frame_signature, init_schedule);
            ESP_LOGI(kTag, "RequestEpdUiRefresh returned %d, g_refresh_task=%p", req_ok, device_ui_internal::g_refresh_task);
            if (req_ok) {
                RecordInitRefresh(state.screen, init_clock);
            }
        }
    }
    TickType_t last_status_refresh = xTaskGetTickCount();
    g_last_active_us_local = esp_timer_get_time();
    TickType_t poll_delay = kUiPollDelayTicks;

    while (true) {
        RefreshSchedule refresh_schedule = RefreshSchedule::kNone;
        const wqn::ButtonEvent event = wqn::PollButtonInput();
        if (event.HasEvent()) {
            wqn::NoteUserActivity();
            wqn::NoteEpdActivity();
            poll_delay = kUiPollDelayTicks;
            g_last_active_us_local = esp_timer_get_time();
        }
        // Apply the button event regardless of EPD refresh state. The previous
        // "skip while g_refresh_busy" guard was making press/release events
        // silently dropped whenever a slow partial refresh was in flight (a
        // timer-page layout change can hold g_refresh_busy=true for 100-300 ms,
        // which is long enough to swallow the user's next press entirely). The
        // refresh task itself dedups via frame_signature and the primary /
        // secondary queue pair in ui_refresh.cpp, so racing with a refresh in
        // flight is safe -- the worst case is the new event lands in the
        // secondary slot and is picked up on the next consume.
        {
            const RefreshSchedule before_sched = refresh_schedule;
            const RefreshSchedule after_sched =
                StrongerSchedule(refresh_schedule, ApplyButtonEvent(event, &state));
            if (after_sched != before_sched && after_sched == RefreshSchedule::kAi) {
                ESP_LOGI(kTag,
                         "AI button refresh scheduled: button=%d type=%d busy=%d screen=%d tier=%d",
                         static_cast<int>(event.button), static_cast<int>(event.type),
                         g_refresh_busy ? 1 : 0,
                         static_cast<int>(state.screen),
                         static_cast<int>(state.ai.tier));
            }
            refresh_schedule = after_sched;
        }

        if (g_todo_result_queue != nullptr) {
            device_ui_internal::TodoCloudResult* todo_result = nullptr;
            while (xQueueReceive(g_todo_result_queue, &todo_result, 0) == pdTRUE) {
                g_todo_cloud_busy = false;
                if (todo_result != nullptr) {
                    if (ApplyTodoCloudResult(&state, *todo_result) && state.screen == wqn::UiScreen::kTodo) {
                        refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kCommit);
                    }
                    delete todo_result;
                }
            }
        }
        if (g_word_result_queue != nullptr) {
            device_ui_internal::WordCloudResult* word_result = nullptr;
            while (xQueueReceive(g_word_result_queue, &word_result, 0) == pdTRUE) {
                g_word_cloud_busy = false;
                if (word_result != nullptr) {
                    if (ApplyWordCloudResult(&state, *word_result) && state.screen == wqn::UiScreen::kWord) {
                        refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kCommit);
                    }
                    delete word_result;
                }
            }
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (wqn::TickTimeApp(&state.time_app, now_ms)) {
            UpdateHomePrimaryTimeLine(&state);
            if (ShouldRefreshTimeTick(state)) {
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kTimer);
            }
        }

        if (wqn::TickAiSession(&state, now_ms) && state.screen == wqn::UiScreen::kAi) {
            refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
        }

        // v2: while the user is recording, the top toast label needs to tick
        // up so "● 录音中 00:04" advances once a second. We do this here on
        // the UI task (the audio task only knows about stream samples).
        if (state.screen == wqn::UiScreen::kAi &&
            state.ai.status == wqn::AiSessionStatus::kListening) {
            // Use the recording start time stored in g_state via status_since_ms.
            const int32_t elapsed = static_cast<int32_t>(now_ms - state.ai.status_since_ms);
            if (elapsed >= 0) {
                wqn::SetAiRecordingLabel(elapsed);
            }
        }

wqn::AiStreamingStatusView streaming_view{};
        bool streaming_changed = false;
#if CONFIG_WQN_AI_ENABLE
        streaming_changed = wqn::CopyAiStreamingStatus(&streaming_view);
#endif
        if (streaming_changed) {
            // v2 SSE status overrides the static "上传" label.
            switch (streaming_view.status) {
                case wqn::AiSessionStatus::kStreaming:
                    state.ai.status = wqn::AiSessionStatus::kStreaming;
                    state.ai.pending_text = streaming_view.pending_label;
                    break;
                case wqn::AiSessionStatus::kReplyReady:
                    state.ai.status = wqn::AiSessionStatus::kReplyReady;
                    state.ai.pending_text.clear();
                    break;
                case wqn::AiSessionStatus::kError:
                    state.ai.status = wqn::AiSessionStatus::kError;
                    break;
                default:
                    break;
            }
            if (state.screen == wqn::UiScreen::kAi &&
                streaming_view.last_render_ms != state.ai.last_render_ms) {
                state.ai.last_render_ms = streaming_view.last_render_ms;
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
            }
            // [streaming-fix] Streaming consumer demanded a full redraw (e.g.
            // kFinal clearing the upload toast and showing the new assistant
            // bubble). Without this bump the screen would stay on the stale
            // "上传" pixel set, since the FrameSignature diff may otherwise
            // not catch the transition when only toast_visible + content
            // changed inside one tick.
            if (state.screen == wqn::UiScreen::kAi && streaming_view.force_full_render) {
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
            }
            if (state.screen == wqn::UiScreen::kAi) {
                state.ai.status_detail = streaming_view.tool_label;
            }
        }

#if CONFIG_WQN_AI_ENABLE
        if (wqn::CopyAiSessionToUi(&state.ai) && state.screen == wqn::UiScreen::kAi) {
            refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
        }
        // [amp-fix] Pump the flash amp idle-tail timer regardless of screen so
        // stale open-amp state is closed even when the user has navigated away.
        wqn::PollFlashAmpIdle();
        wqn::FlashUiState flash_ui;
        if (wqn::CopyFlashStateToUi(&flash_ui)) {
            // Map FlashStatus onto the local AI state so the render layer can
            // read everything from state.ai without grabbing any mutex.
            switch (flash_ui.status) {
                case wqn::FlashStatus::kError:
                    state.ai.status = wqn::AiSessionStatus::kError;
                    state.ai.flash_is_streaming = false;
                    break;
                case wqn::FlashStatus::kStreaming:
                    state.ai.status = wqn::AiSessionStatus::kWaitingReply;
                    state.ai.flash_is_streaming = true;
                    break;
                case wqn::FlashStatus::kConnecting:
                    state.ai.status = wqn::AiSessionStatus::kListening;
                    state.ai.flash_is_streaming = false;
                    break;
                default:
                    state.ai.status = wqn::AiSessionStatus::kIdle;
                    state.ai.flash_is_streaming = false;
                    break;
            }
            state.ai.flash_transcript = flash_ui.user_transcript;
            state.ai.assistant_text = flash_ui.assistant_text;
            state.ai.pending_text = flash_ui.pending_text;
            state.ai.flash_pending = flash_ui.pending_text;
            state.ai.flash_error = flash_ui.error_message;
            // Flash tier uses status_detail for the tool chip on the input bar;
            // when there's no tool active we leave the previous value alone so a
            // settled response still shows its ASR/llm latency summary.
            if (!flash_ui.tool_label.empty()) {
                state.ai.status_detail = flash_ui.tool_label;
            } else if (!state.ai.flash_is_streaming && !state.ai.status_detail.empty()) {
                // After a turn settles, prefer the asr/llm summary if the tool
                // chip has not been cleared by the auto-timeout in ai_session.
            }
            state.ai.status_since_ms = flash_ui.status_since_ms;
            if (state.screen == wqn::UiScreen::kAi) {
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
            }
        }
#endif

        const std::string clock_label = CurrentClockLabel();
        if (clock_label != last_clock_label) {
            last_clock_label = clock_label;
            UpdateHomePrimaryTimeLine(&state);
            // [timer-skip] If the new minute label matches the last
            // successfully-rendered clock label we still need to update
            // state.time_app / home.primary_time_line above, but the on-screen
            // pixels are already correct -- suppress the refresh request so we
            // don't fall into the deep-sleep-induced forced-full-refresh path.
            const bool minute_changed_on_panel =
                !ShouldSkipMinuteTickRefresh(state.screen, clock_label.c_str());
            if (ScreenUsesClockMinute(state) && minute_changed_on_panel) {
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kClock);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_status_refresh >= kStatusRefreshDelayTicks) {
            const std::string before_signature = FrameSignature(wqn::RenderUiFrame(state));
            LoadUiState(&state);
            CheckBatteryProtection();
            const std::string after_signature = FrameSignature(wqn::RenderUiFrame(state));
            if (after_signature != before_signature && refresh_schedule == RefreshSchedule::kNone) {
                refresh_schedule = RefreshSchedule::kSelection;
            }
            last_status_refresh = now;
        }

        if (refresh_schedule != RefreshSchedule::kNone) {
            const wqn::UiFrame frame = wqn::RenderUiFrame(state);
            // [force-full-fix] Consume the one-shot flag HERE, after the final
            // render frame is built. RenderUiFrame above checks (but doesn't
            // clear) the flag, so all signature-comparison calls earlier in
            // this tick also saw it. Now we apply it to the actual frame that
            // will be pushed to the EPD, and clear it for the next tick.
            if (wqn::ConsumeForceFullRefresh()) {
                const_cast<wqn::UiFrame&>(frame).prefer_full_refresh = true;
            }
            const std::string frame_signature = FrameSignature(frame);
            ESP_LOGI(kTag,
                     "dispatch refresh: schedule=%s sig_match=%d last_len=%zu new_len=%zu screen=%d tier=%d",
                     device_ui_internal::RefreshScheduleName(refresh_schedule),
                     frame_signature == last_frame_signature ? 1 : 0,
                     last_frame_signature.size(), frame_signature.size(),
                     static_cast<int>(state.screen), static_cast<int>(state.ai.tier));
            if (frame_signature != last_frame_signature) {
                if (RequestEpdUiRefresh(frame, frame_signature, refresh_schedule)) {
                    ESP_LOGI(kTag, "EPD UI refresh requested: schedule=%s", device_ui_internal::RefreshScheduleName(refresh_schedule));
                    last_frame_signature = frame_signature;
                    // [timer-skip] Persist the clock label that is now on the
                    // panel so the next minute-tick inside the same minute
                    // (e.g. a re-render caused by a status refresh or by the
                    // main loop running twice before deep sleep) is suppressed.
                    RecordInitRefresh(state.screen, last_clock_label.c_str());
                    g_last_active_us_local = esp_timer_get_time();
                    poll_delay = kUiPollDelayTicks;
                } else {
                    ESP_LOGW(kTag, "EPD UI refresh request rejected: schedule=%s",
                             device_ui_internal::RefreshScheduleName(refresh_schedule));
                }
            } else {
                ESP_LOGI(kTag, "EPD UI refresh skipped: signature unchanged");
            }
        }

        wqn::PowerOffEpdAfterIdleIfNeeded();

        // [epd-hang-fix] esp_light_sleep_start() unconditionally isolates all GPIOs
        // (CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y), which releases the GPIO 17 power-latch
        // and causes a 1.6s boot loop on first boot. Disabled until the proper
        // wakeup source + gpio_hold_en(GPIO_17) flow is in place.
        vTaskDelay(pdMS_TO_TICKS(10));

        g_rtc_screen_val = static_cast<int>(state.screen);
        const bool enable_timer_wakeup = (state.screen == wqn::UiScreen::kHome ||
                                          state.screen == wqn::UiScreen::kTime);
        // [download-fix] Don't deep-sleep while a cloud task is mid-flight
        // (word-pack download / todo sync). Sleeping yanks the CPU and cuts
        // the radio during a TLS stream, truncating the download and
        // corrupting the local cache.
        if (!g_word_cloud_busy && !g_todo_cloud_busy) {
            wqn::EnterDeepSleepIfEnabled(enable_timer_wakeup);
        }

        const int64_t idle_ms = (esp_timer_get_time() - g_last_active_us_local) / 1000;
        const bool screen_active = (state.screen == wqn::UiScreen::kTime ||
                                    state.screen == wqn::UiScreen::kAi ||
                                    state.screen == wqn::UiScreen::kWord);
        if (refresh_schedule != RefreshSchedule::kNone ||
            g_todo_cloud_busy || g_word_cloud_busy ||
            screen_active) {
            poll_delay = kUiPollDelayTicks;
        } else if (idle_ms < 3000) {
            poll_delay = kUiPollDelayTicks;
        } else {
            poll_delay = kUiIdlePollDelayTicks;
        }

        vTaskDelay(poll_delay);
    }
}

}  // namespace

namespace wqn {

esp_err_t StartDeviceUiIfEnabled()
{
#if CONFIG_WQN_EPD_UI_ENABLE
    if (device_ui_internal::g_refresh_mutex == nullptr) {
        device_ui_internal::g_refresh_mutex = xSemaphoreCreateMutex();
        if (device_ui_internal::g_refresh_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_todo_request_queue == nullptr) {
        device_ui_internal::g_todo_request_queue = xQueueCreate(2, sizeof(device_ui_internal::TodoCloudRequest));
        if (device_ui_internal::g_todo_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_todo_result_queue == nullptr) {
        device_ui_internal::g_todo_result_queue = xQueueCreate(2, sizeof(device_ui_internal::TodoCloudResult*));
        if (device_ui_internal::g_todo_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_word_request_queue == nullptr) {
        device_ui_internal::g_word_request_queue = xQueueCreate(3, sizeof(device_ui_internal::WordCloudRequest));
        if (device_ui_internal::g_word_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_word_result_queue == nullptr) {
        device_ui_internal::g_word_result_queue = xQueueCreate(3, sizeof(device_ui_internal::WordCloudResult*));
        if (device_ui_internal::g_word_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_refresh_task == nullptr) {
        const BaseType_t refresh_created =
            xTaskCreate(device_ui_internal::EpdRefreshTask, "wqn_epd_refresh", 12288, nullptr, 5, &device_ui_internal::g_refresh_task);
        if (refresh_created != pdPASS) {
            device_ui_internal::g_refresh_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_todo_task == nullptr) {
        const BaseType_t todo_created =
            xTaskCreate(device_ui_internal::TodoCloudTask, "wqn_todo_cloud", 8192, nullptr, 3, &device_ui_internal::g_todo_task);
        if (todo_created != pdPASS) {
            device_ui_internal::g_todo_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_word_task == nullptr) {
        const BaseType_t word_created =
            xTaskCreate(device_ui_internal::WordCloudTask, "wqn_word_cloud", 8192, nullptr, 3, &device_ui_internal::g_word_task);
        if (word_created != pdPASS) {
            device_ui_internal::g_word_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    const BaseType_t created = xTaskCreate(DeviceUiTask, "wqn_ui", 8192, nullptr, 4, nullptr);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    ESP_LOGI(kTag, "EPD device UI disabled by CONFIG_WQN_EPD_UI_ENABLE");
    return ESP_OK;
#endif
}

}  // namespace wqn
