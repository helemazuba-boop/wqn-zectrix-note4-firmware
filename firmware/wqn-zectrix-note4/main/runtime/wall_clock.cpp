#include "runtime/wall_clock.h"

#include <atomic>

namespace wqn::runtime {
namespace {

constexpr uint32_t kRtcContinuityBit = 8;
std::atomic<uint32_t> g_wall_clock_state{0};

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
    PublishNewEpoch(WallClockSource::kSntp);
}

void NoteRtcTimeRestored(bool network_calibrated, uint32_t epoch)
{
    if (epoch > kWallClockEpochMax) return;
    const auto source = network_calibrated
        ? WallClockSource::kRtcSntp : WallClockSource::kRtcApproximate;
    g_wall_clock_state.store(
        (epoch << 4) | kRtcContinuityBit | static_cast<uint32_t>(source),
        std::memory_order_release);
}

}  // namespace wqn::runtime
