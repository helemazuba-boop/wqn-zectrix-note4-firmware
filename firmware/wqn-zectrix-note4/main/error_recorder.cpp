#include "error_recorder.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace wqn {
namespace {

// Same discipline as the sync-snapshot publication (GetSyncSnapshot): a
// dedicated portMUX spinlock, whole-struct copies inside the critical
// section, formatting done outside on the caller's stack.
portMUX_TYPE g_error_ring_lock = portMUX_INITIALIZER_UNLOCKED;
ErrorRecord g_error_ring[kErrorRecordDepth];
uint32_t g_error_seq = 0;  // guarded by g_error_ring_lock

}  // namespace

void RecordError(const char* tag, const char* fmt, ...)
{
    if (tag == nullptr || fmt == nullptr) {
        return;
    }
    ErrorRecord record = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(record.msg, sizeof(record.msg), fmt, args);
    va_end(args);
    std::strncpy(record.tag, tag, sizeof(record.tag) - 1);
    record.unix_sec = time(nullptr);
    record.uptime_ms = esp_timer_get_time() / 1000;

    taskENTER_CRITICAL(&g_error_ring_lock);
    const uint32_t seq = g_error_seq + 1;
    record.seq = seq;
    g_error_ring[seq % kErrorRecordDepth] = record;
    g_error_seq = seq;
    taskEXIT_CRITICAL(&g_error_ring_lock);
}

size_t CopyRecentErrors(ErrorRecord* out, size_t cap)
{
    if (out == nullptr || cap == 0) {
        return 0;
    }
    ErrorRecord ring[kErrorRecordDepth];
    uint32_t seq = 0;
    taskENTER_CRITICAL(&g_error_ring_lock);
    seq = g_error_seq;
    for (ErrorRecord& slot : g_error_ring) {
        ring[&slot - g_error_ring] = slot;
    }
    taskEXIT_CRITICAL(&g_error_ring_lock);

    // Record with seq s lives at slot s % depth. Emit min(seq, depth, cap)
    // newest records, oldest first.
    const size_t count =
        std::min({static_cast<size_t>(seq), kErrorRecordDepth, cap});
    const uint32_t oldest = seq + 1 - static_cast<uint32_t>(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = ring[(oldest + i) % kErrorRecordDepth];
    }
    return count;
}

}  // namespace wqn
