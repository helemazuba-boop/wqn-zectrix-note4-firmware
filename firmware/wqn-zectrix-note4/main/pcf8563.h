#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace wqn {

constexpr uint8_t kPcf8563Addr = 0x51;

bool Pcf8563InitWithBus(i2c_master_bus_handle_t bus);

bool Pcf8563ReadTime(int* year, int* month, int* day, int* hour, int* min, int* sec);

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
