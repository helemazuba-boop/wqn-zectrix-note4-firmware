#pragma once

#include <algorithm>
#include <cstddef>

namespace wqn {
inline constexpr size_t kTodoListVisibleRows = 4;

// Retain the window until selection crosses an edge, in either direction.
inline size_t ClampTodoListWindow(size_t start, size_t selected, size_t count)
{
    if (count <= kTodoListVisibleRows) return 0;
    selected = std::min(selected, count - 1);
    start = std::min(start, count - kTodoListVisibleRows);
    if (selected < start) start = selected;
    if (selected >= start + kTodoListVisibleRows) start = selected + 1 - kTodoListVisibleRows;
    return start;
}
}  // namespace wqn
