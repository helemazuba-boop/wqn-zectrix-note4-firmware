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
// Build time and an RTC derived from it never authorize new learning facts.
// A previously SNTP-calibrated, continuity-checked RTC supports offline use.
inline bool IsCalibratedLearningTime(WallClockSnapshot clock, int64_t seconds)
{
    return clock.network_calibrated() &&
        (clock.source != WallClockSource::kRtcSntp || clock.rtc_continuity) &&
        seconds >= 1704067200 &&
        seconds < 4102444800LL; // PCF8563: through 2099, UTC.
}
bool IsLearningTimeReady();
void NoteBuildTimeApplied();
// Require the applied system clock to agree with the callback (within 2 s).
void NoteSntpTimeSynchronized(int64_t unix_seconds, int64_t applied_unix_seconds);
// Boot-only, after validated RTC I/O and successful system clock application.
void NoteRtcTimeRestored(bool network_calibrated, uint32_t epoch, int64_t unix_seconds);

}  // namespace wqn::runtime
