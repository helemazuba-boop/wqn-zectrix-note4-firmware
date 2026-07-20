#pragma once

#include <cstdint>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace wqn {

void LogWakeupCause();
void NoteUserActivity();
bool IsUiIdleForSleep();
bool IsUiIdleForSleepEx(int extra_idle_ms);

esp_err_t StartPowerCoordinator();
// Synchronizes USB/charger status with the global sleep policy. Call once
// after InitSleepCoordinator(), then periodically from PowerCoordinator.
void RefreshUsbPowerSleepPolicy();
void SetDeepSleepTimerWakePreference(bool enabled);
void ShutdownForBatteryDepleted();

esp_err_t InitPowerHardware(i2c_port_t i2c_port, gpio_num_t i2c_sda, gpio_num_t i2c_scl, int i2c_clk_hz);

i2c_master_bus_handle_t GetSharedI2cBusHandle();

struct PowerStatusSnapshot {
    bool valid = false;
    int adc_raw = 0;
    int adc_mv = 0;
    int battery_mv = 0;
    int battery_percent = 0;
    bool charging = false;
    bool fully_charged = false;
};

// Returns a value snapshot; callers never receive ADC/GPIO driver handles.
bool ReadPowerStatus(PowerStatusSnapshot* snapshot);

int GetBatteryPercent();
uint16_t GetBatteryVoltageMv();
bool IsCharging();
bool IsFullyCharged();
// True only while the ESP32-S3 USB Serial/JTAG peripheral is receiving host
// SOF frames. This distinguishes a connected PC from charger-status GPIOs.
bool IsUsbHostConnected();
bool IsUsbPowered();
bool IsBatteryLow();
bool IsBatteryVeryLow();

}  // namespace wqn
