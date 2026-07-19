#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

namespace wqn {

void LogWakeupCause();
void ReleaseDeepSleepHolds();
void NoteUserActivity();
void NoteEpdActivity();
bool IsUiIdleForSleep();
bool IsUiIdleForSleepEx(int extra_idle_ms);

esp_err_t StartPowerCoordinator();
void SetDeepSleepTimerWakePreference(bool enabled);
void PowerOffEpdAfterIdleIfNeeded();
void ShutdownForBatteryDepleted();

esp_err_t InitPowerHardware(i2c_port_t i2c_port, gpio_num_t i2c_sda, gpio_num_t i2c_scl, int i2c_clk_hz);

i2c_master_bus_handle_t GetSharedI2cBusHandle();

adc_oneshot_unit_handle_t GetSharedAdcHandle();
adc_cali_handle_t GetSharedAdcCaliHandle();

int GetBatteryPercent();
uint16_t GetBatteryVoltageMv();
bool IsCharging();
bool IsFullyCharged();
bool IsUsbPowered();
bool IsBatteryLow();
bool IsBatteryVeryLow();

}  // namespace wqn
