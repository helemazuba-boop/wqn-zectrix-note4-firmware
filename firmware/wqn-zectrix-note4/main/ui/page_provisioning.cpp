#include "ui_internal.h"
#include "ui_fonts.h"
#include "ui/assets/brand_logo.h"
#include "provision_qr.h"

#include <algorithm>
#include <string>

namespace device_ui_internal {
namespace {

const lv_font_t* ProvisionFont(int size)
{
    if (size == 12) return &wqn_ui_sans_12;
    if (size == 24) return &wqn_ui_sans_24;
    if (size == 32) return &wqn_ui_latin_32;
    return wqn::PrimaryUiFont();
}

void ProvisionText(int x, int baseline, std::string value, int size = 16,
                   int width = kProvisionWidth, bool right = false,
                   const lv_font_t* face = nullptr)
{
    const lv_font_t* font = face ? face : ProvisionFont(size);
    std::replace_if(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }, ' ');
    if (wqn::MeasureTextWithFont(font, value.c_str()) > width) {
        while (!value.empty() && wqn::MeasureTextWithFont(font, (value + "...").c_str()) > width) {
            size_t last = value.size() - 1;
            while (last && (static_cast<unsigned char>(value[last]) & 0xc0) == 0x80) --last;
            value.resize(last);
        }
        value += "...";
    }
    const int left = right ? x - wqn::MeasureTextWithFont(font, value.c_str()) : x;
    wqn::DrawTextWithFont(left, baseline - font->line_height + font->base_line,
        font, value.c_str(), true);
}

void ProvisionIdentity(const char* status)
{
    DrawWqnBitmapAsset(kProvisionLeft, kProvisionBrandY, kWLogo, true);
    ProvisionText(kProvisionLeft + 26, kProvisionBrandBaseline, "rongNoteBook", 24,
        kProvisionWidth - 26, false, &wqn_ui_latin_24);
    ProvisionText(kProvisionLeft, kProvisionModelBaseline, "Note4", 32);
    const int status_width = wqn::MeasureTextWithFont(ProvisionFont(12), status);
    FillRect(kProvisionRight - status_width - 11, kProvisionStatusBaseline - 6, 4, 4, true);
    ProvisionText(kProvisionRight, kProvisionStatusBaseline, status, 12, 130, true);
    DrawHorizontalLine(kProvisionLeft, kProvisionDividerY, kProvisionWidth);
}

bool DrawProvisionQr(int column_x, const std::string& payload)
{
    ProvisionQrCode qr;
    if (!qr.Encode(payload)) return false;
    const int modules = qr.size() + 2 * kProvisionQrQuietModules;
    const int scale = kProvisionQrSlotSize / modules;
    if (scale < 2) return false;
    const int width = modules * scale;
    const int left = column_x + (kProvisionQrColumnWidth - width) / 2;
    const int top = kProvisionQrSlotY + (kProvisionQrSlotSize - width) / 2;
    for (int y = 0; y < qr.size(); ++y) {
        for (int x = 0; x < qr.size(); ++x) {
            if (qr.module(x, y)) {
                FillRect(left + (x + kProvisionQrQuietModules) * scale,
                    top + (y + kProvisionQrQuietModules) * scale, scale, scale, true);
            }
        }
    }
    return true;
}

void ProvisionRow(const char* label, const char* value, int baseline)
{
    ProvisionText(kProvisionLeft, baseline, label, 16, 110);
    ProvisionText(kProvisionRight, baseline, value[0] ? value : "未配置", 16, 230, true);
    DrawHorizontalLine(kProvisionLeft, baseline + 9, kProvisionWidth);
}

void ProvisionWaiting(const std::string& ssid)
{
    ProvisionIdentity("等待配置");
    ProvisionText(kProvisionLeft, kProvisionQrLabelBaseline, "1  连接设备热点", 12,
        kProvisionQrColumnWidth);
    ProvisionText(kProvisionQrSecondX, kProvisionQrLabelBaseline, "2  打开配网页", 12,
        kProvisionQrColumnWidth);
    DrawVerticalLine(kProvisionQrDividerX, kProvisionQrDividerY, kProvisionQrDividerHeight);
    std::string wifi_payload;
    if (!BuildProvisionWifiPayload(ssid, &wifi_payload) || !DrawProvisionQr(kProvisionLeft, wifi_payload)) {
        ProvisionText(kProvisionLeft, 202, "请手动连接热点", 12, kProvisionQrColumnWidth);
    }
    if (!DrawProvisionQr(kProvisionQrSecondX, kProvisionPortalUrl)) {
        ProvisionText(kProvisionQrSecondX, 202, "请手动输入地址", 12, kProvisionQrColumnWidth);
    }
    const auto caption = [](int x, const std::string& text) {
        const int width = std::min(kProvisionQrColumnWidth,
            wqn::MeasureTextWithFont(ProvisionFont(12), text.c_str()));
        ProvisionText(x + (kProvisionQrColumnWidth - width) / 2,
            kProvisionQrCaptionBaseline, text, 12, kProvisionQrColumnWidth);
    };
    caption(kProvisionLeft, ssid);
    caption(kProvisionQrSecondX, "192.168.4.1");
}

void ProvisionProgress(const wqn::UiFrame& frame)
{
    const auto& network = frame.settings;
    using Phase = wqn::ProvisionState;
    const bool starting = frame.provisioning_phase == Phase::kStarting ||
        (frame.provisioning_phase == Phase::kScanning && frame.provisioning_ssid.empty());
    if (starting) {
        ProvisionIdentity("准备热点");
        ProvisionText(kProvisionLeft, kProvisionStateTitleBaseline, "正在启动设备热点", 24);
        ProvisionRow("设备热点", frame.provisioning_ssid.empty() ? "准备中" : frame.provisioning_ssid.c_str(),
            kProvisionStateRowBaseline);
        ProvisionText(kProvisionLeft, kProvisionStateNoteBaseline - kProvisionStateRowPitch,
            "热点就绪后，扫描二维码开始配置。", 12);
        return;
    }
    if (frame.provisioning_phase == Phase::kConnecting) {
        ProvisionIdentity("正在连接");
        ProvisionText(kProvisionLeft, kProvisionStateTitleBaseline, "正在连接 WiFi", 24);
        ProvisionRow("主网络", network.wifi_primary_ssid, kProvisionStateRowBaseline);
        ProvisionRow("备用网络", network.wifi_backup_ssid, kProvisionStateRowBaseline + kProvisionStateRowPitch);
        ProvisionText(kProvisionLeft, kProvisionStateNoteBaseline, "配置已保存，请等待设备连接结果。", 12);
        return;
    }
    if (frame.provisioning_phase == Phase::kConnected) {
        ProvisionIdentity("已连接");
        ProvisionText(kProvisionLeft, kProvisionStateTitleBaseline, "WiFi 已连接", 24);
        ProvisionRow("当前连接", network.wifi_active_ssid[0] ? network.wifi_active_ssid : "已连接",
            kProvisionStateRowBaseline);
        ProvisionRow("备用网络", network.wifi_backup_ssid, kProvisionStateRowBaseline + kProvisionStateRowPitch);
        ProvisionText(kProvisionLeft, kProvisionStateNoteBaseline, "网络已就绪，可以继续使用设备。", 12);
        return;
    }
    ProvisionIdentity("连接失败");
    ProvisionText(kProvisionLeft, kProvisionStateTitleBaseline, "未能连接 WiFi", 24);
    ProvisionRow("主网络", network.wifi_primary_ssid[0] ? "未连接" : "未配置", kProvisionStateRowBaseline);
    ProvisionRow("备用网络", network.wifi_backup_ssid[0] ? "未连接" : "未配置",
        kProvisionStateRowBaseline + kProvisionStateRowPitch);
    ProvisionText(kProvisionLeft, kProvisionStateNoteBaseline, "请检查 WiFi 名称和密码后重新配置。", 12);
}

}  // namespace

esp_err_t RenderProvisioningToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    // Pixels stay on the existing EPD owner; no storage, network or panel calls
    // are introduced. Only the actual portal hotspot is encoded for joining.
    wqn::ClearEpdFramebuffer(true);
    if (frame.provisioning_phase == wqn::ProvisionState::kScanning && !frame.provisioning_ssid.empty())
        ProvisionWaiting(frame.provisioning_ssid);
    else
        ProvisionProgress(frame);
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
