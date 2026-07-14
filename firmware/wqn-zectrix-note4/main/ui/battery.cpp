// Battery voltage reading, low-battery protection, and label formatting.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "power_manager.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr gpio_num_t kChargeDetect = GPIO_NUM_2;            // CHRG_L: low means charging.
constexpr gpio_num_t kChargeFull = GPIO_NUM_1;              // /STDBY: low means full (active-low, open-drain). Matches power_manager.cpp IsFullyCharged().
constexpr int kBatteryShutdownPercent = 0;
constexpr int kBatteryShutdownMv = 3450;
constexpr int kBatteryShutdownDebounceCount = 3;
constexpr const char* kPmuStatusUnknown = "unknown";

bool ShouldLogBattery()
{
    static int64_t last_log_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (last_log_us == 0 || now_us - last_log_us >= 60000000LL) {
        last_log_us = now_us;
        return true;
    }
    return false;
}

bool ReadBatteryStatus(BatteryReading* reading)
{
    if (reading == nullptr) {
        return false;
    }

    adc_oneshot_unit_handle_t shared_adc = wqn::GetSharedAdcHandle();
    adc_cali_handle_t shared_cali = wqn::GetSharedAdcCaliHandle();
    if (shared_adc == nullptr) {
        return false;
    }

    int raw_sum = 0;
    int battery_voltage_sum = 0;
    for (int i = 0; i < 10; ++i) {
        int raw = 0;
        int raw_voltage = 0;
        if (adc_oneshot_read(shared_adc, ADC_CHANNEL_3, &raw) != ESP_OK) {
            return false;
        }
        if (shared_cali != nullptr) {
            adc_cali_raw_to_voltage(shared_cali, raw, &raw_voltage);
        } else {
            raw_voltage = (raw * 3100) / 4095;
        }
        raw_sum += raw;
        battery_voltage_sum += raw_voltage * 2;
    }

    reading->raw = raw_sum / 10;
    reading->adc_mv = battery_voltage_sum / 20;
    reading->battery_mv = battery_voltage_sum / 10;
    if (reading->battery_mv <= 0) {
        return false;
    }

    int computed_percent =
        (-1 * reading->battery_mv * reading->battery_mv + 9016 * reading->battery_mv - 19189000) / 10000;
    computed_percent = std::min(100, std::max(0, computed_percent));
    reading->percent = computed_percent;
    reading->chrg_l = gpio_get_level(kChargeDetect);
    reading->stdby_h = gpio_get_level(kChargeFull);
    reading->charging = reading->chrg_l == 0;
    // [charge-polarity-fix] /STDBY is active-low (TP4056 open-drain): driven
    // LOW when full/standby, floats HIGH otherwise. power_manager.cpp
    // IsFullyCharged() already reads == 0; this UI path was inverted (== 1),
    // which showed "满电" while charging/discharging and hid "满电" when actually
    // full. It also gated CheckLowBatteryProtection's depleted_candidate on
    // !full, so a discharging battery could never trip the shutdown debounce.
    reading->full = reading->stdby_h == 0;
    reading->power_present_or_status_known = reading->charging || reading->full;
    reading->pmu_status = kPmuStatusUnknown;
    reading->pmu_implemented = false;
    if (ShouldLogBattery()) {
        ESP_LOGI(
            kTag,
            "battery adc_raw=%d adc_mv=%d battery_mv=%d percent=%d chrg_l=%d stdby_h=%d charging=%d full=%d pmu_status=%s pmu_implemented=%d",
            reading->raw,
            reading->adc_mv,
            reading->battery_mv,
            reading->percent,
            reading->chrg_l,
            reading->stdby_h,
            reading->charging ? 1 : 0,
            reading->full ? 1 : 0,
            reading->pmu_status,
            reading->pmu_implemented ? 1 : 0);
    }
    return true;
}

std::string BatteryLabel(const BatteryReading& reading)
{
    if (reading.full) {
        return "满电";
    }
    if (reading.charging) {
        return "充电 " + std::to_string(reading.percent) + "%";
    }
    if (reading.percent <= kBatteryShutdownPercent) {
        return "低电";
    }
    return std::to_string(reading.percent) + "%";
}

void CheckLowBatteryProtection(const BatteryReading* reading)
{
    static int shutdown_count = 0;

    if (reading == nullptr || reading->battery_mv <= 0) {
        if (shutdown_count != 0) {
            ESP_LOGI(kTag, "Battery shutdown count reset: invalid reading depleted_candidate=0 shutdown_count=0");
        }
        shutdown_count = 0;
        ESP_LOGI(kTag, "battery protection skipped: invalid reading pmu_status=%s depleted_candidate=0 shutdown_count=%d",
                 kPmuStatusUnknown,
                 shutdown_count);
        return;
    }

    const bool depleted_candidate =
        reading->percent == kBatteryShutdownPercent &&
        reading->battery_mv <= kBatteryShutdownMv &&
        !reading->charging &&
        !reading->full;
    if (!depleted_candidate) {
        if (shutdown_count != 0) {
            ESP_LOGI(kTag, "Battery shutdown count reset: percent=%d battery_mv=%d charging=%d full=%d depleted_candidate=0 shutdown_count=0",
                     reading->percent,
                     reading->battery_mv,
                     reading->charging ? 1 : 0,
                     reading->full ? 1 : 0);
        }
        shutdown_count = 0;
        ESP_LOGI(kTag, "battery protection percent=%d battery_mv=%d charging=%d full=%d pmu_status=%s depleted_candidate=0 shutdown_count=%d",
                 reading->percent,
                 reading->battery_mv,
                 reading->charging ? 1 : 0,
                 reading->full ? 1 : 0,
                 reading->pmu_status,
                 shutdown_count);
        return;
    }

    ++shutdown_count;
    ESP_LOGW(kTag, "battery protection percent=%d battery_mv=%d charging=%d full=%d pmu_status=%s depleted_candidate=1 shutdown_count=%d/%d",
             reading->percent,
             reading->battery_mv,
             reading->charging ? 1 : 0,
             reading->full ? 1 : 0,
             reading->pmu_status,
             shutdown_count,
             kBatteryShutdownDebounceCount);
    if (shutdown_count >= kBatteryShutdownDebounceCount) {
        wqn::ShutdownForBatteryDepleted();
    }
}

void CheckBatteryProtection()
{
    BatteryReading battery = {};
    if (ReadBatteryStatus(&battery)) {
        CheckLowBatteryProtection(&battery);
    } else {
        CheckLowBatteryProtection(nullptr);
    }
}

}  // namespace device_ui_internal
