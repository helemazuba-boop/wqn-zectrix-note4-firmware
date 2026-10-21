// Shared clipped and centered text rendering wrappers.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include "display_service.h"

namespace device_ui_internal {

std::string LimitForEpd(const std::string& text)
{
    return wqn::TruncateUtf8TextToWidth(text, kTextWidth);
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

}  // namespace device_ui_internal
