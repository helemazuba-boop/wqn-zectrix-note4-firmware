#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "power/sleep_protocol.h"

namespace wqn::services {

enum class ConnectivityState : uint8_t {
    kOff,
    kProvisioning,
    kConnecting,
    kOnline,
    kBackoff,
    kQuiescing,
};

struct ConnectivitySnapshot {
    ConnectivityState state = ConnectivityState::kOff;
    bool online = false;
    int rssi = 0;
    // SSID of the slot currently connected or being attempted ("" when unknown,
    // e.g. compile-time developer credentials).
    char active_ssid[33] = {};
    // SSID of the other stored credential ("" when there is no second slot).
    char backup_ssid[33] = {};
    // True when a second stored credential is available for failover.
    bool has_backup = false;
};

esp_err_t StartConnectivity();
esp_err_t StartConnectivityWithCredentials(const char* ssid, const char* password);
esp_err_t WaitForConnectivity(TickType_t timeout);
bool IsConnectivityOnline();
int GetConnectivityRssi();
void SetConnectivityProvisioning();
ConnectivitySnapshot GetConnectivitySnapshot();

esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand& command);
void RollbackConnectivityAfterSleepAbort();

}  // namespace wqn::services
