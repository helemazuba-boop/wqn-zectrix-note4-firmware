// Seven-segment digit rendering for the standby clock and timer pages.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

#include "epd_display.h"
#include "font_zectrix.h"

namespace device_ui_internal {

int SevenSegmentDigitWidth(int scale)
{
    return 6 * scale;
}

int SevenSegmentDigitHeight(int scale)
{
    return 10 * scale;
}

int SevenSegmentTextWidth(const std::string& text, int scale)
{
    int width = 0;
    for (char c : text) {
        width += c == ':' ? 2 * scale : SevenSegmentDigitWidth(scale);
        width += scale;
    }
    return std::max(0, width - scale);
}

void DrawSevenSegmentDigit(int x, int y, int scale, char digit)
{
    static constexpr uint8_t kSegments[10] = {
        0b1111110,  // 0
        0b0110000,  // 1
        0b1101101,  // 2
        0b1111001,  // 3
        0b0110011,  // 4
        0b1011011,  // 5
        0b1011111,  // 6
        0b1110000,  // 7
        0b1111111,  // 8
        0b1111011,  // 9
    };
    if (digit < '0' || digit > '9') {
        return;
    }
    const uint8_t segments = kSegments[digit - '0'];
    const int width = SevenSegmentDigitWidth(scale);
    const int height = SevenSegmentDigitHeight(scale);
    const int t = std::max(2, scale);
    if (segments & 0b1000000) {
        DrawSegment(x + t, y, width - 2 * t, t);
    }
    if (segments & 0b0100000) {
        DrawSegment(x + width - t, y + t, t, height / 2 - t);
    }
    if (segments & 0b0010000) {
        DrawSegment(x + width - t, y + height / 2, t, height / 2 - t);
    }
    if (segments & 0b0001000) {
        DrawSegment(x + t, y + height - t, width - 2 * t, t);
    }
    if (segments & 0b0000100) {
        DrawSegment(x, y + height / 2, t, height / 2 - t);
    }
    if (segments & 0b0000010) {
        DrawSegment(x, y + t, t, height / 2 - t);
    }
    if (segments & 0b0000001) {
        DrawSegment(x + t, y + height / 2 - t / 2, width - 2 * t, t);
    }
}

void DrawSevenSegmentTextCentered(int y, const std::string& text, int scale)
{
    int x = std::max(0, (wqn::kEpdWidth - SevenSegmentTextWidth(text, scale)) / 2);
    for (char c : text) {
        if (c == ':') {
            const int dot = std::max(2, scale);
            const int colon_x = x + scale / 2;
            FillRect(colon_x, y + 3 * scale, dot, dot, true);
            FillRect(colon_x, y + 7 * scale, dot, dot, true);
            x += 3 * scale;
        } else {
            DrawSevenSegmentDigit(x, y, scale, c);
            x += SevenSegmentDigitWidth(scale) + scale;
        }
    }
}

std::string ZectrixArtText(const std::string& ascii)
{
    // Map ASCII digits/colon to font_zectrix artistic-glyph codepoints.
    std::string out;
    out.reserve(ascii.size() * 3);
    for (char c : ascii) {
        switch (c) {
            case '0': out += FONT_ZECTRIX_ICON_0; break;
            case '1': out += FONT_ZECTRIX_ICON_1; break;
            case '2': out += FONT_ZECTRIX_ICON_2; break;
            case '3': out += FONT_ZECTRIX_ICON_3; break;
            case '4': out += FONT_ZECTRIX_ICON_4; break;
            case '5': out += FONT_ZECTRIX_ICON_5; break;
            case '6': out += FONT_ZECTRIX_ICON_6; break;
            case '7': out += FONT_ZECTRIX_ICON_7; break;
            case '8': out += FONT_ZECTRIX_ICON_8; break;
            case '9': out += FONT_ZECTRIX_ICON_9; break;
            case ':': out += FONT_ZECTRIX_ICON_COLON; break;
            default: break;  // skip unknown chars
        }
    }
    return out;
}

void DrawStandbyClockDigits(int y, const std::string& text)
{
    const std::string zectrix_text = ZectrixArtText(text);
    const int total_width = wqn::MeasureTextWithFont(&font_zectrix_48_1, zectrix_text.c_str());
    const int x = (wqn::kEpdWidth - total_width) / 2;
    wqn::DrawTextWithFont(std::max(0, x), y, &font_zectrix_48_1, zectrix_text.c_str(), true);
}

void DrawTimerDigitsArt(int y, const std::string& ascii_duration)
{
    const std::string zectrix_text = ZectrixArtText(ascii_duration);
    wqn::DrawTextWithFontCentered(0, y, wqn::kEpdWidth, &font_zectrix_48_1, zectrix_text.c_str(), true);
}

const char* BatteryIconMacro(const wqn::HomeSummary& home)
{
    if (home.charging) return FONT_ZECTRIX_BATTERY_CHARGING;
    if (home.full || home.battery_percent >= 95) return FONT_ZECTRIX_BATTERY_FULL;
    if (home.battery_percent >= 75) return FONT_ZECTRIX_BATTERY_75;
    if (home.battery_percent >= 50) return FONT_ZECTRIX_BATTERY_50;
    if (home.battery_percent >= 25) return FONT_ZECTRIX_BATTERY_25;
    return FONT_ZECTRIX_BATTERY_EMPTY;
}

int DrawStatusBarIcons(int right_edge, int y, const wqn::HomeSummary& home)
{
    int x = right_edge;
    // Battery icon (rightmost).
    const char* bat = BatteryIconMacro(home);
    const int bat_w = wqn::MeasureTextWithFont(&font_zectrix_16_1, bat);
    x -= bat_w;
    wqn::DrawTextWithFont(x, y, &font_zectrix_16_1, bat, true);
    x -= 6;  // gap between battery and wifi
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
    x -= wifi_w;
    wqn::DrawTextWithFont(x, y, &font_zectrix_16_1, wifi, true);
    return x;  // x just left of the wifi icon
}

}  // namespace device_ui_internal
