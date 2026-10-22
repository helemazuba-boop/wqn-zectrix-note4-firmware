#include "runtime/wall_clock.h"

#include <atomic>

#include "esp_timer.h"

namespace wqn::runtime {
namespace {

constexpr uint32_t kRtcContinuityBit = 8;
std::atomic<uint32_t> g_wall_clock_state{0};
// Written at boot before services start; callbacks only read this RTC anchor.
std::atomic<int64_t> g_rtc_anchor_seconds{0};
std::atomic<int64_t> g_rtc_anchor_monotonic_us{0};

void PublishNewEpoch(WallClockSource source)
{
    uint32_t previous = g_wall_clock_state.load(std::memory_order_acquire);
    uint32_t next;
    do {
        const uint32_t epoch = ((previous >> 4) + 1) & kWallClockEpochMax;
        next = (epoch << 4) | static_cast<uint32_t>(source);
    } while (!g_wall_clock_state.compare_exchange_weak(
        previous, next, std::memory_order_acq_rel, std::memory_order_acquire));
}

}  // namespace

WallClockSnapshot GetWallClockSnapshot()
{
    const uint32_t value = g_wall_clock_state.load(std::memory_order_acquire);
    return {static_cast<WallClockSource>(value & 7), value >> 4,
            (value & kRtcContinuityBit) != 0};
}

void NoteBuildTimeApplied()
{
    PublishNewEpoch(WallClockSource::kBuildTime);
}

void NoteSntpTimeSynchronized(int64_t unix_seconds, int64_t applied_unix_seconds)
{
    if (unix_seconds < 1704067200 || applied_unix_seconds < 1704067200) return;
    const int64_t difference = unix_seconds >= applied_unix_seconds
        ? unix_seconds - applied_unix_seconds : applied_unix_seconds - unix_seconds;
    if (difference > 2) return;
    uint32_t previous = g_wall_clock_state.load(std::memory_order_acquire);
    if ((previous & kRtcContinuityBit) != 0) {
        const int64_t anchor = g_rtc_anchor_seconds.load(std::memory_order_relaxed);
        const int64_t since_us = esp_timer_get_time() -
            g_rtc_anchor_monotonic_us.load(std::memory_order_relaxed);
        const int64_t expected = anchor + (since_us > 0 ? since_us / 1000000 : 0);
        const int64_t correction = applied_unix_seconds >= expected
            ? applied_unix_seconds - expected : expected - applied_unix_seconds;
        if (anchor >= 1704067200 && correction <= 2) {
            // Matching SNTP refines provenance without discarding known sleep
            // time on every online boot. Only a changed clock basis needs a
            // new epoch and conservative restoration.
            const uint32_t next = (previous & ~uint32_t{7}) |
                static_cast<uint32_t>(WallClockSource::kSntp);
            if (g_wall_clock_state.compare_exchange_strong(
                    previous, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
        }
    }
    PublishNewEpoch(WallClockSource::kSntp);
}

void NoteRtcTimeRestored(bool network_calibrated, uint32_t epoch, int64_t unix_seconds)
{
    if (epoch > kWallClockEpochMax) return;
    g_rtc_anchor_seconds.store(unix_seconds, std::memory_order_relaxed);
    g_rtc_anchor_monotonic_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    const auto source = network_calibrated
        ? WallClockSource::kRtcSntp : WallClockSource::kRtcApproximate;
    g_wall_clock_state.store(
        (epoch << 4) | kRtcContinuityBit | static_cast<uint32_t>(source),
        std::memory_order_release);
}

}  // namespace wqn::runtime
