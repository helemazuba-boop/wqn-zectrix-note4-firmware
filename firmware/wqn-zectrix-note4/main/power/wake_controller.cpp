#include "power/wake_controller.h"

#include <atomic>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8563.h"
#include "runtime/wake_context.h"
#include "sdkconfig.h"

#ifndef CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC
#define CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC 0
#endif

namespace {

constexpr char kTag[] = "wqn_wake";
constexpr gpio_num_t kConfirmWake = GPIO_NUM_0;
constexpr gpio_num_t kDownPowerWake = GPIO_NUM_18;
constexpr gpio_num_t kRtcIntWake = GPIO_NUM_5;
constexpr gpio_num_t kChargeDetectWake = GPIO_NUM_2;
constexpr gpio_num_t kChargeFullWake = GPIO_NUM_1;
constexpr uint64_t kUserInputWakeMask =
    (1ULL << kConfirmWake) | (1ULL << kDownPowerWake);
constexpr uint64_t kRtcWakeMask = 1ULL << kRtcIntWake;
constexpr uint64_t kExternalPowerWakeMask =
    (1ULL << kChargeDetectWake) | (1ULL << kChargeFullWake);
constexpr uint64_t kWakeMask =
    kUserInputWakeMask | kRtcWakeMask | kExternalPowerWakeMask;

std::atomic<bool> g_pcf8563_available{false};

uint64_t ActiveLowWakePins()
{
    uint64_t active_mask = 0;
    constexpr gpio_num_t pins[] = {
        kConfirmWake,
        kDownPowerWake,
        kRtcIntWake,
        kChargeDetectWake,
        kChargeFullWake,
    };
    for (gpio_num_t pin : pins) {
        if (gpio_get_level(pin) == 0) {
            active_mask |= 1ULL << pin;
        }
    }
    return active_mask;
}

bool DeadlineExpired(int64_t deadline_us)
{
    return deadline_us > 0 && esp_timer_get_time() >= deadline_us;
}

}  // namespace

namespace wqn::power {

void SetPcf8563WakeAvailable(bool available)
{
    g_pcf8563_available.store(available, std::memory_order_release);
}

void CaptureWakeContext()
{
    Pcf8563InterruptFlags flags;
    const bool valid = g_pcf8563_available.load(std::memory_order_acquire) &&
        Pcf8563ReadInterruptFlags(&flags);
    runtime::CaptureWakeContext(
        kUserInputWakeMask,
        kRtcWakeMask,
        valid,
        valid && flags.alarm,
        valid && flags.timer);
}

WakeArmResult ArmWakeSources(bool enable_timer_wakeup, int64_t deadline_us)
{
    WakeArmResult result;
    result.wake_gpio_mask = kWakeMask;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    const bool pcf_available = g_pcf8563_available.load(std::memory_order_acquire);
    if (pcf_available && !Pcf8563DisableTimerWakeAndClearFlags()) {
        ESP_LOGE(kTag, "cannot clear PCF8563 AF/TF before wake assembly");
        result.error = ESP_FAIL;
        return result;
    }

    // AF/TF are cleared before sampling the active-low line. A button pressed
    // during this window is also an ordinary, fully rolled-back sleep denial.
    vTaskDelay(pdMS_TO_TICKS(2));
    if (DeadlineExpired(deadline_us)) {
        result.error = ESP_ERR_TIMEOUT;
        return result;
    }
    result.active_gpio_mask = ActiveLowWakePins();
    if (result.active_gpio_mask != 0) {
        ESP_LOGW(kTag, "wake line active before arm: mask=0x%llx",
                 static_cast<unsigned long long>(result.active_gpio_mask));
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }

#if CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC > 0
    if (enable_timer_wakeup) {
        if (pcf_available &&
            Pcf8563ConfigureTimerWake(CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC)) {
            result.timer_source = TimerWakeSource::kPcf8563;
        } else {
            if (pcf_available && !Pcf8563DisableTimerWakeAndClearFlags()) {
                ESP_LOGE(kTag, "cannot clear PCF8563 after timer arm failure");
                result.error = ESP_FAIL;
                return result;
            }
            const esp_err_t timer_result = esp_sleep_enable_timer_wakeup(
                static_cast<uint64_t>(CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC) * 1000000ULL);
            if (timer_result != ESP_OK) {
                result.error = timer_result;
                return result;
            }
            result.timer_source = TimerWakeSource::kEsp32;
            ESP_LOGW(kTag, "PCF8563 timer unavailable; using ESP32 timer wake");
        }
    }
#else
    (void)enable_timer_wakeup;
#endif

    // Arming the PCF timer is itself fallible. Verify that it did not assert
    // GPIO5 before the ESP32 EXT1 source is assembled.
    vTaskDelay(pdMS_TO_TICKS(2));
    if (DeadlineExpired(deadline_us)) {
        DisarmWakeSources();
        result.error = ESP_ERR_TIMEOUT;
        return result;
    }
    result.active_gpio_mask = ActiveLowWakePins();
    if (result.active_gpio_mask != 0) {
        ESP_LOGW(kTag, "wake line active after timer arm: mask=0x%llx",
                 static_cast<unsigned long long>(result.active_gpio_mask));
        DisarmWakeSources();
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }

    result.error = esp_sleep_enable_ext1_wakeup(kWakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (result.error != ESP_OK) {
        DisarmWakeSources();
        return result;
    }
    ESP_LOGI(kTag, "wake sources armed: gpio_mask=0x%llx timer_source=%s timer_sec=%d",
             static_cast<unsigned long long>(kWakeMask),
             TimerWakeSourceName(result.timer_source),
             CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC);
    return result;
}

void DisarmWakeSources()
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (g_pcf8563_available.load(std::memory_order_acquire) &&
        !Pcf8563DisableTimerWakeAndClearFlags()) {
        ESP_LOGW(kTag, "PCF8563 wake flags could not be cleared during rollback");
    }
}

const char* TimerWakeSourceName(TimerWakeSource source)
{
    switch (source) {
        case TimerWakeSource::kDisabled:
            return "disabled";
        case TimerWakeSource::kPcf8563:
            return "pcf8563";
        case TimerWakeSource::kEsp32:
            return "esp32";
        default:
            return "unknown";
    }
}

}  // namespace wqn::power
