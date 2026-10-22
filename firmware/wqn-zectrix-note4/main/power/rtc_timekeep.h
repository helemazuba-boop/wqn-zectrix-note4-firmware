#pragma once

#include <cstdint>

namespace wqn::power::timekeep {

// UTC calendar fields using the same conventions as pcf8563.h: `year` is
// years since 1900 (valid 100..199 => 2000..2099), `month` is 0-based and
// `weekday` is Sunday-based (0..6).
struct RtcCalendar {
    int year = 100;
    int month = 0;
    int day = 1;
    int hour = 0;
    int min = 0;
    int sec = 0;
    int weekday = 0;
};

// Pure UTC conversions between Unix seconds and RtcCalendar. No hardware and
// no libc timezone state is touched, so the contract fixture self-test can
// exercise them on-device. CalendarFromUnixSeconds additionally rejects dates
// outside the PCF8563 century window (2000..2099). UnixSecondsFromCalendar
// validates register-level ranges only; it does not reject impossible
// day-of-month values such as February 30th, mirroring Pcf8563WriteTime.
bool CalendarFromUnixSeconds(int64_t seconds, RtcCalendar* out);
bool UnixSecondsFromCalendar(const RtcCalendar& calendar, int64_t* seconds_out);

// Sleep-side hook: writes the current system wall clock into the PCF8563 and
// stamps an internal cross-deep-sleep trust record. Designed to be called by
// PowerCoordinator after every service has quiesced and before wake-source
// assembly. Every attempt revokes old trust before validation/I/O; only a
// verified write publishes trust for this generation. Failure never prevents
// the sleep transaction from proceeding.
bool PersistSystemTimeToRtc(uint32_t sleep_generation);
// Shares the prepare transaction's absolute esp_timer deadline with every
// write/verify step. Revokes old trust even if already expired. Mandatory
// STOP cleanup may outlive this work budget; failure remains nonfatal to sleep.
bool PersistSystemTimeToRtc(uint32_t sleep_generation, int64_t deadline_us);

// Boot-side hook: on a trusted deep-sleep resume (ext1/timer resume reason,
// deep-sleep reset reason, valid trust record matching the committed sleep
// snapshot generation, PCF8563 VL clear), reads the
// RTC clock back and applies it via settimeofday() so downstream consumers
// (UI seeding, sync deadlines, outbox stamps, HTTPS clock gating) resume from
// the clock written before sleep. Trust here proves write/generation integrity,
// not network calibration: a reasonable build-time seed can also be persisted.
// Returns true only when the system clock was actually applied.
bool RestoreSystemTimeFromRtc();

// Establishes the process timezone and, only when neither the platform nor
// RestoreSystemTimeFromRtc supplied a reasonable wall clock, seeds a
// fallback from the firmware build timestamp. This is approximate, not proof
// of network calibration. Call before any scheduler performs wall-clock
// admission; later SNTP calibration may refine it.
bool SeedSystemTimeFromBuildTimeIfNeeded();

}  // namespace wqn::power::timekeep
