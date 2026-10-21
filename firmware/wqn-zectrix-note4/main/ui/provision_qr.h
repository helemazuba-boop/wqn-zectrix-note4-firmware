#pragma once

#include <array>
#include <string>

#include "qrcodegen.h"

namespace device_ui_internal {

inline constexpr char kProvisionPortalUrl[] = "http://192.168.4.1/";
inline constexpr int kProvisionQrMinVersion = 3;
inline constexpr int kProvisionQrMaxVersion = 6;
inline constexpr int kProvisionQrQuietModules = 4;

// Pure encoder, independent of ESP-IDF and the display. Each instance owns one
// bounded matrix; Encode uses a second bounded scratch buffer on its caller.
class ProvisionQrCode {
public:
    bool Encode(const std::string& payload);
    int size() const { return valid_ ? qrcodegen_getSize(bits_.data()) : 0; }
    bool module(int x, int y) const
    {
        return valid_ && qrcodegen_getModule(bits_.data(), x, y);
    }

private:
    std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(kProvisionQrMaxVersion)> bits_{};
    bool valid_ = false;
};

// Open provisioning hotspot only; no saved STA passwords enter the QR payload.
// SSIDs are bounded in bytes and special characters use WiFi QR escaping.
bool BuildProvisionWifiPayload(const std::string& ssid, std::string* payload);

}  // namespace device_ui_internal
