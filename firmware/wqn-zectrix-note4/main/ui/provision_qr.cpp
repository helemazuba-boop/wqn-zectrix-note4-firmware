#include "provision_qr.h"

namespace device_ui_internal {

bool ProvisionQrCode::Encode(const std::string& payload)
{
    valid_ = false;
    if (payload.empty() || payload.find('\0') != std::string::npos) {
        return false;
    }
    std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(kProvisionQrMaxVersion)> scratch{};
    valid_ = qrcodegen_encodeText(payload.c_str(), scratch.data(), bits_.data(),
        qrcodegen_Ecc_MEDIUM, kProvisionQrMinVersion, kProvisionQrMaxVersion,
        qrcodegen_Mask_AUTO, false);
    return valid_;
}

bool BuildProvisionWifiPayload(const std::string& ssid, std::string* payload)
{
    if (payload == nullptr) {
        return false;
    }
    payload->clear();
    if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != std::string::npos) {
        return false;
    }
    payload->reserve(82);  // Prefix + at most 32 escaped bytes + terminators.
    payload->assign("WIFI:T:nopass;S:");
    for (const char c : ssid) {
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"' || c == '\'') {
            payload->push_back('\\');
        }
        payload->push_back(c);
    }
    payload->append(";;");
    return true;
}

}  // namespace device_ui_internal
