#include "power/rtc_timekeep.h"

#include <cstddef>
#include <ctime>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "pcf8563.h"
#include "runtime/wake_context.h"

namespace {

constexpr char kTag[] = "wqn_rtc_time";

constexpr uint32_t kTrustMagic = 0x57514B54;  // "WQTK"
constexpr uint32_t kTrustVersion = 1;
// Matches the copies in ui_internal.h and wqn_api.cpp (2024-01-01 UTC).
constexpr int64_t kMinReasonableUnixTime = 1704067200;

// Written before every committed deep-sleep entry. RTC slow memory keeps the
// value across deep sleep but loses it on real power loss, so a valid record
// proves "this device wrote the PCF8563 clock on its way into the sleep that
// just ended" -- a factory chip ships with VL clear and a zeroed clock, which
// the read path alone cannot distinguish from a genuine persist.
struct TimekeepTrustRecord {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t checksum;
};

RTC_DATA_ATTR TimekeepTrustRecord g_trust_record = {};

uint32_t TrustChecksum(const TimekeepTrustRecord& record)
{
    constexpr uint32_t kFnvOffset = 2166136261U;
    constexpr uint32_t kFnvPrime = 16777619U;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t hash = kFnvOffset;
    for (size_t index = 0; index < offsetof(TimekeepTrustRecord, checksum); ++index) {
        hash = (hash ^ bytes[index]) * kFnvPrime;
    }
    return hash;
}

bool TrustRecordValid()
{
    return g_trust_record.magic == kTrustMagic &&
           g_trust_record.version == kTrustVersion &&
           g_trust_record.checksum == TrustChecksum(g_trust_record);
}

int64_t UtcDaysFromCivil(int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

void CivilFromUtcDays(int64_t days, int* year_out, unsigned* month_out,
                      unsigned* day_out)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t year = static_cast<int64_t>(year_of_era) + era * 400;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned mp = (5 * day_of_year + 2) / 153;
    *day_out = day_of_year - (153 * mp + 2) / 5 + 1;
    *month_out = mp + (mp < 10 ? 3 : -9);
    *year_out = static_cast<int>(year + (*month_out <= 2));
}

}  // namespace

namespace wqn::power::timekeep {

bool CalendarFromUnixSeconds(int64_t seconds, RtcCalendar* out)
{
    if (out == nullptr || seconds < 0) {
        return false;
    }
    const int64_t days = seconds / 86400;
    const int64_t remainder = seconds % 86400;
    int year_full = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromUtcDays(days, &year_full, &month, &day);
    if (year_full < 2000 || year_full > 2099) {
        return false;
    }
    out->year = year_full - 1900;
    out->month = static_cast<int>(month) - 1;
    out->day = static_cast<int>(day);
    out->hour = static_cast<int>(remainder / 3600);
    out->min = static_cast<int>((remainder % 3600) / 60);
    out->sec = static_cast<int>(remainder % 60);
    // 1970-01-01 was a Thursday (Sunday-based index 4).
    out->weekday = static_cast<int>((days + 4) % 7);
    return true;
}

bool UnixSecondsFromCalendar(const RtcCalendar& calendar, int64_t* seconds_out)
{
    if (seconds_out == nullptr ||
        calendar.year < 100 || calendar.year > 199 ||
        calendar.month < 0 || calendar.month > 11 ||
        calendar.day < 1 || calendar.day > 31 ||
        calendar.hour < 0 || calendar.hour > 23 ||
        calendar.min < 0 || calendar.min > 59 ||
        calendar.sec < 0 || calendar.sec > 59 ||
        calendar.weekday < 0 || calendar.weekday > 6) {
        return false;
    }
    const int64_t days = UtcDaysFromCivil(
        calendar.year + 1900,
        static_cast<unsigned>(calendar.month) + 1,
        static_cast<unsigned>(calendar.day));
    *seconds_out =
        days * 86400 + calendar.hour * 3600 + calendar.min * 60 + calendar.sec;
    return true;
}

bool PersistSystemTimeToRtc(uint32_t sleep_generation)
{
    const std::time_t now = std::time(nullptr);
    if (now < kMinReasonableUnixTime) {
        ESP_LOGW(kTag, "system clock unreasonable (%lld); skip RTC persist",
                 static_cast<long long>(now));
        return false;
    }
    RtcCalendar calendar;
    if (!CalendarFromUnixSeconds(static_cast<int64_t>(now), &calendar)) {
        ESP_LOGW(kTag, "wall clock outside the PCF8563 century window; skip RTC persist");
        return false;
    }
    if (!Pcf8563WriteTime(calendar.year, calendar.month, calendar.day,
                          calendar.hour, calendar.min, calendar.sec,
                          calendar.weekday)) {
        return false;
    }
    TimekeepTrustRecord record = {};
    record.magic = kTrustMagic;
    record.version = kTrustVersion;
    record.generation = sleep_generation;
    record.checksum = TrustChecksum(record);
    g_trust_record = record;
    ESP_LOGI(kTag,
             "wall clock persisted to PCF8563: generation=%lu utc=%04d-%02d-%02d %02d:%02d:%02d",
             static_cast<unsigned long>(sleep_generation),
             calendar.year + 1900, calendar.month + 1, calendar.day,
             calendar.hour, calendar.min, calendar.sec);
    return true;
}

bool RestoreSystemTimeFromRtc()
{
    const runtime::WakeContext& wake = runtime::GetWakeContext();
    if (!wake.deep_sleep_resume || wake.reset_reason != ESP_RST_DEEPSLEEP) {
        return false;
    }
    if (!TrustRecordValid()) {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    if (!Pcf8563ReadTime(&year, &month, &day, &hour, &min, &sec)) {
        // Pcf8563ReadTime already warned when the VL integrity flag rejected
        // the read; this covers transport-level failures.
        ESP_LOGW(kTag, "PCF8563 time unavailable on deep-sleep resume");
        return false;
    }

    RtcCalendar calendar;
    calendar.year = year;
    calendar.month = month;
    calendar.day = day;
    calendar.hour = hour;
    calendar.min = min;
    calendar.sec = sec;
    int64_t restored_seconds = 0;
    if (!UnixSecondsFromCalendar(calendar, &restored_seconds)) {
        ESP_LOGW(kTag, "PCF8563 fields invalid after resume");
        return false;
    }
    if (restored_seconds < kMinReasonableUnixTime) {
        ESP_LOGW(kTag, "restored RTC time not reasonable: %lld",
                 static_cast<long long>(restored_seconds));
        return false;
    }

    const timeval tv = {
        .tv_sec = static_cast<time_t>(restored_seconds),
        .tv_usec = 0,
    };
    settimeofday(&tv, nullptr);
    ESP_LOGI(kTag,
             "system clock calibrated from PCF8563: generation=%lu/%lu utc=%lld",
             static_cast<unsigned long>(g_trust_record.generation),
             static_cast<unsigned long>(wake.sleep_generation),
             static_cast<long long>(restored_seconds));
    return true;
}

}  // namespace wqn::power::timekeep
