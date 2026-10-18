#pragma once

#include <algorithm>
#include <cstddef>

namespace wqn {

inline constexpr size_t kAgentPickerVisibleRows = 4;

// Keep an existing window still until selection actually crosses its edge.
// Selection itself stays session-id anchored by the Agent worker on reload.
inline size_t ClampAgentPickerWindow(size_t start, size_t selected, size_t count)
{
    if (count <= kAgentPickerVisibleRows) return 0;
    selected = std::min(selected, count - 1);
    start = std::min(start, count - kAgentPickerVisibleRows);
    if (selected < start) return selected;
    if (selected >= start + kAgentPickerVisibleRows)
        return selected + 1 - kAgentPickerVisibleRows;
    return start;
}

}  // namespace wqn
