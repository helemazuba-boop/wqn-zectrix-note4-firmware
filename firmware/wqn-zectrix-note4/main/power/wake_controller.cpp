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
constexpr uint64_t kChargeFullWakeMask = 1ULL << kChargeFullWake;
constexpr uint64_t kExternalPowerWakeMask =
    (1ULL << kChargeDetectWake) | kChargeFullWakeMask;
constexpr uint64_t kWakeMask =
    kUserInputWakeMask | kRtcWakeMask | kExternalPowerWakeMask;
constexpr uint32_t kStuckWakeLineDenialThreshold = 3;

constexpr uint64_t WakeMaskForChargeFullStatus(bool charge_full_asserted)
{
    return charge_full_asserted
        ? kWakeMask & ~kChargeFullWakeMask
        : kWakeMask;
}

static_assert(
    (WakeMaskForChargeFullStatus(true) & kChargeFullWakeMask) == 0);
static_assert(
    (WakeMaskForChargeFullStatus(false) & kChargeFullWakeMask) != 0);

std::atomic<bool> g_pcf8563_available{false};
uint64_t g_last_active_wake_line_mask = 0;
uint32_t g_repeated_active_wake_line_denials = 0;

void RecordActiveWakeLineDenial(uint64_t active_mask, const char* phase)
{
    if (active_mask == g_last_active_wake_line_mask) {
        ++g_repeated_active_wake_line_denials;
    } else {
        g_last_active_wake_line_mask = active_mask;
        g_repeated_active_wake_line_denials = 1;
    }
    ESP_LOGW(
        kTag,
        "wake line active %s arm: mask=0x%llx repeated=%u",
        phase,
        static_cast<unsigned long long>(active_mask),
        static_cast<unsigned>(g_repeated_active_wake_line_denials));
    if (g_repeated_active_wake_line_denials ==
        kStuckWakeLineDenialThreshold) {
        ESP_LOGE(
            kTag,
            "wake line appears stuck across %u sleep attempts: mask=0x%llx; deep sleep remains denied",
            static_cast<unsigned>(kStuckWakeLineDenialThreshold),
            static_cast<unsigned long long>(active_mask));
    }
}

void ResetActiveWakeLineDenialTracking()
{
    g_last_active_wake_line_mask = 0;
    g_repeated_active_wake_line_denials = 0;
}

uint64_t ActiveLowWakePins(uint64_t wake_mask)
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
        const uint64_t pin_mask = 1ULL << pin;
        if ((wake_mask & pin_mask) != 0 && gpio_get_level(pin) == 0) {
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

WakeArmResult ArmWakeSources(uint32_t timer_wakeup_seconds, int64_t deadline_us)
{
    WakeArmResult result;
    const bool charge_full_asserted = gpio_get_level(kChargeFullWake) == 0;
    result.wake_gpio_mask = WakeMaskForChargeFullStatus(charge_full_asserted);
    // /STDBY can remain low after USB removal on Note4. EXT1 ANY_LOW cannot
    // arm an already-low line, and the charge-complete signal is not needed
    // to detect charger insertion: CHRG_L remains armed for that purpose.
    // Preserve the polarity and omit only this asserted status line for the
    // current sleep transaction.
    if (charge_full_asserted) {
        ESP_LOGW(
            kTag,
            "/STDBY already asserted; omitted GPIO1 from this EXT1 wake mask");
    }
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
    result.active_gpio_mask = ActiveLowWakePins(result.wake_gpio_mask);
    if (result.active_gpio_mask != 0) {
        RecordActiveWakeLineDenial(result.active_gpio_mask, "before");
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }

    if (timer_wakeup_seconds > 0) {
        if (pcf_available && timer_wakeup_seconds <= UINT8_MAX &&
            Pcf8563ConfigureTimerWake(
                static_cast<uint8_t>(timer_wakeup_seconds))) {
            result.timer_source = TimerWakeSource::kPcf8563;
        } else {
            if (pcf_available && !Pcf8563DisableTimerWakeAndClearFlags()) {
                ESP_LOGE(kTag, "cannot clear PCF8563 after timer arm failure");
                result.error = ESP_FAIL;
                return result;
            }
            const esp_err_t timer_result = esp_sleep_enable_timer_wakeup(
                static_cast<uint64_t>(timer_wakeup_seconds) * 1000000ULL);
            if (timer_result != ESP_OK) {
                result.error = timer_result;
                return result;
            }
            result.timer_source = TimerWakeSource::kEsp32;
            ESP_LOGI(
                kTag,
                "using ESP32 timer wake: seconds=%u",
                static_cast<unsigned>(timer_wakeup_seconds));
        }
    }

    // Arming the PCF timer is itself fallible. Verify that it did not assert
    // GPIO5 before the ESP32 EXT1 source is assembled.
    vTaskDelay(pdMS_TO_TICKS(2));
    if (DeadlineExpired(deadline_us)) {
        DisarmWakeSources();
        result.error = ESP_ERR_TIMEOUT;
        return result;
    }
    result.active_gpio_mask = ActiveLowWakePins(result.wake_gpio_mask);
    if (result.active_gpio_mask != 0) {
        RecordActiveWakeLineDenial(result.active_gpio_mask, "after-timer");
        DisarmWakeSources();
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }

    result.error = esp_sleep_enable_ext1_wakeup(
        result.wake_gpio_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (result.error != ESP_OK) {
        DisarmWakeSources();
        return result;
    }
    ResetActiveWakeLineDenialTracking();
    ESP_LOGI(kTag, "wake sources armed: gpio_mask=0x%llx timer_source=%s timer_sec=%u",
             static_cast<unsigned long long>(result.wake_gpio_mask),
             TimerWakeSourceName(result.timer_source),
             static_cast<unsigned>(timer_wakeup_seconds));
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
