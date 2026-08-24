#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace wqn {

constexpr uint8_t kPcf8563Addr = 0x51;

bool Pcf8563InitWithBus(i2c_master_bus_handle_t bus);

bool Pcf8563ReadTime(int* year, int* month, int* day, int* hour, int* min, int* sec);

// Writes the wall clock into the RTC in one burst while the prescaler is
// stopped (Control_status_1 STOP bit), clearing the VL integrity flag via the
// seconds byte. Conventions mirror Pcf8563ReadTime: `year` is years since
// 1900, `month` is 0-based; `weekday` is 0-6 with Sunday = 0. The whole
// sequence runs under a single bus-lock hold so an audio transaction cannot
// interleave between STOP and the time registers.
bool Pcf8563WriteTime(int year, int month, int day, int hour, int min,
                      int sec, int weekday);

bool Pcf8563ConfigureTimerWake(uint8_t seconds);

struct Pcf8563InterruptFlags {
    bool alarm = false;
    bool timer = false;
};

// Reads AF/TF without clearing them so WakeController can classify the boot
// before the next sleep transaction clears and rearms the interrupt line.
bool Pcf8563ReadInterruptFlags(Pcf8563InterruptFlags* flags);

// Stops the countdown, disables its interrupt and clears AF/TF so GPIO5 can
// be validated before it is armed as an EXT1 wake source.
bool Pcf8563DisableTimerWakeAndClearFlags();

}  // namespace wqn
