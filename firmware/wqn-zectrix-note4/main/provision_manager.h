#pragma once

#include <functional>
#include <string>

#include "esp_err.h"

namespace wqn {

enum class ProvisionState : uint8_t {
    kIdle,
    kStarting,
    kScanning,
    kConnecting,
    kConnected,
    kFailed,
};

#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
using ProvisionDoneCallback = std::function<void(const std::string& ssid, const std::string& password)>;
#else
struct ProvisionDoneCallback {};
#endif

esp_err_t StartProvisioningMode();
esp_err_t StopProvisioningMode();
ProvisionState GetProvisioningState();
bool IsProvisioningActive();
std::string GetProvisioningApSsid();
const char* ProvisionStateLabel(ProvisionState state);
void SetProvisionDoneCallback(ProvisionDoneCallback callback);

}
