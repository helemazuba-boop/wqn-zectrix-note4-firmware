// Seven-segment digit rendering for the standby clock and timer pages.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>

#include "epd_display.h"

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

void DrawStandbyClockDigits(int y, const std::string& text)
{
    static constexpr int kScale = 10;
    static constexpr int kGap = 8;
    static constexpr int kColonWidth = 20;
    const int digit_width = SevenSegmentDigitWidth(kScale);
    const int total_width = digit_width * 4 + kColonWidth + kGap * 4;
    int x = (wqn::kEpdWidth - total_width) / 2;
    int digit_index = 0;

    for (char c : text) {
        if (c == ':') {
            const int dot = 8;
            const int colon_x = x + (kColonWidth - dot) / 2;
            FillRect(colon_x, y + 32, dot, dot, true);
            FillRect(colon_x, y + 68, dot, dot, true);
            x += kColonWidth + kGap;
            continue;
        }
        if (c >= '0' && c <= '9') {
            DrawSevenSegmentDigit(x, y, kScale, c);
        } else {
            FillRect(x + 2, y + 44, digit_width - 4, 8, true);
        }
        x += digit_width;
        ++digit_index;
        x += kGap;
    }
}

}  // namespace device_ui_internal
