#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace wqn {

esp_err_t StartWifiStationIfEnabled();
esp_err_t StartWifiWithCredentials(const char* ssid, const char* password);
esp_err_t WaitForWifiStationConnected(TickType_t timeout);
bool IsWifiStationConnected();

// Returns the connected AP's RSSI (dBm, e.g. -65), or 0 if not connected/WiFi disabled.
int GetWifiRssi();

}  // namespace wqn
