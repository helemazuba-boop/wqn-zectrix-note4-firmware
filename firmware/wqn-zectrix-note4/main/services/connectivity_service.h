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
