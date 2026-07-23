// Battery voltage reading, low-battery protection, and label formatting.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "power_manager.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
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

    wqn::PowerStatusSnapshot snapshot;
    if (!wqn::ReadPowerStatus(&snapshot)) {
        return false;
    }
    reading->raw = snapshot.adc_raw;
    reading->adc_mv = snapshot.adc_mv;
    reading->battery_mv = snapshot.battery_mv;
    reading->percent = snapshot.battery_percent;
    reading->charging = snapshot.charging;
    reading->full = snapshot.fully_charged;
    reading->chrg_l = reading->charging ? 0 : 1;
    reading->stdby_h = reading->full ? 0 : 1;
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
