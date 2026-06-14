#include "power_manager.h"

#include <algorithm>

#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "epd_display.h"
#include "pcf8563.h"
#include "sdkconfig.h"

#ifndef CONFIG_WQN_EPD_IDLE_POWER_OFF_MS
#define CONFIG_WQN_EPD_IDLE_POWER_OFF_MS 1500
#endif

#ifndef CONFIG_WQN_DEEP_SLEEP_IDLE_MS
#define CONFIG_WQN_DEEP_SLEEP_IDLE_MS 300000
#endif

#ifndef CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS
#define CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS 300000
#endif

#ifndef CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC
#define CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC 0
#endif

#ifndef CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV
#define CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV 3450
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

constexpr gpio_num_t kChargeDetect = GPIO_NUM_2;
constexpr gpio_num_t kChargeFull = GPIO_NUM_1;

constexpr gpio_num_t kBatAdc = GPIO_NUM_4;
constexpr adc_channel_t kBatAdcChannel = ADC_CHANNEL_3;
constexpr int kBatAdcAtten = ADC_ATTEN_DB_12;
constexpr int kBatAdcBitwidth = ADC_BITWIDTH_12;
constexpr int kBatAdcSamples = 10;

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

adc_oneshot_unit_handle_t g_adc_handle = nullptr;
adc_cali_handle_t g_adc_cali_handle = nullptr;
bool g_adc_initialized = false;
bool g_pcf8563_initialized = false;

i2c_master_bus_handle_t g_i2c_bus = nullptr;

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
    if (IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered()) {
        ESP_LOGW(kTag, "battery critically low during user activity, initiating shutdown");
        ShutdownForBatteryDepleted();
    }
}

void NoteEpdActivity()
{
    g_last_epd_activity_ms = NowMs();
    g_epd_idle_cut = false;
}

bool IsUiIdleForSleep()
{
    return IsUiIdleForSleepEx(0);
}

bool IsUiIdleForSleepEx(int extra_idle_ms)
{
    int threshold_ms = CONFIG_WQN_DEEP_SLEEP_IDLE_MS;
    if (IsCharging()) {
        threshold_ms += CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS;
    }
    threshold_ms += extra_idle_ms;

    const int64_t now_ms = NowMs();
    const int64_t last_activity_ms = std::max(g_last_user_activity_ms, g_last_epd_activity_ms);
    return last_activity_ms > 0 && (now_ms - last_activity_ms) >= threshold_ms;
}

esp_err_t InitPowerHardware(i2c_port_t i2c_port, gpio_num_t i2c_sda, gpio_num_t i2c_scl, int i2c_clk_hz)
{
    gpio_config_t charge_cfg = {};
    charge_cfg.pin_bit_mask = (1ULL << kChargeDetect) | (1ULL << kChargeFull);
    charge_cfg.mode = GPIO_MODE_INPUT;
    charge_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    charge_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    charge_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&charge_cfg), kTag, "configure charge detect pins");

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &g_adc_handle), kTag, "init ADC1 unit");

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = static_cast<adc_bitwidth_t>(kBatAdcBitwidth);
    chan_cfg.atten = static_cast<adc_atten_t>(kBatAdcAtten);
    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(g_adc_handle, kBatAdcChannel, &chan_cfg),
        kTag,
        "config ADC channel %d", kBatAdcChannel);

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = static_cast<adc_atten_t>(kBatAdcAtten);
    cali_cfg.bitwidth = static_cast<adc_bitwidth_t>(kBatAdcBitwidth);
    const esp_err_t cali_ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_adc_cali_handle);
    if (cali_ret == ESP_OK) {
        ESP_LOGI(kTag, "ADC calibration (curve fitting) enabled");
    } else {
        ESP_LOGW(kTag, "ADC calibration unavailable: %s; using raw read", esp_err_to_name(cali_ret));
        g_adc_cali_handle = nullptr;
    }

    g_adc_initialized = true;
    ESP_LOGI(kTag, "ADC initialized: channel=%d atten=%d bits=%d samples=%d",
             kBatAdcChannel, kBatAdcAtten, kBatAdcBitwidth, kBatAdcSamples);

    if (g_i2c_bus == nullptr) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = i2c_port;
        bus_cfg.sda_io_num = i2c_sda;
        bus_cfg.scl_io_num = i2c_scl;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = 1;
        const esp_err_t bus_err = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);
        if (bus_err != ESP_OK) {
            ESP_LOGE(kTag, "failed to create shared I2C bus: %s", esp_err_to_name(bus_err));
            return bus_err;
        }
        ESP_LOGI(kTag, "shared I2C bus created: port=%d SDA=%d SCL=%d clk=%d",
                 static_cast<int>(i2c_port), static_cast<int>(i2c_sda), static_cast<int>(i2c_scl), i2c_clk_hz);
    }

    if (Pcf8563InitWithBus(g_i2c_bus)) {
        g_pcf8563_initialized = true;
        ESP_LOGI(kTag, "PCF8563 initialized on shared I2C bus");
    } else {
        ESP_LOGW(kTag, "PCF8563 init failed; RTC timer wake will use ESP32 internal timer");
    }

    return ESP_OK;
}

i2c_master_bus_handle_t GetSharedI2cBusHandle()
{
    return g_i2c_bus;
}

adc_oneshot_unit_handle_t GetSharedAdcHandle()
{
    return g_adc_handle;
}

adc_cali_handle_t GetSharedAdcCaliHandle()
{
    return g_adc_cali_handle;
}

uint16_t GetBatteryVoltageMv()
{
    if (!g_adc_initialized) {
        return 0;
    }

    int sum_mv = 0;
    int valid_samples = 0;

    for (int i = 0; i < kBatAdcSamples; ++i) {
        int raw = 0;
        const esp_err_t ret = adc_oneshot_read(g_adc_handle, kBatAdcChannel, &raw);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "ADC read sample %d failed: %s", i, esp_err_to_name(ret));
            continue;
        }

        int mv = 0;
        if (g_adc_cali_handle != nullptr) {
            adc_cali_raw_to_voltage(g_adc_cali_handle, raw, &mv);
        } else {
            const int max_raw = (1 << kBatAdcBitwidth) - 1;
            const int max_voltage_mv = 3100;
            mv = (raw * max_voltage_mv) / max_raw;
        }

        sum_mv += mv * 2;
        ++valid_samples;

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (valid_samples == 0) {
        return 0;
    }

    return static_cast<uint16_t>(sum_mv / valid_samples);
}

int GetBatteryPercent()
{
    const uint16_t mv = GetBatteryVoltageMv();
    if (mv == 0) {
        return 0;
    }

    const int64_t mv_sq = static_cast<int64_t>(mv) * static_cast<int64_t>(mv);
    const int64_t raw = (-mv_sq + 9016LL * mv - 19189000LL) / 10000LL;

    int percent = static_cast<int>(raw);
    percent = std::max(0, percent);
    percent = std::min(100, percent);

    return percent;
}

bool IsCharging()
{
    return gpio_get_level(kChargeDetect) == 0;
}

bool IsUsbPowered()
{
    return IsCharging() || IsFullyCharged();
}

bool IsFullyCharged()
{
    return gpio_get_level(kChargeFull) == 1;
}

bool IsBatteryLow()
{
    return GetBatteryVoltageMv() <= CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV;
}

bool IsBatteryVeryLow()
{
    constexpr int kVeryLowMv = 3430;
    return GetBatteryVoltageMv() <= kVeryLowMv;
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
    if (g_pcf8563_initialized) {
        if (Pcf8563ConfigureTimerWake(CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC)) {
            ESP_LOGI(kTag, "deep sleep using PCF8563 timer wake: %d sec", CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC);
        } else {
            ESP_LOGW(kTag, "PCF8563 timer config failed, falling back to ESP32 internal timer");
            ESP_RETURN_ON_ERROR(
                esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC) * 1000000ULL),
                kTag,
                "enable ESP32 timer wakeup");
        }
    } else
#endif
    {
#if CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC > 0
        ESP_RETURN_ON_ERROR(
            esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC) * 1000000ULL),
            kTag,
            "enable ESP32 timer wakeup");
#endif
    }

    ESP_LOGI(
        kTag,
        "deep sleep armed: wake_gpio_mask=0x%llx timer_sec=%d pcf8563=%s",
        static_cast<unsigned long long>(wake_mask),
        CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC,
        g_pcf8563_initialized ? "yes" : "no");
    return ESP_OK;
}

void EnterDeepSleepIfEnabled()
{
#if CONFIG_WQN_DEEP_SLEEP_ENABLE
    if (IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered()) {
        ShutdownForBatteryDepleted();
        return;
    }

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
