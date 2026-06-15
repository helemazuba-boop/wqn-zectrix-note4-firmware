// Device UI entry points: StartDeviceUiIfEnabled() + DeviceUiTask().
// All other responsibilities (rendering, refresh, cloud, state, input) live in main/ui/.

#include "device_ui.h"

#include "ui/ui_internal.h"

#include <cstdio>
#include <ctime>
#include <string>

#include "ai_session.h"
#include "button_input.h"
#include "config.h"
#include "driver/gpio.h"
#include "epd_display.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "power_manager.h"
#include "ui_model.h"
#include "wqn_api.h"

namespace {

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
using device_ui_internal::g_word_cloud_busy;
using device_ui_internal::g_word_result_queue;

// Local copy — only DeviceUiTask reads/writes this; ui_refresh.cpp's
// g_last_active_us is the cross-file symbol declared in ui_internal.h
// (currently only forwarded, no definition site yet).
int64_t g_last_active_us_local = 0;

void DeviceUiTask(void*)
{
    ESP_LOGI(kTag, "device UI task started");
    wqn::NoteUserActivity();
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
    CheckBatteryProtection();
    ESP_LOGI(kTag, "DeviceUiTask: CheckBatteryProtection done");
    std::string last_clock_label = CurrentClockLabel();
    std::string last_frame_signature;
    {
        const wqn::UiFrame frame = wqn::RenderUiFrame(state);
        last_frame_signature = FrameSignature(frame);
        ESP_LOGI(kTag, "Requesting initial EPD refresh: signature_len=%zu schedule=immediate", last_frame_signature.size());
        const bool req_ok = RequestEpdUiRefresh(frame, last_frame_signature, RefreshSchedule::kImmediate);
        ESP_LOGI(kTag, "RequestEpdUiRefresh returned %d, g_refresh_task=%p", req_ok, device_ui_internal::g_refresh_task);
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
        refresh_schedule = StrongerSchedule(refresh_schedule, ApplyButtonEvent(event, &state));

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

#if CONFIG_WQN_AI_ENABLE
        if (wqn::CopyAiSessionToUi(&state.ai) && state.screen == wqn::UiScreen::kAi) {
            refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
        }
#endif

        const std::string clock_label = CurrentClockLabel();
        if (clock_label != last_clock_label) {
            last_clock_label = clock_label;
            UpdateHomePrimaryTimeLine(&state);
            if (ScreenUsesClockMinute(state)) {
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
            const std::string frame_signature = FrameSignature(frame);
            if (frame_signature != last_frame_signature) {
                if (RequestEpdUiRefresh(frame, frame_signature, refresh_schedule)) {
                    ESP_LOGI(kTag, "EPD UI refresh requested: schedule=%s", device_ui_internal::RefreshScheduleName(refresh_schedule));
                    last_frame_signature = frame_signature;
                    g_last_active_us_local = esp_timer_get_time();
                    poll_delay = kUiPollDelayTicks;
                }
            }
        }

        wqn::PowerOffEpdAfterIdleIfNeeded();

        // [power-fix] With CONFIG_PM_ENABLE=y, the FreeRTOS idle task now
        // auto-enters light sleep whenever every task is in vTaskDelay. The
        // previous "esp_light_sleep_start() unconditionally isolates all
        // GPIOs" failure was caused by CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND
        // releasing the GPIO 17 power-latch; that is now suppressed by
        // gpio_sleep_sel_dis(kBoardPowerLatch) in InitZectrixNote4SafePins().
        // Bumping the delay from 10ms to 50ms gives PM DFS a cleaner window
        // to actually transition the CPU into light sleep, while still
        // keeping the UI loop responsive to button events.
        vTaskDelay(pdMS_TO_TICKS(50));

        wqn::EnterDeepSleepIfEnabled();

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
