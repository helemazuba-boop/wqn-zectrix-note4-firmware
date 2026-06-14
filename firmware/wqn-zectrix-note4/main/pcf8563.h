#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace wqn {

constexpr uint8_t kPcf8563Addr = 0x51;

bool Pcf8563Init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, int clk_hz);

bool Pcf8563InitWithBus(i2c_master_bus_handle_t bus);

bool Pcf8563ReadTime(int* year, int* month, int* day, int* hour, int* min, int* sec);

bool Pcf8563ConfigureTimerWake(uint8_t seconds);

}  // namespace wqn
