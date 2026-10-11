#pragma once

#include <cstddef>
#include <cstdint>

namespace wqn {

// Counts include the in-flight prefix. A later keypress never refreshes the
// oldest timestamp. These are event limits (reveal and verdict both count),
// not an unbounded queue of five-question groups.
struct WordBatchPolicy {
    static constexpr size_t kFlushCount = 5;
    static constexpr size_t kCapacity = 10;
    static constexpr int64_t kMaxAgeMs = 30000;
    static constexpr int64_t kRetryMs = 1000;

    static bool CanAccept(size_t outstanding, bool failed) {
        return !failed && outstanding < kCapacity;
    }
    static bool FlushDue(size_t outstanding, size_t in_flight, int64_t oldest_ms,
                         int64_t now_ms, int64_t retry_after_ms, bool forced) {
        return outstanding != 0 && in_flight == 0 && now_ms >= retry_after_ms &&
            (forced || outstanding >= kFlushCount || now_ms - oldest_ms >= kMaxAgeMs);
    }
};

}  // namespace wqn
