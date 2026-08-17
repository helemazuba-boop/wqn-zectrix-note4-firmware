#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "power/sleep_protocol.h"

namespace wqn {

enum class WifiStationEvent : uint8_t {
    kStarted,
    kAssociated,
    kGotIp,
    kDisconnected,
};

using WifiStationEventSink = void (*)(WifiStationEvent event, int reason, int rssi);

esp_err_t StartWifiStationIfEnabled();
esp_err_t StartWifiWithCredentials(const char* ssid, const char* password);
esp_err_t ConnectWifiStationNow();
esp_err_t DisconnectWifiStationNow();
esp_err_t StopWifiStationRadio();
esp_err_t StartWifiStationRadio();
esp_err_t WaitForWifiStationConnected(TickType_t timeout);
bool IsWifiStationConnected();
bool IsWifiStationInitialized();
void SetWifiStationEventSink(WifiStationEventSink sink);

// True for disconnect reasons that mean the credential/AP itself is unusable
// (auth failure, AP not found) rather than transient signal loss. The
// connectivity service uses this to pivot to a backup credential immediately
// instead of burning fast retries on a dead slot.
bool IsWifiCredentialFailureReason(int reason);

// Returns the connected AP's RSSI (dBm, e.g. -65), or 0 if not connected/WiFi disabled.
int GetWifiRssi();

esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand& command);
void RollbackConnectivityAfterSleepAbort();

}  // namespace wqn
