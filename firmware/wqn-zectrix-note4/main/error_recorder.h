// [dev-diag] Central recent-error ring (DEV_DIAGNOSTICS.md §5). Always
// compiled in: failure paths are rare and the whole module is ~0.8 KB of
// static storage, so release builds keep the capture even though the Kconfig
// flag hides the Dev menu rows that display it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wqn {

struct ErrorRecord {
    uint32_t seq = 0;       // 0 = never written; monotonically increasing
    int64_t unix_sec = 0;   // wall clock at record time (unsane before sync)
    int64_t uptime_ms = 0;  // monotonic uptime at record time
    char tag[12] = {};      // subsystem label, see DEV_DIAGNOSTICS.md §5
    char msg[64] = {};      // one-line cause (esp_err_to_name / server code)
};

constexpr size_t kErrorRecordDepth = 8;

// Task-context only (no ISR caller today). Never allocates, never blocks on a
// mutex (short portMUX critical section), never logs — safe to call from any
// failure path, including before the services start.
void RecordError(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Copies up to `cap` most-recent records into `out`, oldest first. Returns the
// number copied. UI task only; snapshots the ring for the Dev error dialog.
size_t CopyRecentErrors(ErrorRecord* out, size_t cap);

}  // namespace wqn
