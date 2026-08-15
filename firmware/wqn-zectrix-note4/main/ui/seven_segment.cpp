// Shared bitmap digits plus WiFi/battery status rendering.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

#include "display_service.h"
#include "font_zectrix.h"
#include "ui/assets/font_wqn_digits_16_1.h"
#include "ui/assets/font_wqn_digits_48_1.h"

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

const char* BatteryIconGlyph(const wqn::HomeSummary& home)
{
    if (home.charging) {
        return FONT_ZECTRIX_BATTERY_CHARGING;
    }
    if (home.full || home.battery_percent >= 80) {
        return FONT_ZECTRIX_BATTERY_FULL;
    }
    if (home.battery_percent >= 60) {
        return FONT_ZECTRIX_BATTERY_75;
    }
    if (home.battery_percent >= 40) {
        return FONT_ZECTRIX_BATTERY_50;
    }
    if (home.battery_percent >= 20) {
        return FONT_ZECTRIX_BATTERY_25;
    }
    return FONT_ZECTRIX_BATTERY_EMPTY;
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

int DrawBatteryStatusIcon(int right_edge, int y, const wqn::HomeSummary& home)
{
    const char* battery = BatteryIconGlyph(home);
    const int battery_w = wqn::MeasureTextWithFont(&font_zectrix_16_1, battery);
    const int x = right_edge - battery_w;
    wqn::DrawTextWithFont(x, y, &font_zectrix_16_1, battery, true);
    return x;
}

int DrawStatusBarIcons(int right_edge, int y, const wqn::HomeSummary& home)
{
    int x = right_edge;
    x = DrawBatteryStatusIcon(x, y, home);
    x -= 6;
    return DrawWifiStatusIcon(x, y, home);
}

}  // namespace device_ui_internal
