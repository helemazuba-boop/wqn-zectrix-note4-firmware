#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

namespace wqn {

inline constexpr i2c_port_t kNote4I2cPort = I2C_NUM_0;
inline constexpr gpio_num_t kNote4I2cSda = GPIO_NUM_47;
inline constexpr gpio_num_t kNote4I2cScl = GPIO_NUM_48;
inline constexpr int kNote4I2cClockHz = 100000;

esp_err_t InitZectrixNote4SafePins();

}  // namespace wqn
