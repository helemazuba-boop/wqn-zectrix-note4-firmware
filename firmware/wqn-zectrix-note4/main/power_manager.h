#pragma once

#include <cstdint>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace wqn {

void LogWakeupCause();
void NoteUserActivity();
// [sleep-race] Publishes user activity with an explicit event timestamp, called
// by the button task BEFORE a critical (non-repeat) event enters the input
// ring. Publish-then-enqueue is the safe order: the sleep commit gate only
// observes g_activity_gate, so "event queued but generation stale" must be
// impossible, while "generation bumped but event dropped by the full-ring
// policy" merely over-cancels one sleep. An already-queued release/short-press
// (whose GPIO is no longer held low and thus cannot re-arm EXT1 wake) can
// therefore never be lost to sleep. Safe from the button task: it touches
// only the activity gate + atomics, never I2C/battery hardware.
void NoteUserActivityAtMs(int64_t occurred_at_ms);
// [sleep-race] UI-task battery guard formerly bundled into NoteUserActivity.
// Button-event consumption calls ONLY this (the activity generation was
// already published at production by NoteUserActivityAtMs; re-publishing at
// consume time would double-bump the generation and overwrite the event
// timestamp with the consume time). Touches I2C -- UI/power task only.
void CheckBatteryAfterUserActivity();
bool IsUiIdleForSleep();
bool IsUiIdleForSleepEx(int extra_idle_ms);
// True when an idle, battery-powered device is ready to enter deep sleep and
// a cosmetic minute refresh should yield instead of acquiring a display
// lease at the same threshold.
bool ShouldYieldClockRefreshToDeepSleep();

esp_err_t StartPowerCoordinator();
// Synchronizes USB/charger status with the global sleep policy. Call once
// after InitSleepCoordinator(), then periodically from PowerCoordinator.
void RefreshUsbPowerSleepPolicy();
void SetDeepSleepTimerWakePreference(bool enabled);
void ShutdownForBatteryDepleted();
// [power-fix] User-initiated power-off (settings page): the coordinator
// clears the panel on the EPD owner task, quiesces services, then cuts the
// board power latch. Safe to call from any task; re-request if busy.
void RequestUserPowerOff();

esp_err_t InitPowerHardware(i2c_port_t i2c_port, gpio_num_t i2c_sda, gpio_num_t i2c_scl, int i2c_clk_hz);

i2c_master_bus_handle_t GetSharedI2cBusHandle();

struct PowerStatusSnapshot {
    bool valid = false;
    int adc_raw = 0;
    int adc_mv = 0;
    int battery_mv = 0;
    int battery_percent = 0;
    bool usb_host_connected = false;
    bool charging = false;
    bool fully_charged = false;
    // Unified policy result: USB SOF or CHRG_L, never /STDBY alone.
    bool external_power_present = false;
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
// True when USB SOF or active-low CHRG_L confirms external power. /STDBY is a
// charge-complete status only and may remain asserted after cable removal, so
// it does not independently block sleep.
bool IsUsbPowered();
bool IsBatteryLow();
bool IsBatteryVeryLow();

}  // namespace wqn
