// Production RTC/timekeep/lock translation units are linked without extraction.
// Codec register functions below are extracted intact; their lifecycle/recovery
// collaborators are explicit ports and are not claimed as codec runtime proof.
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <vector>

#include "pcf8563.h"
#include "power/rtc_timekeep.h"
#include "runtime/wake_context.h"
#include "i2c_bus_lock.h"
#include "services/audio_service.h"
#include "services/audio_codec_deadline.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "runtime/wall_clock.cpp"
#include "retention.cpp"

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct FixtureMutex { unsigned depth = 0; };
static FixtureMutex mutex;
static int bus_token, rtc_token, codec_token;
static unsigned takes, gives, fail_take, transfers, failure_mask, fail_read_at;
static unsigned errors, cleanup_warnings, delays, recoveries, active_operations;
static unsigned set_calls;
static bool add_fails, corrupt_read, set_fails, apply_power_fails, recovery_fails;
static int transfer_failure = ESP_ERR_TIMEOUT;
static time_t now = 1792627200;  // 2026-10-22 00:00:00 UTC.
static timeval applied{};
static int64_t monotonic_us = 1000000;
static int64_t lock_cost_us, transfer_cost_us, verify_log_cost_us;
static bool failed_write_applies;
static bool deadline_cases;
static bool synchronize_during_time_read;
static unsigned synchronize_at_transfer;
static std::vector<int> timeouts;
static std::vector<TickType_t> lock_timeouts;
int64_t esp_timer_get_time() { return monotonic_us; }
static wqn::runtime::WakeContext wake;
static std::array<uint8_t, 256> rtc_regs{}, codec_regs{};
struct Transfer { bool read; uint8_t reg; std::vector<uint8_t> bytes; };
static std::vector<Transfer> trace;

void FixtureLog(const char*, const char* fmt, ...)
{
    if (std::strstr(fmt, "RTC STOP cleanup failed")) ++cleanup_warnings;
    if (std::strstr(fmt, "PCF8563 time written and verified")) {
        monotonic_us += verify_log_cost_us;
    }
}
namespace wqn {
void RecordError(const char*, const char*, ...) { ++errors; }
}
namespace wqn::runtime {
const WakeContext& GetWakeContext() { return wake; }
}
extern "C" time_t __wrap_time(time_t* out)
{
    const time_t sampled = now;
    if (synchronize_during_time_read) {
        synchronize_during_time_read = false;
        now += 3600;
        wqn::runtime::NoteSntpTimeSynchronized(now, now);
    }
    if (out) *out = sampled;
    return sampled;
}
extern "C" int __wrap_settimeofday(const timeval* value, const struct timezone*)
{
    ++set_calls;
    if (set_fails) { errno = EPERM; return -1; }
    applied = *value;
    now = value->tv_sec;
    return 0;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return &mutex; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t value, TickType_t ticks)
{
    CHECK(value == &mutex);
    CHECK(ticks <= pdMS_TO_TICKS(100));
    if (!deadline_cases) CHECK(ticks == pdMS_TO_TICKS(100));
    lock_timeouts.push_back(ticks);
    monotonic_us += lock_cost_us;
    ++takes;
    if (fail_take == takes) return pdFALSE;
    ++mutex.depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t value)
{
    CHECK(value == &mutex && mutex.depth > 0);
    --mutex.depth;
    ++gives;
    return pdTRUE;
}
void vTaskDelay(TickType_t ticks) { CHECK(ticks > 0); ++delays; }

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t* config, i2c_master_dev_handle_t* dev)
{
    CHECK(bus == &bus_token && config->device_address == 0x51);
    CHECK(config->scl_speed_hz == 100000);
    if (add_fails) return ESP_FAIL;
    *dev = &rtc_token;
    return ESP_OK;
}
static std::array<uint8_t, 256>& Registers(i2c_master_dev_handle_t dev)
{
    CHECK(dev == &rtc_token || dev == &codec_token);
    if (dev == &codec_token) CHECK(active_operations == 1);
    return dev == &rtc_token ? rtc_regs : codec_regs;
}
static bool BeginTransfer(int milliseconds)
{
    CHECK(milliseconds > 0 && milliseconds <= 100);
    if (!deadline_cases) CHECK(milliseconds == 100);
    timeouts.push_back(milliseconds);
    monotonic_us += transfer_cost_us;
    CHECK(mutex.depth > 0);
    ++transfers;
    if (synchronize_at_transfer == transfers) {
        now += 3600;
        wqn::runtime::NoteSntpTimeSynchronized(now, now);
    }
    return (failure_mask & (1U << transfers)) == 0;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
    const uint8_t* bytes, size_t length, int milliseconds)
{
    CHECK(length >= 2 && size_t(bytes[0]) + length - 1 <= 256);
    auto& regs = Registers(dev);
    const bool success = BeginTransfer(milliseconds);
    trace.push_back({false, bytes[0], {bytes + 1, bytes + length}});
    if (success || failed_write_applies) {
        std::memcpy(regs.data() + bytes[0], bytes + 1, length - 1);
    }
    return success ? ESP_OK : transfer_failure;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
    const uint8_t* reg, size_t write_length, uint8_t* out, size_t length, int milliseconds)
{
    CHECK(write_length == 1 && size_t(*reg) + length <= 256);
    auto& regs = Registers(dev);
    const bool success = BeginTransfer(milliseconds);
    trace.push_back({true, *reg, {}});
    if (!success || transfers == fail_read_at) return transfer_failure;
    std::memcpy(out, regs.data() + *reg, length);
    if (corrupt_read) out[0] ^= 0x04;
    return ESP_OK;
}

// Only the two production codec functions are inserted. DriverOperation and
// recovery ports preserve lifetime/ordering assertions without a fake audio task.
namespace {
constexpr char kTag[] = "codec_fixture";
struct esp_sntp_config_t { void (*sync_cb)(timeval*) = nullptr; };
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) esp_sntp_config_t{}
static bool g_sntp_started;
static unsigned sntp_starts, sntp_waits, watchdog_feeds;
static int sntp_start_result = ESP_OK, sntp_wait_result = ESP_OK;
static void (*sntp_callback)(timeval*);
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t* config)
{
    ++sntp_starts;
    if (sntp_start_result == ESP_OK) sntp_callback = config->sync_cb;
    return sntp_start_result;
}
esp_err_t esp_netif_sntp_sync_wait(TickType_t ticks)
{
    CHECK(ticks == pdMS_TO_TICKS(15000));
    ++sntp_waits;
    if (sntp_wait_result == ESP_OK) {
        now = 1792627200;
        timeval synchronized{now, 0};
        if (sntp_callback) sntp_callback(&synchronized);
    }
    return sntp_wait_result;
}
void FeedTaskWatchdogIfSubscribed() { ++watchdog_feeds; }
@@CLOCK_FUNCTIONS@@
using wqn::services::detail::CodecDeadline;
@@CODEC_CONSTANTS@@
class DriverOperation {
public:
    enum class Kind { kCodec };
    DriverOperation(const wqn::services::AudioSession& session, Kind, void* codec)
        : accepted_(session.id == 1 && codec == &codec_token)
    { if (accepted_) ++active_operations; }
    ~DriverOperation() { if (accepted_) --active_operations; }
    explicit operator bool() const { return accepted_; }
private:
    bool accepted_;
};
i2c_master_dev_handle_t AsCodec(void* codec) { return codec; }
esp_err_t ApplyCodecPower(bool on) { CHECK(on); return apply_power_fails ? ESP_FAIL : ESP_OK; }
@@RETRYABLE@@
esp_err_t RecoverCodecI2cBusLocked(const wqn::services::AudioSession&,
                                  const char*, uint8_t, esp_err_t)
{
    CHECK(mutex.depth > 0 && active_operations == 1);
    ++recoveries;
    return recovery_fails ? ESP_FAIL : ESP_OK;
}
@@CODEC_HELPERS@@
}
namespace wqn::services {
@@CODEC_FUNCTIONS@@
}

static void ResetPorts()
{
    CHECK(mutex.depth == 0 && active_operations == 0);
    takes = gives = fail_take = transfers = failure_mask = fail_read_at = 0;
    errors = cleanup_warnings = delays = recoveries = set_calls = 0;
    add_fails = corrupt_read = set_fails = apply_power_fails = recovery_fails = false;
    transfer_failure = ESP_ERR_TIMEOUT;
    trace.clear();
    timeouts.clear();
    lock_timeouts.clear();
    monotonic_us = 1000000;
    lock_cost_us = transfer_cost_us = verify_log_cost_us = 0;
    failed_write_applies = false;
    synchronize_during_time_read = false;
    synchronize_at_transfer = 0;
    applied = {};
    // RTC memory and trust intentionally survive between attempts.
}
static bool WriteTime()
{
    return wqn::Pcf8563WriteTime(126, 9, 22, 12, 34, 56, 4);
}
static void Resume(uint32_t generation)
{
    wake = {};
    wake.deep_sleep_resume = true;
    wake.reset_reason = ESP_RST_DEEPSLEEP;
    wake.sleep_snapshot_valid = true;
    wake.sleep_generation = generation;
}
static void DriverCases()
{
    using namespace wqn;
    CHECK(!Pcf8563ReadTime(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    CHECK(!WriteTime());
    CHECK(!Pcf8563ConfigureTimerWake(1));
    CHECK(!Pcf8563DisableTimerWakeAndClearFlags());
    CHECK(!Pcf8563InitWithBus(nullptr));
    add_fails = true;
    CHECK(!Pcf8563InitWithBus(&bus_token));
    add_fails = false;
    CHECK(Pcf8563InitWithBus(&bus_token));
    CHECK(Pcf8563InitWithBus(&bus_token));
    ResetPorts();
    CHECK(WriteTime());
    CHECK(takes == 1 && gives == 1 && transfers == 4);
    CHECK(!trace[0].read && trace[0].reg == 0 && trace[0].bytes == std::vector<uint8_t>{0x20});
    CHECK(trace[1].reg == 2 && trace[1].bytes ==
        (std::vector<uint8_t>{0x56, 0x34, 0x12, 0x22, 0x04, 0x10, 0x26}));
    CHECK(trace[2].reg == 0 && trace[2].bytes == std::vector<uint8_t>{0});
    CHECK(trace[3].read && trace[3].reg == 2);
    int y, m, d, h, min, sec;
    CHECK(Pcf8563ReadTime(&y, &m, &d, &h, &min, &sec));
    CHECK(y == 126 && m == 9 && d == 22 && h == 12 && min == 34 && sec == 56);
    rtc_regs[2] |= 0x80;
    CHECK(!Pcf8563ReadTime(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    CHECK(errors == 1);
    rtc_regs[2] &= 0x7f;
    ResetPorts();
    CHECK(!Pcf8563WriteTime(99, 9, 22, 1, 2, 3, 4));
    CHECK(!Pcf8563WriteTime(126, 12, 22, 1, 2, 3, 4));
    CHECK(!Pcf8563WriteTime(126, 9, 22, 1, 2, 3, 7));
    CHECK(transfers == 0 && takes == 0);
    for (unsigned step = 1; step <= 4; ++step) {
        ResetPorts();
        failure_mask = 1U << step;
        CHECK(!WriteTime());
        CHECK(takes == 1 && gives == 1);
        CHECK(transfers == (step < 4 ? step + 1 : step));
        CHECK(rtc_regs[0] == 0);
        if (step < 4) CHECK(trace.back().reg == 0);
    }
    ResetPorts();
    failure_mask = (1U << 2) | (1U << 3);
    CHECK(!WriteTime());
    CHECK(transfers == 3 && errors == 1 && cleanup_warnings == 1);
    ResetPorts();
    corrupt_read = true;
    CHECK(!WriteTime());
    CHECK(transfers == 4 && errors == 1);
    ResetPorts();
    fail_take = 1;
    CHECK(!WriteTime());
    CHECK(transfers == 0 && gives == 0);
    ResetPorts();
    CHECK(!Pcf8563ConfigureTimerWake(0));
    CHECK(transfers == 0);
    CHECK(Pcf8563ConfigureTimerWake(60));
    CHECK(rtc_regs[14] == 0x82 && rtc_regs[15] == 60 && rtc_regs[1] == 1);
    CHECK(transfers == 6 && takes == 6 && gives == 6);
    for (unsigned step = 1; step <= 6; ++step) {
        ResetPorts(); failure_mask = 1U << step;
        CHECK(!Pcf8563ConfigureTimerWake(60));
        CHECK(transfers == step && takes == gives);
    }
    ResetPorts(); corrupt_read = true;
    CHECK(!Pcf8563ConfigureTimerWake(60));
    ResetPorts();
    Pcf8563InterruptFlags flags;
    CHECK(!Pcf8563ReadInterruptFlags(nullptr));
    rtc_regs[1] = 0x0c;
    CHECK(Pcf8563ReadInterruptFlags(&flags) && flags.alarm && flags.timer);
    CHECK(Pcf8563DisableTimerWakeAndClearFlags());
    CHECK(rtc_regs[14] == 2 && rtc_regs[1] == 0);
    for (unsigned step = 1; step <= 3; ++step) {
        ResetPorts(); failure_mask = 1U << step;
        CHECK(!Pcf8563DisableTimerWakeAndClearFlags());
        CHECK(transfers == step && takes == gives);
    }
    ResetPorts(); corrupt_read = true;
    CHECK(!Pcf8563DisableTimerWakeAndClearFlags());
    ResetPorts(); fail_take = 1;
    CHECK(!Pcf8563ConfigureTimerWake(60));
    CHECK(transfers == 0 && gives == 0);
    ResetPorts(); fail_take = 1;
    CHECK(!Pcf8563ReadInterruptFlags(&flags));
    CHECK(transfers == 0 && gives == 0);
    ResetPorts(); fail_take = 1;
    CHECK(!Pcf8563ReadTime(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    CHECK(transfers == 0 && gives == 0);
}

static void TimekeepCases()
{
    using namespace wqn::power::timekeep;
    ResetPorts(); Resume(10);
    CHECK(!RestoreSystemTimeFromRtc());  // No trust has ever been published.
    CHECK(transfers == 0 && set_calls == 0);
    now = 1792627200;
    CHECK(PersistSystemTimeToRtc(10));
    ResetPorts();
    CHECK(RestoreSystemTimeFromRtc());
    CHECK(set_calls == 1 && applied.tv_sec == 1792627200 && applied.tv_usec == 0);
    ResetPorts(); Resume(11);
    CHECK(!RestoreSystemTimeFromRtc());
    CHECK(transfers == 0 && set_calls == 0);
    Resume(10); wake.sleep_snapshot_valid = false;
    CHECK(!RestoreSystemTimeFromRtc());
    CHECK(transfers == 0 && set_calls == 0);
    Resume(10); wake.deep_sleep_resume = false;
    CHECK(!RestoreSystemTimeFromRtc());
    Resume(10); wake.reset_reason = ESP_RST_SW;
    CHECK(!RestoreSystemTimeFromRtc());
    Resume(10); wake.reset_reason = ESP_RST_BROWNOUT;
    CHECK(!RestoreSystemTimeFromRtc());
    Resume(10); wake.reset_reason = ESP_RST_POWERON;
    CHECK(!RestoreSystemTimeFromRtc());
    ResetPorts(); Resume(10); rtc_regs[2] |= 0x80;
    CHECK(!RestoreSystemTimeFromRtc() && set_calls == 0);
    rtc_regs[2] &= 0x7f;
    ResetPorts(); failure_mask = 1U << 1;
    CHECK(!RestoreSystemTimeFromRtc() && set_calls == 0);
    ResetPorts(); rtc_regs[3] = 0x60;
    CHECK(!RestoreSystemTimeFromRtc() && set_calls == 0);
    rtc_regs[3] = 0;
    ResetPorts(); rtc_regs[8] = 0x23;
    CHECK(!RestoreSystemTimeFromRtc() && set_calls == 0);
    rtc_regs[8] = 0x26;
    ResetPorts(); set_fails = true;
    CHECK(!RestoreSystemTimeFromRtc() && set_calls == 1);
    ResetPorts();
    CHECK(RestoreSystemTimeFromRtc());

    // Revoke old trust on every exit: invalid wall clock/century, lock denial,
    // each write phase, verification mismatch, and failed STOP cleanup.
    for (unsigned fault = 0; fault < 9; ++fault) {
        ResetPorts(); now = 1792627200;
        CHECK(PersistSystemTimeToRtc(20));
        ResetPorts();
        if (fault == 0) now = 0;
        if (fault == 1) now = 4102444800LL;  // 2100-01-01 UTC.
        if (fault == 2) fail_take = 1;
        if (fault >= 3 && fault <= 6) failure_mask = 1U << (fault - 2);
        if (fault == 7) corrupt_read = true;
        if (fault == 8) failure_mask = (1U << 2) | (1U << 3);
        CHECK(!PersistSystemTimeToRtc(21));
        ResetPorts(); Resume(21);
        CHECK(!RestoreSystemTimeFromRtc());
        CHECK(transfers == 0 && set_calls == 0);
        // Also reject the old matching snapshot: generation checks alone must
        // not hide a failure to actually revoke the previous trust record.
        Resume(20);
        CHECK(!RestoreSystemTimeFromRtc());
        CHECK(transfers == 0 && set_calls == 0);
    }
    ResetPorts(); now = 1792627200;
    CHECK(PersistSystemTimeToRtc(22));
    ResetPorts(); Resume(22);
    CHECK(RestoreSystemTimeFromRtc());  // A later success can establish new trust.
    ResetPorts(); now = 0; set_fails = true;
    CHECK(!SeedSystemTimeFromBuildTimeIfNeeded() && set_calls == 1 && now == 0);
    ResetPorts();
    CHECK(SeedSystemTimeFromBuildTimeIfNeeded());
    CHECK(set_calls == 1 && now >= 1704067200);
    ResetPorts();
    CHECK(!SeedSystemTimeFromBuildTimeIfNeeded() && set_calls == 0);

    RtcCalendar calendar;
    CHECK(CalendarFromUnixSeconds(1709251199, &calendar));  // Leap day.
    CHECK(calendar.year == 124 && calendar.month == 1 && calendar.day == 29);
    CHECK(calendar.hour == 23 && calendar.min == 59 && calendar.sec == 59);
    int64_t seconds = 0;
    CHECK(UnixSecondsFromCalendar(calendar, &seconds) && seconds == 1709251199);
    CHECK(!CalendarFromUnixSeconds(946684799, &calendar));
    CHECK(CalendarFromUnixSeconds(946684800, &calendar));
    CHECK(CalendarFromUnixSeconds(4102444799LL, &calendar));
    CHECK(!CalendarFromUnixSeconds(4102444800LL, &calendar));
    CHECK(!CalendarFromUnixSeconds(-1, &calendar));
    CHECK(!CalendarFromUnixSeconds(1709251199, nullptr));
    calendar.sec = 60;
    CHECK(!UnixSecondsFromCalendar(calendar, &seconds));
}

static void CodecCases()
{
    using namespace wqn::services;
    const AudioSession session{1, AudioActivity::kCapture};
    uint8_t value = 0;
    ResetPorts();
    CHECK(WriteAudioCodecRegister(session, &codec_token, 0x10, 0x42) == ESP_OK);
    CHECK(ReadAudioCodecRegister(session, &codec_token, 0x10, &value) == ESP_OK);
    CHECK(value == 0x42 && transfers == 2 && takes == gives);
    ResetPorts();
    CHECK(ReadAudioCodecRegister(session, &codec_token, 0x10, nullptr) == ESP_ERR_INVALID_STATE);
    CHECK(WriteAudioCodecRegister({}, &codec_token, 0x10, 0x42) == ESP_ERR_INVALID_STATE);
    CHECK(WriteAudioCodecRegister(session, nullptr, 0x10, 0x42) == ESP_ERR_INVALID_STATE);
    CHECK(transfers == 0 && takes == 0);
    for (bool read : {false, true}) {
        const auto call = [&]() { return read
            ? ReadAudioCodecRegister(session, &codec_token, 0x10, &value)
            : WriteAudioCodecRegister(session, &codec_token, 0x10, 0x42); };
        for (int retryable : {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_RESPONSE}) {
            ResetPorts(); failure_mask = 1U << 1; transfer_failure = retryable;
            CHECK(call() == ESP_OK);
            CHECK(transfers == 2 && recoveries == 1 && delays == 1 && takes == gives);
        }
        ResetPorts(); failure_mask = (1U << (kCodecI2cMaxAttempts + 1)) - 2;
        CHECK(call() == ESP_ERR_TIMEOUT);
        CHECK(transfers == kCodecI2cMaxAttempts && recoveries == kCodecI2cMaxAttempts);
        CHECK(delays == kCodecI2cMaxAttempts - 1 && takes == gives);
        ResetPorts(); failure_mask = 1U << 1; transfer_failure = ESP_ERR_INVALID_ARG;
        CHECK(call() == ESP_ERR_INVALID_ARG && transfers == 1 && recoveries == 0);
        ResetPorts(); fail_take = 1;
        CHECK(call() == ESP_OK && transfers == 1 && takes == 2 && gives == 1);
        ResetPorts(); apply_power_fails = true;
        CHECK(call() == ESP_FAIL && transfers == 0 && takes == gives);
        ResetPorts(); failure_mask = 1U << 1; recovery_fails = true;
        CHECK(call() == ESP_FAIL && transfers == 1 && recoveries == 1);
    }
}

static void PersistDeadlineCases()
{
    using namespace wqn;
    using namespace wqn::power::timekeep;
    deadline_cases = true;
    const auto write = [](int64_t deadline) {
        return Pcf8563WriteTime(126, 9, 22, 12, 34, 56, 4, deadline);
    };
    ResetPorts();
    // An expired/sub-millisecond budget never starts STOP. Lock admission and
    // the driver budget use the same absolute time, including late acquisition.
    CHECK(!write(monotonic_us));
    CHECK(takes == 0 && transfers == 0);
    CHECK(!write(monotonic_us - 1));
    CHECK(takes == 0 && transfers == 0);
    CHECK(!write(monotonic_us + 500));
    CHECK(takes == 1 && transfers == 0 && lock_timeouts.back() == 0);
    ResetPorts(); lock_cost_us = 12000;
    CHECK(!write(monotonic_us + 5000));
    CHECK(takes == gives && transfers == 0 && lock_timeouts.back() == pdMS_TO_TICKS(5));
    ResetPorts(); fail_take = 1;
    CHECK(!write(monotonic_us + 50000));
    CHECK(transfers == 0 && gives == 0);
    CHECK(lock_timeouts.back() == pdMS_TO_TICKS(50));

    ResetPorts(); transfer_cost_us = 30000;
    CHECK(write(monotonic_us + 125000));
    CHECK(timeouts == (std::vector<int>{100, 95, 65, 35}));
    CHECK(transfers == 4 && takes == 1 && gives == 1 && rtc_regs[0] == 0);
    // Expiry after STOP or burst must release STOP once, using the separate
    // cleanup allowance. A late successful release needs no duplicate cleanup.
    for (int window_ms : {25, 60, 65, 90, 100}) {
        ResetPorts(); transfer_cost_us = 30000;
        CHECK(!write(monotonic_us + window_ms * 1000));
        CHECK(takes == 1 && gives == 1 && rtc_regs[0] == 0);
        if (window_ms == 25) {
            CHECK(transfers == 2 && timeouts == (std::vector<int>{25, 100}));
        } else if (window_ms == 60) {
            CHECK(transfers == 3 && timeouts == (std::vector<int>{60, 30, 100}));
        } else if (window_ms == 65) {
            CHECK(transfers == 3 && timeouts == (std::vector<int>{65, 35, 5}));
        } else if (window_ms == 90) {
            CHECK(transfers == 3 && timeouts == (std::vector<int>{90, 60, 30}));
        } else {
            CHECK(transfers == 4 && timeouts == (std::vector<int>{100, 70, 40, 10}));
        }
    }
    // A timeout is not proof that a write had no device-side effect.
    for (unsigned step : {1U, 2U, 3U}) {
        ResetPorts(); failed_write_applies = true; failure_mask = 1U << step;
        CHECK(!write(monotonic_us + 200000));
        CHECK(transfers == step + 1 && rtc_regs[0] == 0);
        CHECK(trace.back().reg == 0 && trace.back().bytes == std::vector<uint8_t>{0});
    }
    ResetPorts(); failure_mask = (1U << 1) | (1U << 2); failed_write_applies = true;
    CHECK(!write(monotonic_us + 200000));
    CHECK(transfers == 2 && errors == 1 && cleanup_warnings == 1);
    ResetPorts(); failure_mask = (1U << 3) | (1U << 4);
    CHECK(!write(monotonic_us + 200000));
    CHECK(transfers == 4 && errors == 1 && cleanup_warnings == 1);

    // Timekeep must forward the budget, revoke old trust before any early
    // exit, and reject expiry after the driver's final validation/logging.
    for (unsigned fault = 0; fault < 4; ++fault) {
        ResetPorts(); now = 1792627200;
        CHECK(PersistSystemTimeToRtc(80));
        ResetPorts();
        int64_t deadline = monotonic_us;
        if (fault == 1) { lock_cost_us = 60000; deadline += 50000; }
        if (fault == 2) { transfer_cost_us = 30000; deadline += 60000; }
        if (fault == 3) {
            transfer_cost_us = 30000; verify_log_cost_us = 6000; deadline += 125000;
        }
        CHECK(!PersistSystemTimeToRtc(81, deadline));
        CHECK(transfers == (fault < 2 ? 0 : (fault == 2 ? 3 : 4)));
        CHECK(takes == gives && rtc_regs[0] == 0);
        ResetPorts(); Resume(81);
        CHECK(!RestoreSystemTimeFromRtc() && transfers == 0 && set_calls == 0);
        Resume(80);
        CHECK(!RestoreSystemTimeFromRtc() && transfers == 0 && set_calls == 0);
    }
    ResetPorts(); now = 1792627200; transfer_cost_us = 30000;
    CHECK(PersistSystemTimeToRtc(82, monotonic_us + 125000));
    CHECK(timeouts == (std::vector<int>{100, 95, 65, 35}));
    ResetPorts(); Resume(82);
    CHECK(RestoreSystemTimeFromRtc() && applied.tv_sec == 1792627200);
    deadline_cases = false;
}

static void ColdClock()
{
    // Model a new boot's BSS without adding reset hooks to product code.
    wqn::runtime::g_wall_clock_state.store(0, std::memory_order_relaxed);
}
static void ResetSntp()
{
    ColdClock();
    g_sntp_started = false;
    sntp_starts = sntp_waits = watchdog_feeds = 0;
    sntp_start_result = sntp_wait_result = ESP_OK;
    sntp_callback = nullptr;
}
static void SourceCases()
{
    using namespace wqn::runtime;
    ResetPorts(); ResetSntp(); now = 1792627200;
    CHECK(GetWallClockSnapshot().source == WallClockSource::kUnknown);
    StartSntpOnce(); StartSntpOnce();
    CHECK(sntp_starts == 1 && sntp_callback != nullptr);
    CHECK(GetWallClockSnapshot().epoch == 0);  // Started is not calibrated.
    TimeSyncCallback(nullptr);
    timeval invalid{};
    sntp_callback(&invalid);
    CHECK(GetWallClockSnapshot().epoch == 0);
    timeval unapplied{now + 3600, 0};
    sntp_callback(&unapplied);
    CHECK(GetWallClockSnapshot().epoch == 0);
    timeval synchronized{now, 0};
    sntp_callback(&synchronized);
    CHECK(GetWallClockSnapshot().source == WallClockSource::kSntp);
    CHECK(GetWallClockSnapshot().network_calibrated());
    CHECK(!GetWallClockSnapshot().rtc_continuity);
    const uint32_t epoch = GetWallClockSnapshot().epoch;
    sntp_callback(&synchronized);
    CHECK(GetWallClockSnapshot().epoch == epoch + 1);
    NoteRtcTimeRestored(true, kWallClockEpochMax + 1);
    CHECK(GetWallClockSnapshot().epoch == epoch + 1);

    // Execute actual HTTPS clock admission, including its fallback callback.
    ResetSntp(); now = 1792627200; NoteBuildTimeApplied();
    CHECK(EnsureClockSyncedForHttps() == ESP_OK);
    CHECK(sntp_starts == 0 && sntp_waits == 0);
    CHECK(GetWallClockSnapshot().source == WallClockSource::kBuildTime);
    ResetSntp(); now = 0;
    CHECK(EnsureClockSyncedForHttps() == ESP_OK);
    CHECK(sntp_starts == 1 && sntp_waits == 1 && watchdog_feeds == 2);
    CHECK(GetWallClockSnapshot().source == WallClockSource::kSntp);
    ResetSntp(); now = 0; sntp_start_result = ESP_FAIL;
    CHECK(EnsureClockSyncedForHttps() == ESP_FAIL && sntp_waits == 0);
    CHECK(GetWallClockSnapshot().source == WallClockSource::kUnknown);
    ResetSntp(); now = 0; sntp_wait_result = ESP_ERR_TIMEOUT;
    CHECK(EnsureClockSyncedForHttps() == ESP_ERR_TIMEOUT);
    CHECK(GetWallClockSnapshot().source == WallClockSource::kUnknown);
    ResetSntp(); now = 0; sntp_start_result = ESP_ERR_INVALID_STATE;
    CHECK(EnsureClockSyncedForHttps() == ESP_OK);  // Other initializer owns callback.
    CHECK(GetWallClockSnapshot().source == WallClockSource::kUnknown);

    ResetPorts(); ColdClock(); now = 0; set_fails = true;
    CHECK(!wqn::power::timekeep::SeedSystemTimeFromBuildTimeIfNeeded());
    CHECK(GetWallClockSnapshot().source == WallClockSource::kUnknown);
    set_fails = false;
    CHECK(wqn::power::timekeep::SeedSystemTimeFromBuildTimeIfNeeded());
    CHECK(GetWallClockSnapshot().source == WallClockSource::kBuildTime);
    CHECK(!GetWallClockSnapshot().network_calibrated());
    const auto seeded = GetWallClockSnapshot();
    CHECK(!wqn::power::timekeep::SeedSystemTimeFromBuildTimeIfNeeded());
    CHECK(GetWallClockSnapshot().epoch == seeded.epoch);
}

static wqn::TimeAppState RunningTimer()
{
    wqn::TimeAppState state;
    state.tile = wqn::TimeTile::kCountdown;
    state.active_mode = wqn::TimerMode::kCountdown;
    state.status = wqn::TimerStatus::kRunning;
    state.remaining_seconds = 120;
    state.last_tick_ms = monotonic_us / 1000;
    state.session_started_unix_seconds = state.phase_started_unix_seconds = now - 180;
    state.phase_ends_unix_seconds = now + 120;
    return state;
}
static void ResumeClock(uint32_t generation, unsigned sleep_seconds)
{
    // Advance the simulated chip using production calendar/driver functions.
    now += sleep_seconds;
    wqn::power::timekeep::RtcCalendar calendar;
    CHECK(wqn::power::timekeep::CalendarFromUnixSeconds(now, &calendar));
    CHECK(wqn::Pcf8563WriteTime(calendar.year, calendar.month, calendar.day,
        calendar.hour, calendar.min, calendar.sec, calendar.weekday));
    ColdClock(); ResetPorts(); Resume(generation);
    CHECK(wqn::power::timekeep::RestoreSystemTimeFromRtc());
}
static void ClockRetentionCases()
{
    using namespace wqn;
    using namespace wqn::runtime;
    using namespace wqn::power::timekeep;
    using namespace device_ui_internal;
    for (bool calibrated : {false, true}) {
        ResetPorts(); ColdClock(); now = 1792627200;
        if (calibrated) { timeval tv{now, 0}; TimeSyncCallback(&tv); }
        else NoteBuildTimeApplied();
        const auto before = GetWallClockSnapshot();
        auto state = RunningTimer(); RetainTimeAppState(state);
        CHECK(PersistSystemTimeToRtc(100));
        ResumeClock(100, 30);
        const auto after = GetWallClockSnapshot();
        CHECK(after.source == (calibrated ? WallClockSource::kRtcSntp : WallClockSource::kRtcApproximate));
        CHECK(after.epoch == before.epoch && after.rtc_continuity);
        CHECK(after.network_calibrated() == calibrated);
        CHECK(RestoreRetainedTimeApp(&state));
        CHECK(state.remaining_seconds == 90 && state.status == TimerStatus::kRunning);
        CHECK(state.phase_ends_unix_seconds == now + 90 && state.last_tick_ms == 0);
        CHECK(PersistSystemTimeToRtc(101));
        ResumeClock(101, 30);
        CHECK(GetWallClockSnapshot().network_calibrated() == calibrated);
        CHECK(RestoreRetainedTimeApp(&state) && state.remaining_seconds == 60);
    }

    ResetPorts(); ColdClock(); now = 1792627200; NoteBuildTimeApplied();
    auto state = RunningTimer(); RetainTimeAppState(state);
    CHECK(PersistSystemTimeToRtc(110));
    rtc_regs[2] |= 0x80; ColdClock(); Resume(110);
    CHECK(!RestoreSystemTimeFromRtc());
    CHECK(GetWallClockSnapshot().source == WallClockSource::kUnknown);
    now += 86400; NoteBuildTimeApplied();  // A future fallback must not expire it.
    CHECK(RestoreRetainedTimeApp(&state));
    CHECK(state.remaining_seconds == 120 && state.status == TimerStatus::kRunning);
    CHECK(state.phase_ends_unix_seconds == now + 120);

    for (bool calibration_before_persist : {false, true}) {
        ResetPorts(); ColdClock(); now = 1792627200; NoteBuildTimeApplied();
        state = RunningTimer(); RetainTimeAppState(state);
        if (calibration_before_persist) {
            now += 3600; timeval tv{now, 0}; TimeSyncCallback(&tv);
        }
        CHECK(PersistSystemTimeToRtc(111));
        ResumeClock(111, 30);
        if (!calibration_before_persist) {
            now += 3600; timeval tv{now, 0}; TimeSyncCallback(&tv);
        }
        CHECK(RestoreRetainedTimeApp(&state));
        CHECK(state.remaining_seconds == 120 && state.status == TimerStatus::kRunning);
        CHECK(state.phase_ends_unix_seconds == now + 120);
    }

    // Calibration before Retain can leave display timestamps on the old basis.
    // The relative snapshot uses the actual current clock and saved budget.
    ResetPorts(); ColdClock(); now = 1792627200; NoteBuildTimeApplied();
    state = RunningTimer(); now += 3600;
    timeval tv{now, 0}; TimeSyncCallback(&tv);
    RetainTimeAppState(state); CHECK(PersistSystemTimeToRtc(112));
    ResumeClock(112, 30);
    CHECK(RestoreRetainedTimeApp(&state) && state.remaining_seconds == 90);
    CHECK(state.phase_ends_unix_seconds == now + 90);

    // Preserve awake time pending between the last deduction and a button.
    ResetPorts(); ColdClock(); now = 1792627200; NoteBuildTimeApplied();
    state = RunningTimer(); monotonic_us += 5000000; now += 5;
    RetainTimeAppState(state); CHECK(PersistSystemTimeToRtc(113));
    ResumeClock(113, 30);
    CHECK(RestoreRetainedTimeApp(&state) && state.remaining_seconds == 85);

    // A source change during RTC I/O revokes the persist attempt's trust.
    ResetPorts(); ColdClock(); now = 1792627200; NoteBuildTimeApplied();
    CHECK(PersistSystemTimeToRtc(114));
    ResetPorts(); synchronize_at_transfer = 2;
    CHECK(!PersistSystemTimeToRtc(115));
    ResetPorts(); ColdClock(); Resume(115);
    CHECK(!RestoreSystemTimeFromRtc() && transfers == 0);
    Resume(114);
    CHECK(!RestoreSystemTimeFromRtc() && transfers == 0);

    // A calibration while capturing retained time invalidates that anchor.
    ResetPorts(); ColdClock(); now = 1792627200; NoteBuildTimeApplied();
    state = RunningTimer(); synchronize_during_time_read = true;
    RetainTimeAppState(state); CHECK(PersistSystemTimeToRtc(116));
    ResumeClock(116, 30);
    CHECK(RestoreRetainedTimeApp(&state) && state.remaining_seconds == 120);
}

int main()
{
    DriverCases();
    TimekeepCases();
    CodecCases();
    PersistDeadlineCases();
    SourceCases();
    ClockRetentionCases();
    CHECK(mutex.depth == 0 && active_operations == 0);
    std::printf("RTC/timekeep/codec %d Hz: %u checks PASS\n", FIXTURE_TICK_RATE_HZ, checks);
}
