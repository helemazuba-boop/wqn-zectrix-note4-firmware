#pragma once

#include <algorithm>
#include <string>

#include "display_service.h"
#include "ui_fonts.h"

namespace device_ui_internal {

inline const lv_font_t* AiNativeFont(int size = 16)
{
    if (size == 12) return &wqn_ui_sans_12;
    if (size == 20) return &wqn_ui_sans_20;
    if (size == 24) return &wqn_ui_sans_24;
    return wqn::PrimaryUiFont();
}

inline std::string AiNativeClip(std::string text, int width, const lv_font_t* font)
{
    if (width <= 0) return {};
    std::replace_if(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }, ' ');
    if (wqn::MeasureTextWithFont(font, text.c_str()) <= width) return text;
    if (wqn::MeasureTextWithFont(font, "...") > width) return {};
    while (!text.empty() && wqn::MeasureTextWithFont(font, (text + "...").c_str()) > width) {
        size_t end = text.size() - 1;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
    }
    return text + "...";
}

inline void AiNativeText(int x, int y, int width, const std::string& text,
                         int size = 16, bool ink = true)
{
    const lv_font_t* font = AiNativeFont(size);
    const std::string clipped = AiNativeClip(text, width, font);
    if (!clipped.empty()) wqn::DrawTextWithFont(x, y, font, clipped.c_str(), ink);
}

}  // namespace device_ui_internal
