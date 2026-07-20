// Shared bitmap digits plus WiFi/battery status rendering.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

#include "display_service.h"
#include "font_zectrix.h"
#include "ui/assets/font_wqn_digits_16_1.h"
#include "ui/assets/font_wqn_digits_48_1.h"
#include "ui/assets/font_wqn_ui_16_1.h"

namespace device_ui_internal {

namespace {

using DigitLookup = const WqnBitmapAsset* (*)(char);

int MeasureBitmapDigits(const std::string& text, DigitLookup lookup)
{
    int width = 0;
    int glyphs = 0;
    for (char c : text) {
        const WqnBitmapAsset* asset = lookup(c);
        if (asset == nullptr) {
            continue;
        }
        width += asset->width;
        ++glyphs;
    }
    return width + std::max(0, glyphs - 1);
}

void DrawBitmapDigits(int x, int y, const std::string& text, DigitLookup lookup, bool black)
{
    bool first = true;
    for (char c : text) {
        const WqnBitmapAsset* asset = lookup(c);
        if (asset == nullptr) {
            continue;
        }
        if (!first) {
            ++x;
        }
        DrawWqnBitmapAsset(x, y, *asset, black);
        x += asset->width;
        first = false;
    }
}

void DrawBitmapDigitsCentered(
    int x, int y, int width, const std::string& text, DigitLookup lookup, bool black)
{
    const int text_width = MeasureBitmapDigits(text, lookup);
    DrawBitmapDigits(x + std::max(0, (width - text_width) / 2), y, text, lookup, black);
}

const WqnBitmapAsset& BatteryIconAsset(const wqn::HomeSummary& home)
{
    if (home.charging) return b05_battery_charging_16_asset;
    if (home.full || home.battery_percent >= 95) return b04_battery_full_16_asset;
    if (home.battery_percent >= 75) return b03_battery_75_16_asset;
    if (home.battery_percent >= 50) return b02_battery_50_16_asset;
    if (home.battery_percent >= 25) return b01_battery_25_16_asset;
    return b00_battery_empty_16_asset;
}

}  // namespace

void DrawStandbyClockDigits(int y, const std::string& text)
{
    DrawBitmapDigitsCentered(0, y, wqn::kEpdWidth, text, WqnDigit48, true);
}

void DrawConfigDigitsCentered(int x, int y, int width, const std::string& ascii, bool black)
{
    DrawBitmapDigitsCentered(x, y, width, ascii, WqnDigit16, black);
}

void DrawTimerDigitsArt(int y, const std::string& ascii_duration)
{
    DrawBitmapDigitsCentered(0, y, wqn::kEpdWidth, ascii_duration, WqnDigit48, true);
}

int DrawWifiStatusIcon(int right_edge, int y, const wqn::HomeSummary& home)
{
    // WiFi icon (3-level signal when connected, slash when not).
    const char* wifi;
    if (!home.wifi_connected) {
        wifi = FONT_ZECTRIX_WIFI_SLASH;
    } else if (home.wifi_rssi >= -55) {
        wifi = FONT_ZECTRIX_WIFI_FULL;
    } else if (home.wifi_rssi >= -70) {
        wifi = FONT_ZECTRIX_WIFI_FAIR;
    } else {
        wifi = FONT_ZECTRIX_WIFI_WEAK;
    }
    const int wifi_w = wqn::MeasureTextWithFont(&font_zectrix_16_1, wifi);
    const int x = right_edge - wifi_w;
    wqn::DrawTextWithFont(x, y, &font_zectrix_16_1, wifi, true);
    return x;
}

int DrawStatusBarIcons(int right_edge, int y, const wqn::HomeSummary& home)
{
    int x = right_edge;
    const WqnBitmapAsset& battery = BatteryIconAsset(home);
    x -= battery.width;
    DrawWqnBitmapAsset(x, y, battery, true);
    x -= 6;
    return DrawWifiStatusIcon(x, y, home);
}

}  // namespace device_ui_internal
