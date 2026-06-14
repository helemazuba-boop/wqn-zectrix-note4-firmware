// Text rendering wrappers + UTF-8 page slicing + AI call summary joining.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <vector>

#include "epd_display.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr int kEpdTextWidth = wqn::kEpdWidth - 12;

std::string LimitForEpd(const std::string& text)
{
    return wqn::TruncateUtf8TextToWidth(text, kEpdTextWidth);
}

esp_err_t DrawClippedText(int x, int y, int max_width, const std::string& text, bool black)
{
    const std::string clipped = wqn::TruncateUtf8TextToWidth(text, max_width);
    return wqn::DrawUtf8Text(x, y, clipped.c_str(), black);
}

esp_err_t DrawCenteredText(int x, int y, int width, const std::string& text, bool black)
{
    const std::string clipped = wqn::TruncateUtf8TextToWidth(text, width - 4);
    const int text_width = wqn::MeasureUtf8TextWidth(clipped.c_str());
    const int text_x = x + std::max(2, (width - text_width) / 2);
    return wqn::DrawUtf8Text(text_x, y, clipped.c_str(), black);
}

esp_err_t DrawWrappedText(int x, int y, int width, const std::string& text, int max_lines, bool black)
{
    const std::vector<std::string> lines = wqn::WrapUtf8TextToWidth(text, width, max_lines);
    int line_y = y;
    for (const std::string& line : lines) {
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, line_y, line.c_str(), black), kTag, "draw wrapped text");
        line_y += 18;
    }
    return ESP_OK;
}

std::string Utf8PageSlice(const std::string& text, size_t page, size_t chars_per_page)
{
    if (chars_per_page == 0) {
        return text;
    }
    const size_t start_char = page * chars_per_page;
    const size_t end_char = start_char + chars_per_page;
    size_t char_index = 0;
    size_t start_byte = text.size();
    size_t end_byte = text.size();
    for (size_t i = 0; i < text.size();) {
        if (char_index == start_char) {
            start_byte = i;
        }
        if (char_index == end_char) {
            end_byte = i;
            break;
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        if ((c & 0x80) == 0) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        if (i + step > text.size()) {
            step = 1;
        }
        i += step;
        ++char_index;
    }
    if (start_char >= char_index && start_byte == text.size()) {
        return "";
    }
    if (start_byte == text.size() && start_char == 0) {
        start_byte = 0;
    }
    if (end_byte < start_byte) {
        end_byte = text.size();
    }
    return text.substr(start_byte, end_byte - start_byte);
}

std::string JoinAiFunctionCallSummaries(const std::vector<std::string>& summaries)
{
    std::string output;
    for (const std::string& summary : summaries) {
        if (summary.empty()) {
            continue;
        }
        if (!output.empty()) {
            output += "\n";
        }
        output += summary;
    }
    return output;
}

}  // namespace device_ui_internal
