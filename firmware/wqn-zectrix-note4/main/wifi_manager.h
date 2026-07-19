#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "power/sleep_protocol.h"

namespace wqn {

enum class WifiStationEvent : uint8_t {
    kStarted,
    kConnected,
    kDisconnected,
};

using WifiStationEventSink = void (*)(WifiStationEvent event, int reason, int rssi);

esp_err_t StartWifiStationIfEnabled();
esp_err_t StartWifiWithCredentials(const char* ssid, const char* password);
esp_err_t ConnectWifiStationNow();
esp_err_t StopWifiStationRadio();
esp_err_t StartWifiStationRadio();
esp_err_t WaitForWifiStationConnected(TickType_t timeout);
bool IsWifiStationConnected();
bool IsWifiStationInitialized();
void SetWifiStationEventSink(WifiStationEventSink sink);

// Returns the connected AP's RSSI (dBm, e.g. -65), or 0 if not connected/WiFi disabled.
int GetWifiRssi();

esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand& command);
void RollbackConnectivityAfterSleepAbort();

}  // namespace wqn
