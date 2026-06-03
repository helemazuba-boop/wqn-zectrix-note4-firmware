#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace wqn {

esp_err_t StartWifiStationIfEnabled();
esp_err_t WaitForWifiStationConnected(TickType_t timeout);
bool IsWifiStationConnected();

}  // namespace wqn
