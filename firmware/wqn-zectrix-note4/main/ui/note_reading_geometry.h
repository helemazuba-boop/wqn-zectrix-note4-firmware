#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "ui_layout.h"

namespace device_ui_internal {
struct NoteReadingGeometry {
    int top;
    uint32_t visible;
    uint32_t max_scroll;
    uint32_t step;
};

inline NoteReadingGeometry ComputeNoteReadingGeometry(size_t rows, bool images, bool notice)
{
    const int top = kNoteBodyTop + (images ? kNoteBodyLineHeight : 0) +
        (notice ? kNoteBodyLineHeight : 0);
    const auto visible = static_cast<uint32_t>(std::max(1,
        (kNoteBodyBottom - top) / kNoteBodyLineHeight));
    return {top, visible, rows > visible ? static_cast<uint32_t>(rows - visible) : 0,
        std::max<uint32_t>(1, visible - 1)};
}
}  // namespace device_ui_internal
