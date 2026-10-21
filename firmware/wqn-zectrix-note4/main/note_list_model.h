#pragma once

#include <algorithm>
#include <cstddef>

namespace wqn {
inline constexpr size_t kNoteListVisibleRows = 4;

// Keep a stable four-row window until the focused item crosses its edge.
inline size_t ClampNoteListWindow(size_t start, size_t selected, size_t count)
{
    if (count <= kNoteListVisibleRows) return 0;
    selected = std::min(selected, count - 1);
    start = std::min(start, count - kNoteListVisibleRows);
    if (selected < start) start = selected;
    if (selected >= start + kNoteListVisibleRows) start = selected + 1 - kNoteListVisibleRows;
    return start;
}
}  // namespace wqn
