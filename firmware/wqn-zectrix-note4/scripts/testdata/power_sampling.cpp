#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "power_manager.h"
#include "runtime/task_delay.h"
#include "esp_log.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
constexpr int64_t kTickUs = 1000000 / configTICK_RATE_HZ;
static int64_t monotonic_us;
static unsigned reads, calibrations, takes, gives, delays, shutdowns, failures;
static int sample_base = 2400;
static bool host, charging, full, take_fails;
static bool g_adc_initialized = true;
static int mutex_token, adc_token, calibration_token;
static void* g_adc_mutex = &mutex_token;
static void* g_adc_handle = &adc_token;
static void* g_adc_cali_handle = &calibration_token;
constexpr int kChargeDetect = 2, kChargeFull = 1, kBatAdcChannel = 3, kBatAdcBitwidth = 12;
constexpr char kTag[] = "power_host";
@@SAMPLES@@
@@POLICY@@
@@CURVE@@
void FixtureLog(const char*, const char*, ...) {}
int64_t esp_timer_get_time() { return monotonic_us; }
int gpio_get_level(int pin) { return pin == kChargeDetect ? !charging : !full; }
int xSemaphoreTake(void* mutex, TickType_t)
{
    CHECK(mutex == g_adc_mutex); ++takes;
    return take_fails ? pdFALSE : pdTRUE;
}
int xSemaphoreGive(void* mutex) { CHECK(mutex == g_adc_mutex); ++gives; return pdTRUE; }
int adc_oneshot_read(void* adc, int channel, int* raw)
{
    CHECK(adc == g_adc_handle && channel == kBatAdcChannel);
    ++reads; monotonic_us += 50;  // Controlled conversion cost, not a measurement.
    if (failures & (1U << ((reads - 1) % 10))) return ESP_FAIL;
    *raw = sample_base + static_cast<int>((reads - 1) % 10);
    return ESP_OK;
}
int adc_cali_raw_to_voltage(void* calibration, int raw, int* mv)
{
    CHECK(calibration == g_adc_cali_handle); ++calibrations;
    monotonic_us += 20; *mv = raw - 350;
    return ESP_OK;
}
void vTaskDelay(TickType_t ticks)
{
    ++delays;
    if (ticks > 0) monotonic_us += int64_t{ticks} * kTickUs - monotonic_us % kTickUs;
}
namespace wqn {
bool IsUsbHostConnected() { return host; }
void ShutdownForBatteryDepleted() { ++shutdowns; }
@@POWER@@
}
namespace device_ui_internal {
constexpr const char* kPmuStatusUnknown = "unknown";
@@BATTERY_TYPE@@
@@LOG_GATE@@
@@BATTERY_ADAPTER@@
}
static void Reset(int64_t phase = 0)
{
    monotonic_us = 1000000 + phase;
    reads = calibrations = takes = gives = delays = shutdowns = failures = 0;
    host = charging = full = take_fails = false;
    sample_base = 2400;
    g_adc_initialized = true;
    g_adc_mutex = &mutex_token; g_adc_cali_handle = &calibration_token;
}
static void BatchAndUiBudget()
{
    for (int64_t phase : {int64_t{0}, kTickUs / 2, kTickUs - 1}) {
        Reset(phase);
        wqn::PowerStatusSnapshot s;
        const auto start = monotonic_us;
        CHECK(wqn::ReadPowerStatus(&s));
        const auto elapsed = monotonic_us - start;
        std::printf("ADC batch %d Hz phase=%lld: modeled_us=%lld samples=%u\n",
                    configTICK_RATE_HZ, static_cast<long long>(phase),
                    static_cast<long long>(elapsed), reads);
        std::fflush(stdout);
        // No coarse-tick amplification in the synchronous 100 Hz UI path.
        CHECK(elapsed <= (configTICK_RATE_HZ == 100 ? 2000 : 11000));
        CHECK(reads == 10 && calibrations == 10 && takes == 1 && gives == 1);
        CHECK(s.valid && s.adc_raw == 2404 && s.adc_mv == 2054 && s.battery_mv == 4108);
        CHECK(s.battery_percent == BatteryPercentFromMillivolts(4108));
        // Execute actual input guard and UI adapter, including two further
        // adapter reads like summary + diagnostics in the traced call graph.
        Reset(phase);
        const auto ui_start = monotonic_us;
        wqn::CheckBatteryAfterUserActivity();
        device_ui_internal::BatteryReading battery;
        CHECK(device_ui_internal::ReadBatteryStatus(&battery));
        CHECK(device_ui_internal::ReadBatteryStatus(&battery));
        CHECK(reads == 30 && gives == 3 && shutdowns == 0);
        CHECK(monotonic_us - ui_start <= (configTICK_RATE_HZ == 100 ? 6000 : 33000));
        CHECK(battery.battery_mv == 4108 && !battery.external_power_present);
    }
}
static void FaultCases()
{
    wqn::PowerStatusSnapshot s;
    Reset(); CHECK(!wqn::ReadPowerStatus(nullptr) && reads == 0);
    g_adc_initialized = false;
    CHECK(!wqn::ReadPowerStatus(&s) && reads == 0 && !s.valid);
    Reset(); g_adc_mutex = nullptr;
    CHECK(!wqn::ReadPowerStatus(&s) && reads == 0);
    Reset(); take_fails = true;
    CHECK(!wqn::ReadPowerStatus(&s) && reads == 0 && gives == 0);
    Reset(); failures = 0x3ff;
    CHECK(!wqn::ReadPowerStatus(&s) && reads == 10 && gives == 1 && !s.valid);
    CHECK(wqn::GetBatteryVoltageMv() == 0 && !wqn::IsBatteryVeryLow());
    Reset(); failures = 1;
    CHECK(wqn::ReadPowerStatus(&s) && reads == 10 && calibrations == 9 && gives == 1);
    CHECK(s.adc_raw == 2405 && s.adc_mv == 2055 && s.battery_mv == 4110);
    Reset(); full = true;
    CHECK(wqn::ReadPowerStatus(&s) && !s.external_power_present && s.battery_percent < 100);
    Reset(); full = host = true;
    CHECK(wqn::ReadPowerStatus(&s) && s.external_power_present && s.battery_percent == 100);
    Reset(); charging = true;
    CHECK(wqn::ReadPowerStatus(&s) && s.charging && s.external_power_present);
    Reset(); g_adc_cali_handle = nullptr;
    CHECK(wqn::ReadPowerStatus(&s) && calibrations == 0 && s.valid && gives == 1);
    CHECK(s.adc_mv == s.adc_raw * 3100 / 4095 || s.adc_mv == s.adc_raw * 3100 / 4095 - 1);
    Reset(); sample_base = 2050;
    wqn::CheckBatteryAfterUserActivity();
    CHECK(shutdowns == 1);  // The rollback must preserve the critical-battery guard.
    Reset(); sample_base = 2050; host = true;
    wqn::CheckBatteryAfterUserActivity(); CHECK(shutdowns == 0);
    Reset(); sample_base = 2050; charging = true;
    wqn::CheckBatteryAfterUserActivity(); CHECK(shutdowns == 0);
    Reset(); sample_base = 2050; full = true;
    wqn::CheckBatteryAfterUserActivity(); CHECK(shutdowns == 1);
}
int main()
{
    BatchAndUiBudget(); FaultCases();
    std::printf("Power sampling/UI guard %d Hz: %u checks PASS\n", configTICK_RATE_HZ, checks);
}
