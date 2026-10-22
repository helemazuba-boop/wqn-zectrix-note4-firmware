#pragma once

#include <cstdint>

namespace wqn::runtime {

enum class WallClockSource : uint8_t {
    kUnknown, kBuildTime, kSntp, kRtcApproximate, kRtcSntp,
};

// One atomic word publishes source, epoch and RTC continuity together. SNTP
// means a calibration event was observed, not an authenticated time source.
struct WallClockSnapshot {
    WallClockSource source = WallClockSource::kUnknown;
    uint32_t epoch = 0;
    bool rtc_continuity = false;

    bool network_calibrated() const
    {
        return source == WallClockSource::kSntp || source == WallClockSource::kRtcSntp;
    }
};

constexpr uint32_t kWallClockEpochMax = (1U << 28) - 1;
WallClockSnapshot GetWallClockSnapshot();
void NoteBuildTimeApplied();
// Require the applied system clock to agree with the callback (within 2 s).
void NoteSntpTimeSynchronized(int64_t unix_seconds, int64_t applied_unix_seconds);
// Boot-only, after validated RTC I/O and successful system clock application.
void NoteRtcTimeRestored(bool network_calibrated, uint32_t epoch, int64_t unix_seconds);

}  // namespace wqn::runtime
