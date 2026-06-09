#include "power_manager.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "epd_display.h"
#include "sdkconfig.h"

#include <algorithm>

#ifndef CONFIG_WQN_EPD_IDLE_POWER_OFF_MS
#define CONFIG_WQN_EPD_IDLE_POWER_OFF_MS 0
#endif

#ifndef CONFIG_WQN_DEEP_SLEEP_IDLE_MS
#define CONFIG_WQN_DEEP_SLEEP_IDLE_MS 300000
#endif

#ifndef CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC
#define CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC 0
#endif

namespace {

constexpr char kTag[] = "wqn_power";

constexpr gpio_num_t kConfirmWake = GPIO_NUM_0;
constexpr gpio_num_t kDownPowerWake = GPIO_NUM_18;
constexpr gpio_num_t kRtcIntWake = GPIO_NUM_5;
constexpr gpio_num_t kBoardPowerLatch = GPIO_NUM_17;
constexpr gpio_num_t kEpdPower = GPIO_NUM_6;
constexpr gpio_num_t kAudioPower = GPIO_NUM_42;
constexpr gpio_num_t kAudioAmp = GPIO_NUM_46;
constexpr gpio_num_t kNfcPower = GPIO_NUM_21;
constexpr gpio_num_t kLed = GPIO_NUM_3;

int64_t NowMs()
{
    return esp_timer_get_time() / 1000;
}

const char* WakeupCauseName(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return "undefined";
        case ESP_SLEEP_WAKEUP_EXT0:
            return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:
            return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:
            return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:
            return "gpio";
        case ESP_SLEEP_WAKEUP_UART:
            return "uart";
        case ESP_SLEEP_WAKEUP_WIFI:
            return "wifi";
        case ESP_SLEEP_WAKEUP_COCPU:
            return "cocpu";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
            return "cocpu_trap";
        case ESP_SLEEP_WAKEUP_BT:
            return "bt";
        default:
            return "other";
    }
}

void HoldOutput(gpio_num_t pin, int level)
{
    gpio_hold_dis(pin);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

int64_t g_last_user_activity_ms = 0;
int64_t g_last_epd_activity_ms = 0;
bool g_epd_idle_cut = false;

}  // namespace

namespace wqn {

void LogWakeupCause()
{
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(kTag, "wakeup cause: %s (%d)", WakeupCauseName(cause), static_cast<int>(cause));
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(kTag, "ext1 wakeup status mask=0x%llx", static_cast<unsigned long long>(esp_sleep_get_ext1_wakeup_status()));
    }
}

void ReleaseDeepSleepHolds()
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(kEpdPower);
    gpio_hold_dis(kAudioPower);
    gpio_hold_dis(kAudioAmp);
    gpio_hold_dis(kNfcPower);
    gpio_hold_dis(kLed);
    gpio_hold_dis(kBoardPowerLatch);
}

void NoteUserActivity()
{
    g_last_user_activity_ms = NowMs();
}

void NoteEpdActivity()
{
    g_last_epd_activity_ms = NowMs();
    g_epd_idle_cut = false;
}

bool IsUiIdleForSleep()
{
    const int64_t now_ms = NowMs();
    const int64_t last_activity_ms = std::max(g_last_user_activity_ms, g_last_epd_activity_ms);
    return last_activity_ms > 0 && (now_ms - last_activity_ms) >= CONFIG_WQN_DEEP_SLEEP_IDLE_MS;
}

esp_err_t PrepareForDeepSleep()
{
    ESP_LOGI(kTag, "preparing board for deep sleep");

    PowerOffEpd();
    HoldOutput(kEpdPower, 0);
    HoldOutput(kAudioPower, 0);
    HoldOutput(kAudioAmp, 0);
    HoldOutput(kNfcPower, 0);
    HoldOutput(kLed, 1);
    HoldOutput(kBoardPowerLatch, 1);
    gpio_deep_sleep_hold_en();

    const uint64_t wake_mask =
        (1ULL << kConfirmWake) |
        (1ULL << kDownPowerWake) |
        (1ULL << kRtcIntWake);
    ESP_RETURN_ON_ERROR(
        esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW),
        kTag,
        "enable ext1 wakeup");

#if CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC > 0
    ESP_RETURN_ON_ERROR(
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC) * 1000000ULL),
        kTag,
        "enable timer wakeup");
#endif

    ESP_LOGI(
        kTag,
        "deep sleep armed: wake_gpio_mask=0x%llx timer_sec=%d",
        static_cast<unsigned long long>(wake_mask),
        CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC);
    return ESP_OK;
}

void EnterDeepSleepIfEnabled()
{
#if CONFIG_WQN_DEEP_SLEEP_ENABLE
    if (!IsUiIdleForSleep()) {
        return;
    }
    const esp_err_t result = PrepareForDeepSleep();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "deep sleep aborted: %s", esp_err_to_name(result));
        return;
    }
    ESP_LOGI(kTag, "entering deep sleep");
    esp_deep_sleep_start();
#endif
}

void ShutdownForBatteryDepleted()
{
    ESP_LOGW(kTag, "Battery depleted, shutting down");

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    PowerOffEpd();
    HoldOutput(kEpdPower, 0);
    HoldOutput(kAudioPower, 0);
    HoldOutput(kAudioAmp, 0);
    HoldOutput(kNfcPower, 0);
    HoldOutput(kLed, 1);

    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(kBoardPowerLatch);
    gpio_set_level(kBoardPowerLatch, 0);

    ESP_LOGI(kTag, "entering depleted-battery deep sleep with PWR_ON latch released");
    esp_deep_sleep_start();
}

void PowerOffEpdAfterIdleIfNeeded()
{
    const int idle_ms = CONFIG_WQN_EPD_IDLE_POWER_OFF_MS;
    if (idle_ms <= 0 || g_epd_idle_cut || g_last_epd_activity_ms == 0) {
        return;
    }
    if ((NowMs() - g_last_epd_activity_ms) < idle_ms) {
        return;
    }

    ESP_LOGI(kTag, "EPD idle power-off after %d ms", idle_ms);
    PowerOffEpd();
    g_epd_idle_cut = true;
}

}  // namespace wqn
