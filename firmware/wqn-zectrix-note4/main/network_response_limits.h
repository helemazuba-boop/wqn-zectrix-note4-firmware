#pragma once

#include <cstddef>

namespace wqn::network_limits {

// Buffered JSON only. Pack/image bodies use their own streaming sinks. Pages
// contain <=100 manifest rows; allow their bounded titles/URLs and JSON escapes.
inline constexpr size_t kMaxBufferedJsonBytes = 512U * 1024U;
// session.ready is a small control object, independent of the SSE frame budget.
inline constexpr size_t kMaxSessionControlBytes = 4U * 1024U;

}  // namespace wqn::network_limits
