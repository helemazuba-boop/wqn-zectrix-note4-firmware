#pragma once

#include <algorithm>
#include <cstdint>

namespace device_ui_internal {

inline int AiFloorDivide(int value, int divisor)
{
    return value < 0 ? -((-value + divisor - 1) / divisor) : value / divisor;
}

inline int AiCeilDivide(int value, int divisor)
{
    return -AiFloorDivide(-value, divisor);
}

// Keep a complete overlap row even when a block inset leaves partial rows at
// both viewport edges. Only Agent uses viewport paging; other tiers keep theirs.
inline int AiPageStepRows(int viewport_height, int line_height)
{
    return std::max(1, viewport_height / line_height - 2);
}

struct AiScrollGeometry {
    int max_window_top = 0;
    int min_scroll = 0;
    int max_scroll = 0;
    int scroll = 0;
    int window_top = 0;
};

inline AiScrollGeometry ComputeAiScrollGeometry(int total_height, int anchor,
                                               int viewport_height, int line_height,
                                               int32_t requested)
{
    AiScrollGeometry out;
    out.max_window_top = std::max(0, total_height - viewport_height);
    // Round outwards. Truncating a negative partial row towards zero strands
    // the last complete row; rounding the other edge inwards hides the first.
    out.min_scroll = std::min(0, AiFloorDivide(anchor - out.max_window_top, line_height));
    out.max_scroll = AiCeilDivide(anchor, line_height);
    out.scroll = std::clamp<int>(requested, out.min_scroll, out.max_scroll);
    out.window_top = std::clamp(anchor - out.scroll * line_height, 0, out.max_window_top);
    return out;
}

inline int AiOffsetForBlockTop(int anchor, int target, int line_height,
                               const AiScrollGeometry& geometry)
{
    // Land before the target, never a few pixels into its first text row.
    return std::clamp(AiCeilDivide(anchor - target, line_height),
                      geometry.min_scroll, geometry.max_scroll);
}

}  // namespace device_ui_internal
