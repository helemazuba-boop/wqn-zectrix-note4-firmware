#include "power_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>

#include "display_service.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/sync_service.h"
#include "pcf8563.h"
#include "power/rtc_timekeep.h"
#include "power/sleep_protocol.h"
#include "power/wake_controller.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/sleep_snapshot.h"
#include "runtime/wake_context.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_manager.h"
#include "services/connectivity_service.h"
#include "services/audio_service.h"

#ifndef CONFIG_WQN_DEEP_SLEEP_IDLE_MS
#define CONFIG_WQN_DEEP_SLEEP_IDLE_MS 300000
#endif

#ifndef CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS
#define CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS 300000
#endif

#ifndef CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC
#define CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC 0
#endif

#ifndef CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV
#define CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV 3450
#endif

namespace {

constexpr char kTag[] = "wqn_power";

constexpr gpio_num_t kBoardPowerLatch = GPIO_NUM_17;
constexpr gpio_num_t kNfcPower = GPIO_NUM_21;
constexpr gpio_num_t kLed = GPIO_NUM_3;

constexpr gpio_num_t kChargeDetect = GPIO_NUM_2;
constexpr gpio_num_t kChargeFull = GPIO_NUM_1;

constexpr gpio_num_t kBatAdc = GPIO_NUM_4;
constexpr adc_channel_t kBatAdcChannel = ADC_CHANNEL_3;
constexpr int kBatAdcAtten = ADC_ATTEN_DB_12;
constexpr int kBatAdcBitwidth = ADC_BITWIDTH_12;
constexpr int kBatAdcSamples = 10;

int64_t NowMs()
{
    return esp_timer_get_time() / 1000;
}

const char* WakeupCauseName(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return "undefined";
        case ESP_SLEEP_WAKEUP_EXT0:
            return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:
            return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:
            return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:
            return "gpio";
        case ESP_SLEEP_WAKEUP_UART:
            return "uart";
        case ESP_SLEEP_WAKEUP_WIFI:
            return "wifi";
        case ESP_SLEEP_WAKEUP_BT:
            return "bt";
        default:
            return "other";
    }
}

void HoldOutput(gpio_num_t pin, int level)
{
    gpio_hold_dis(pin);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

RTC_DATA_ATTR int64_t g_last_user_activity_ms = 0;
RTC_DATA_ATTR uint32_t g_consecutive_sleep_cycles = 0;
// [sleep-race] Written by the UI task (first interaction of this boot) and
// read by the power task's timer-wake fast path; atomic removes the
// unsynchronized cross-task access.
static std::atomic<bool> g_user_interacted_current_boot{false};
// [sleep-race] Monotonic count of user interactions. The power task samples it
// BEFORE the idle/token/USB checks, re-validates before and after quiesce, and
// performs the final check inside CommitDeepSleep after the UART flush +50 ms
// settle, serialized with NoteUserActivity through g_activity_gate so no bump
// can land between that last check and the deep-sleep entry. An interaction
// in any earlier window cancels the sleep instead of losing RAM-staged input.
// g_last_user_activity_ms / g_consecutive_sleep_cycles stay plain
// RTC_DATA_ATTR variables; cross-task accesses go through std::atomic_ref so
// the int64 cannot tear on this 32-bit core and the UI-task reset of the
// cycle counter is not a data race.
static std::atomic<uint32_t> g_user_activity_generation{0};
static portMUX_TYPE g_activity_gate = portMUX_INITIALIZER_UNLOCKED;

inline std::atomic_ref<int64_t> UserActivityMsRef()
{
    return std::atomic_ref<int64_t>(g_last_user_activity_ms);
}

inline std::atomic_ref<uint32_t> ConsecutiveSleepCyclesRef()
{
    return std::atomic_ref<uint32_t>(g_consecutive_sleep_cycles);
}

adc_oneshot_unit_handle_t g_adc_handle = nullptr;
adc_cali_handle_t g_adc_cali_handle = nullptr;
bool g_adc_initialized = false;
StaticSemaphore_t g_adc_mutex_storage;
SemaphoreHandle_t g_adc_mutex = nullptr;

i2c_master_bus_handle_t g_i2c_bus = nullptr;

TaskHandle_t g_power_coordinator_task = nullptr;
std::atomic<bool> g_timer_wakeup_preference{true};
std::atomic<bool> g_battery_shutdown_requested{false};
// Published by the power task after all non-display deep-sleep admission
// checks pass. The UI task reads only this scalar; it must not call
// HasUsableStoredToken(), which performs a synchronous storage read.
std::atomic<bool> g_deep_sleep_clock_yield{false};
uint32_t g_next_sleep_generation = 1;
std::atomic<int64_t> g_sleep_retry_not_before_us{0};
wqn::runtime::SleepLease g_usb_power_lease;
bool g_usb_power_policy_sampled = false;

constexpr int64_t kPrepareSleepTimeoutUs = 5 * 1000 * 1000;
// [power-fix] Sleep-preparation failure backoff escalates per consecutive
// system failure (30s -> 60s -> 120s -> 5m cap) so a wedged service cannot
// pin the coordinator into a 30s retry storm. User-activity rollbacks reset
// the escalation instead of growing it: the user is present, fast retries
// are desirable. RTC retention keeps the curve alive across deep sleep.
constexpr int64_t kSleepRetryBackoffLadderUs[] = {
    30LL * 1000 * 1000,
    60LL * 1000 * 1000,
    120LL * 1000 * 1000,
    300LL * 1000 * 1000};
constexpr size_t kSleepRetryBackoffLadderSize =
    sizeof(kSleepRetryBackoffLadderUs) / sizeof(kSleepRetryBackoffLadderUs[0]);
RTC_DATA_ATTR uint8_t g_sleep_retry_escalation = 0;

int64_t SleepRetryBackoffUs()
{
    const uint8_t index = std::min<uint8_t>(
        g_sleep_retry_escalation,
        static_cast<uint8_t>(kSleepRetryBackoffLadderSize - 1));
    return kSleepRetryBackoffLadderUs[index];
}

// [gap-1] Background-maintenance wake escalation: sync-source timer wakes
// with no user interaction in between are counted across deep-sleep cycles;
// once this streak passes the threshold, the effective wake floor jumps to
// 15 minutes so a stuck content/retry deadline can at most burn one
// radio-on window per 15 minutes. Display/clock wakes never increment the
// counter (the minute clock is an intended 60s consumer), and any user
// interaction resets it alongside ConsecutiveSleepCycles.
constexpr uint32_t kUnattendedSyncWakeEscalationAfter = 8;
constexpr uint32_t kEscalatedSyncWakeFloorSec = 900;
// [power-fix] Unpaired-on-battery maintenance wake cadence (see the token
// policy note in EnterDeepSleepIfEnabled).
constexpr uint32_t kUnpairedBatteryMaintenanceWakeSec = 900;
RTC_DATA_ATTR uint32_t g_unattended_sync_wakes = 0;
constexpr int64_t kEmergencyStorageTimeoutUs = 2 * 1000 * 1000;
constexpr int64_t kEmergencyHardwareTimeoutUs = 2 * 1000 * 1000;
constexpr int64_t kLeaseWarningAfterUs = 60 * 1000 * 1000;

struct BatteryCurvePoint {
    int millivolts;
    int percent;
};

// Resting-voltage approximation for the single-cell Li-ion battery. The old
// quadratic returned >=100% through almost the entire useful range, so the UI
// stayed at 100% after charge removal. Keep the calibration points explicit
// and monotonic so later HIL measurements can tune this board's divider/load.
constexpr std::array<BatteryCurvePoint, 16> kBatteryCurve{{
    {3430, 0},  {3500, 3},  {3550, 7},  {3600, 12}, {3650, 20},
    {3700, 30}, {3750, 40}, {3800, 50}, {3850, 57}, {3900, 65},
    {3950, 72}, {4000, 80}, {4050, 85}, {4100, 90}, {4150, 95},
    {4200, 100},
}};

constexpr int BatteryPercentFromMillivolts(int millivolts)
{
    if (millivolts <= kBatteryCurve.front().millivolts) {
        return kBatteryCurve.front().percent;
    }
    for (size_t i = 1; i < kBatteryCurve.size(); ++i) {
        if (millivolts <= kBatteryCurve[i].millivolts) {
            const BatteryCurvePoint& lower = kBatteryCurve[i - 1];
            const BatteryCurvePoint& upper = kBatteryCurve[i];
            const int voltage_span = upper.millivolts - lower.millivolts;
            const int percent_span = upper.percent - lower.percent;
            return lower.percent +
                ((millivolts - lower.millivolts) * percent_span) / voltage_span;
        }
    }
    return kBatteryCurve.back().percent;
}

static_assert(BatteryPercentFromMillivolts(3492) < 10);
static_assert(BatteryPercentFromMillivolts(4142) < 100);
static_assert(BatteryPercentFromMillivolts(4176) < 100);

}  // namespace

namespace wqn {

void LogWakeupCause()
{
    const runtime::WakeContext& wake = runtime::GetWakeContext();
    ESP_LOGI(kTag,
             "wake context: kind=%s raw=%s(%d) reset=%d ext1=0x%llx pcf_valid=%d "
             "pcf_af=%d pcf_tf=%d sleep_snapshot=%d sleep_generation=%u "
             "sleep_cycles=%u timer_requested=%d display_timer=%d panel_cache=%s",
             runtime::WakeKindName(wake.kind), WakeupCauseName(wake.raw_cause),
             static_cast<int>(wake.raw_cause), static_cast<int>(wake.reset_reason),
             static_cast<unsigned long long>(wake.ext1_status),
             wake.pcf_flags_valid ? 1 : 0,
             wake.pcf_alarm ? 1 : 0,
             wake.pcf_timer ? 1 : 0,
             wake.sleep_snapshot_valid ? 1 : 0,
             static_cast<unsigned>(wake.sleep_generation),
             static_cast<unsigned>(wake.consecutive_sleep_cycles),
             wake.requested_timer_wakeup ? 1 : 0,
             wake.requested_display_timer_wakeup ? 1 : 0,
             wake.panel_cache_trusted ? "trusted" : "untrusted");
}

// Publishes an interaction: timestamp + generation bump + cycle reset, all
// inside g_activity_gate so the deep-sleep commit's final check (same gate)
// can never interleave with a half-published interaction, and the cycle reset
// cannot be lost against a concurrent sleep. Bump is release-ordered; the
// sleep gate reads generation with acquire.
void PublishUserActivity(int64_t occurred_at_ms)
{
    g_user_interacted_current_boot.store(true, std::memory_order_relaxed);
    g_deep_sleep_clock_yield.store(false, std::memory_order_release);
    taskENTER_CRITICAL(&g_activity_gate);
    UserActivityMsRef().store(occurred_at_ms, std::memory_order_relaxed);
    // A physical interaction starts a new HIL/product idle sequence; reset
    // inside the gate so it is not lost against a concurrent sleep commit.
    ConsecutiveSleepCyclesRef().store(0, std::memory_order_relaxed);
    g_unattended_sync_wakes = 0;
    g_user_activity_generation.fetch_add(1, std::memory_order_release);
    taskEXIT_CRITICAL(&g_activity_gate);
}

void NoteUserActivity()
{
    PublishUserActivity(NowMs());
    CheckBatteryAfterUserActivity();
}

void NoteUserActivityAtMs(int64_t occurred_at_ms)
{
    // Button-task entry: publish only. The battery check touches I2C and must
    // stay on the UI task; it runs there when the event is consumed
    // (CheckBatteryAfterUserActivity).
    PublishUserActivity(occurred_at_ms);
}

void CheckBatteryAfterUserActivity()
{
    if (IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered()) {
        ESP_LOGW(kTag, "battery critically low during user activity, initiating shutdown");
        ShutdownForBatteryDepleted();
    }
}

bool IsUiIdleForSleep()
{
    return IsUiIdleForSleepEx(0);
}

bool IsUiIdleForSleepEx(int extra_idle_ms)
{
    // If we woke up by a timer and there has been no user interaction in this boot session,
    // we should sleep immediately.
    if (runtime::GetWakeContext().kind == runtime::WakeKind::kScheduledTimer &&
        !g_user_interacted_current_boot.load(std::memory_order_relaxed)) {
        return true;
    }

    int threshold_ms = CONFIG_WQN_DEEP_SLEEP_IDLE_MS;
    /* Temporarily commented out for fast testing/verification over USB
    if (IsCharging()) {
        threshold_ms += CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS;
    }
    */
    threshold_ms += extra_idle_ms;

    const int64_t now_ms = NowMs();
    const int64_t last_activity_ms =
        UserActivityMsRef().load(std::memory_order_relaxed);
    // [power-fix] Only the last user activity drives the deep-sleep idle
    // timer. NoteEpdActivity() runs on the clock screen's minute rollover, so
    // including it in `std::max(user, epd)` made the threshold unreachable and
    // permanently pinned the
    // device in active mode. EPD activity is still tracked separately for
    // the EPD rail power-off path.
    return last_activity_ms > 0 && (now_ms - last_activity_ms) >= threshold_ms;
}

bool ShouldYieldClockRefreshToDeepSleep()
{
#if CONFIG_WQN_DEEP_SLEEP_ENABLE
    return g_deep_sleep_clock_yield.load(std::memory_order_acquire);
#else
    return false;
#endif
}

static bool DisplayIsOnlyActiveSleepBlocker()
{
    if (runtime::ActiveSleepBlockerCount(runtime::SleepBlocker::kDisplay) == 0) {
        return false;
    }
    for (uint8_t value = 0;
         value < static_cast<uint8_t>(runtime::SleepBlocker::kCount);
         ++value) {
        const auto blocker = static_cast<runtime::SleepBlocker>(value);
        if (blocker != runtime::SleepBlocker::kDisplay &&
            runtime::ActiveSleepBlockerCount(blocker) != 0) {
            return false;
        }
    }
    return true;
}

esp_err_t InitPowerHardware(i2c_port_t i2c_port, gpio_num_t i2c_sda, gpio_num_t i2c_scl, int i2c_clk_hz)
{
    gpio_config_t charge_cfg = {};
    charge_cfg.pin_bit_mask = (1ULL << kChargeDetect) | (1ULL << kChargeFull);
    charge_cfg.mode = GPIO_MODE_INPUT;
    charge_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    charge_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    charge_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&charge_cfg), kTag, "configure charge detect pins");

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &g_adc_handle), kTag, "init ADC1 unit");

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = static_cast<adc_bitwidth_t>(kBatAdcBitwidth);
    chan_cfg.atten = static_cast<adc_atten_t>(kBatAdcAtten);
    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(g_adc_handle, kBatAdcChannel, &chan_cfg),
        kTag,
        "config ADC channel %d", kBatAdcChannel);

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = static_cast<adc_atten_t>(kBatAdcAtten);
    cali_cfg.bitwidth = static_cast<adc_bitwidth_t>(kBatAdcBitwidth);
    const esp_err_t cali_ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_adc_cali_handle);
    if (cali_ret == ESP_OK) {
        ESP_LOGI(kTag, "ADC calibration (curve fitting) enabled");
    } else {
        ESP_LOGW(kTag, "ADC calibration unavailable: %s; using raw read", esp_err_to_name(cali_ret));
        g_adc_cali_handle = nullptr;
    }

    g_adc_mutex = xSemaphoreCreateMutexStatic(&g_adc_mutex_storage);
    ESP_RETURN_ON_FALSE(g_adc_mutex != nullptr, ESP_ERR_NO_MEM, kTag,
                        "create ADC mutex");
    g_adc_initialized = true;
    ESP_LOGI(kTag, "ADC initialized: channel=%d atten=%d bits=%d samples=%d",
             kBatAdcChannel, kBatAdcAtten, kBatAdcBitwidth, kBatAdcSamples);

    if (g_i2c_bus == nullptr) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = i2c_port;
        bus_cfg.sda_io_num = i2c_sda;
        bus_cfg.scl_io_num = i2c_scl;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = 1;
        const esp_err_t bus_err = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);
        if (bus_err != ESP_OK) {
            ESP_LOGE(kTag, "failed to create shared I2C bus: %s", esp_err_to_name(bus_err));
            return bus_err;
        }
        ESP_LOGI(kTag, "shared I2C bus created: port=%d SDA=%d SCL=%d clk=%d",
                 static_cast<int>(i2c_port), static_cast<int>(i2c_sda), static_cast<int>(i2c_scl), i2c_clk_hz);
    } else {
        ESP_LOGI(kTag, "shared I2C bus reused: bus=%p port=%d SDA=%d SCL=%d",
                 g_i2c_bus, static_cast<int>(i2c_port),
                 static_cast<int>(i2c_sda), static_cast<int>(i2c_scl));
    }

    if (Pcf8563InitWithBus(g_i2c_bus)) {
        power::SetPcf8563WakeAvailable(true);
        ESP_LOGI(kTag, "PCF8563 initialized on shared I2C bus");
    } else {
        power::SetPcf8563WakeAvailable(false);
        ESP_LOGW(kTag, "PCF8563 init failed; RTC timer wake will use ESP32 internal timer");
    }

    return ESP_OK;
}

i2c_master_bus_handle_t GetSharedI2cBusHandle()
{
    return g_i2c_bus;
}

bool ReadPowerStatus(PowerStatusSnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return false;
    }
    *snapshot = {};
    snapshot->charging = gpio_get_level(kChargeDetect) == 0;
    snapshot->fully_charged = gpio_get_level(kChargeFull) == 0;
    if (!g_adc_initialized || g_adc_mutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(g_adc_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    int sum_raw = 0;
    int sum_mv = 0;
    int valid_samples = 0;

    for (int i = 0; i < kBatAdcSamples; ++i) {
        int raw = 0;
        const esp_err_t ret = adc_oneshot_read(g_adc_handle, kBatAdcChannel, &raw);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "ADC read sample %d failed: %s", i, esp_err_to_name(ret));
            continue;
        }

        int mv = 0;
        if (g_adc_cali_handle != nullptr) {
            adc_cali_raw_to_voltage(g_adc_cali_handle, raw, &mv);
        } else {
            const int max_raw = (1 << kBatAdcBitwidth) - 1;
            const int max_voltage_mv = 3100;
            mv = (raw * max_voltage_mv) / max_raw;
        }

        sum_raw += raw;
        sum_mv += mv;
        ++valid_samples;

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (valid_samples == 0) {
        xSemaphoreGive(g_adc_mutex);
        return false;
    }

    snapshot->adc_raw = sum_raw / valid_samples;
    snapshot->adc_mv = sum_mv / valid_samples;
    snapshot->battery_mv = snapshot->adc_mv * 2;
    snapshot->battery_percent = snapshot->fully_charged
        ? 100
        : BatteryPercentFromMillivolts(snapshot->battery_mv);
    snapshot->valid = snapshot->battery_mv > 0;
    xSemaphoreGive(g_adc_mutex);
    return snapshot->valid;
}

uint16_t GetBatteryVoltageMv()
{
    PowerStatusSnapshot snapshot;
    return ReadPowerStatus(&snapshot)
        ? static_cast<uint16_t>(snapshot.battery_mv)
        : 0;
}

int GetBatteryPercent()
{
    PowerStatusSnapshot snapshot;
    return ReadPowerStatus(&snapshot) ? snapshot.battery_percent : 0;
}

bool IsCharging()
{
    return gpio_get_level(kChargeDetect) == 0;
}

bool IsUsbPowered()
{
    return IsUsbHostConnected() || IsCharging() || IsFullyCharged();
}

bool IsUsbHostConnected()
{
    // ESP-IDF's connection monitor samples USB SOF packets from the host.
    // CHRG_L and /STDBY describe charger state, not whether a PC is attached;
    // both may be high on a physically connected Note4, which previously let
    // the 60-second deep-sleep policy tear down native USB and reset Flash.
    return usb_serial_jtag_is_connected();
}

// kChargeFull (GPIO1) is the /STDBY pin of the charge IC (e.g. TP4056).
// It is open-drain active-low: when the battery is NOT fully charged the
// pin floats high (via MCU pull-up); when fully charged it is driven low.
// Therefore we detect a full charge by reading 0, not 1.
bool IsFullyCharged()
{
    return gpio_get_level(kChargeFull) == 0;
}

void RefreshUsbPowerSleepPolicy()
{
    const bool host_connected = IsUsbHostConnected();
    const bool charging = IsCharging();
    const bool full = IsFullyCharged();
    const bool usb_powered = host_connected || charging || full;

    if (usb_powered && !g_usb_power_lease) {
        runtime::SleepLease lease = runtime::SleepLease::TryAcquire(
            runtime::SleepBlocker::kUsbPower,
            "usb-power-present",
            __FILE__,
            __LINE__);
        if (!lease) {
            ESP_LOGW(
                kTag,
                "USB/charger detected but sleep lease acquisition was denied: host=%d charging=%d full=%d",
                host_connected ? 1 : 0,
                charging ? 1 : 0,
                full ? 1 : 0);
            return;
        }
        g_usb_power_lease = std::move(lease);
        ESP_LOGI(
            kTag,
            "USB/charger detected: host=%d charging=%d full=%d; light/deep sleep blocked",
            host_connected ? 1 : 0,
            charging ? 1 : 0,
            full ? 1 : 0);
    } else if (!usb_powered && g_usb_power_lease) {
        g_usb_power_lease.Reset();
        ESP_LOGI(kTag, "USB/charger removed; light/deep sleep policy restored");
    } else if (!g_usb_power_policy_sampled && !usb_powered) {
        ESP_LOGI(
            kTag,
            "USB/charger not detected: host=0 charging=0 full=0; normal sleep policy active");
    }
    g_usb_power_policy_sampled = true;
}

bool IsBatteryLow()
{
    const uint16_t mv = GetBatteryVoltageMv();
    if (mv == 0) {
        return false;
    }
    return mv <= CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV;
}

bool IsBatteryVeryLow()
{
    const uint16_t mv = GetBatteryVoltageMv();
    if (mv == 0) {
        return false;
    }
    constexpr int kVeryLowMv = 3430;
    return mv <= kVeryLowMv;
}


static power::PrepareSleepResult MakePrepareResult(
    const power::PrepareSleepCommand& command,
    power::SleepService service,
    esp_err_t error)
{
    power::PrepareSleepResult result;
    result.generation = command.generation;
    result.service = service;
    result.error = error;
    if (error == ESP_OK) {
        result.status = power::SleepPrepareStatus::kReady;
    } else if (error == ESP_ERR_TIMEOUT ||
               (command.deadline_us > 0 && esp_timer_get_time() >= command.deadline_us)) {
        result.status = power::SleepPrepareStatus::kTimedOut;
    } else {
        result.status = power::SleepPrepareStatus::kDenied;
    }
    ESP_LOGI(kTag,
             "prepare-sleep result: generation=%u service=%s status=%s error=%s",
             static_cast<unsigned>(result.generation),
             power::SleepServiceName(result.service),
             power::SleepPrepareStatusName(result.status),
             esp_err_to_name(result.error));
    return result;
}

static power::PrepareSleepResults BroadcastPrepareSleep(const power::PrepareSleepCommand& command)
{
    power::PrepareSleepResults results{};
    ESP_LOGI(kTag, "prepare-sleep broadcast: generation=%u mode=%s deadline_us=%lld",
             static_cast<unsigned>(command.generation),
             power::SleepModeName(command.mode),
             static_cast<long long>(command.deadline_us));

    const auto deadline_result = [&command]() {
        return command.deadline_us > 0 && esp_timer_get_time() >= command.deadline_us
            ? ESP_ERR_TIMEOUT
            : ESP_OK;
    };

    esp_err_t error = deadline_result();
    if (error == ESP_OK) {
        error = PrepareDisplayForSleep(command.deadline_us);
    }
    results[static_cast<size_t>(power::SleepService::kDisplay)] =
        MakePrepareResult(command, power::SleepService::kDisplay, error);

    error = deadline_result();
    if (error == ESP_OK) {
        error = PrepareStorageForSleep(command.deadline_us);
    }
    results[static_cast<size_t>(power::SleepService::kStorage)] =
        MakePrepareResult(command, power::SleepService::kStorage, error);

    error = deadline_result();
    if (error == ESP_OK) {
        error = services::PrepareAudioServiceForSleep(command);
    }
    results[static_cast<size_t>(power::SleepService::kAudio)] =
        MakePrepareResult(command, power::SleepService::kAudio, error);

    error = deadline_result();
    if (error == ESP_OK) {
        error = services::PrepareConnectivityForSleep(command);
    }
    results[static_cast<size_t>(power::SleepService::kConnectivity)] =
        MakePrepareResult(command, power::SleepService::kConnectivity, error);
    return results;
}

static bool AllServicesReady(
    const power::PrepareSleepCommand& command,
    const power::PrepareSleepResults& results)
{
    for (const power::PrepareSleepResult& result : results) {
        if (result.generation != command.generation ||
            result.status != power::SleepPrepareStatus::kReady) {
            return false;
        }
    }
    return true;
}

static void PrepareBoardPowerState(power::SleepMode mode)
{
    HoldOutput(kNfcPower, 0);
    HoldOutput(kLed, 1);
    HoldOutput(kBoardPowerLatch, mode == power::SleepMode::kIdle ? 1 : 0);
    gpio_deep_sleep_hold_en();
}

static void RollbackBoardPowerState()
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(kNfcPower);
    gpio_set_level(kNfcPower, 0);
    gpio_hold_dis(kLed);
    gpio_set_level(kLed, 1);
    gpio_hold_dis(kBoardPowerLatch);
    gpio_set_level(kBoardPowerLatch, 1);
}

static void RollbackSleepPreparation(uint32_t generation, const char* reason)
{
    // User-activity rollbacks are healthy interactions, not system failures:
    // reset the escalation ladder so retries stay fast while the user is
    // present. Everything else (service denial/timeout, wake-arm errors)
    // climbs the ladder.
    const bool user_activity_rollback =
        std::strncmp(reason, "user-activity", 13) == 0;
    if (user_activity_rollback) {
        g_sleep_retry_escalation = 0;
    } else if (g_sleep_retry_escalation <
               static_cast<uint8_t>(kSleepRetryBackoffLadderSize - 1)) {
        ++g_sleep_retry_escalation;
    }
    const int64_t retry_backoff_us = SleepRetryBackoffUs();
    ESP_LOGW(kTag,
             "sleep rollback: generation=%u reason=%s retry_ms=%lld escalation=%u",
             static_cast<unsigned>(generation), reason,
             static_cast<long long>(retry_backoff_us / 1000),
             static_cast<unsigned>(g_sleep_retry_escalation));
    power::DisarmWakeSources();
    // Reopen lease acquisition before services restore active hardware. A
    // service returning to Connecting/Provisioning must be able to reacquire
    // its lease as part of the synchronous rollback.
    runtime::CancelSleepQuiesce(generation);
    services::RollbackConnectivityAfterSleepAbort();
    services::RollbackAudioServiceAfterSleepAbort(generation);
    RollbackStorageAfterSleepAbort();
    RollbackDisplayAfterSleepAbort();
    RollbackBoardPowerState();
    runtime::InvalidateSleepSnapshot();
    g_sleep_retry_not_before_us.store(
        esp_timer_get_time() + retry_backoff_us,
        std::memory_order_relaxed);
    g_deep_sleep_clock_yield.store(false, std::memory_order_release);
}

static uint32_t NextSleepGeneration()
{
    uint32_t generation = g_next_sleep_generation++;
    if (generation == 0) {
        generation = g_next_sleep_generation++;
    }
    return generation;
}

// Commits the prepared deep sleep. For the idle path, gate_on_activity_baseline
// points at the activity generation sampled before the idle checks: the FINAL
// validation runs after the UART flush + 50 ms settle, inside g_activity_gate
// (the same critical section NoteUserActivity publishes through), so no bump
// can land between the check and the deep-sleep entry. Returns only when the
// sleep was aborted (late activity) -- the caller must roll back. The battery-
// emergency path passes nullptr: it must power down regardless of input.
static bool CommitDeepSleep(
    const power::PrepareSleepCommand& command,
    const uint32_t* gate_on_activity_baseline)
{
    ESP_LOGI(kTag, "deep-sleep commit: generation=%u mode=%s consecutive=%u stack_free=%u radio_on_total_ms=%u",
             static_cast<unsigned>(command.generation),
             power::SleepModeName(command.mode),
             static_cast<unsigned>(ConsecutiveSleepCyclesRef().load(std::memory_order_relaxed)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(wqn::GetWifiRadioOnTotalMs()));
    uart_wait_tx_idle_polling(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM));
    vTaskDelay(pdMS_TO_TICKS(50));

    // The idle path validates the interaction generation one last time under
    // g_activity_gate so no NoteUserActivity can publish between the check and
    // the sleep; the emergency path (nullptr baseline) sleeps unconditionally.
    // Both converge on the SINGLE deep-sleep entry below (the M8
    // architecture gate enforces exactly one deep-sleep entry firmware-wide).
    bool proceed = true;
    if (gate_on_activity_baseline != nullptr) {
        taskENTER_CRITICAL(&g_activity_gate);
        proceed = g_user_activity_generation.load(std::memory_order_acquire) ==
            *gate_on_activity_baseline;
        if (!proceed) {
            taskEXIT_CRITICAL(&g_activity_gate);
        }
        // When proceeding, the gate is held THROUGH the deep-sleep entry (which
        // never returns): a racing NoteUserActivity spins on the gate and can
        // only publish after we are asleep, at which point its key press is
        // itself an armed wake source.
    }
    if (proceed) {
        // noreturn: nothing after this call is reachable.
        esp_deep_sleep_start();
    }
    return true;
}

static void RunBatteryEmergencyShutdown()
{
    const uint32_t generation = NextSleepGeneration();
    if (!runtime::BeginEmergencySleepQuiesce(generation)) {
        ESP_LOGE(kTag, "cannot begin emergency quiesce: generation=%u",
                 static_cast<unsigned>(generation));
        return;
    }

    const int64_t storage_deadline_us = esp_timer_get_time() + kEmergencyStorageTimeoutUs;
    while (runtime::ActiveSleepBlockerCount(runtime::SleepBlocker::kStorage) != 0 &&
           esp_timer_get_time() < storage_deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    power::PrepareSleepCommand command;
    command.generation = generation;
    command.mode = power::SleepMode::kBatteryEmergency;
    command.deadline_us = esp_timer_get_time() + kEmergencyHardwareTimeoutUs;
    const power::PrepareSleepResults results = BroadcastPrepareSleep(command);
    for (const power::PrepareSleepResult& result : results) {
        if (result.status != power::SleepPrepareStatus::kReady) {
            ESP_LOGW(kTag, "emergency shutdown continuing after service failure: service=%s status=%s",
                     power::SleepServiceName(result.service),
                     power::SleepPrepareStatusName(result.status));
        }
    }

    power::DisarmWakeSources();
    PrepareBoardPowerState(power::SleepMode::kBatteryEmergency);
    runtime::SleepSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.mode = power::SleepMode::kBatteryEmergency;
    snapshot.consecutive_cycles = ConsecutiveSleepCyclesRef().load(std::memory_order_relaxed);
    runtime::CommitSleepSnapshot(snapshot);
    CommitDeepSleep(command, nullptr);
}

static bool PreemptIdleSleepForBatteryEmergency(uint32_t generation)
{
    if (!g_battery_shutdown_requested.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    RollbackSleepPreparation(generation, "battery-emergency-preemption");
    RunBatteryEmergencyShutdown();
    return true;
}

static void EnterDeepSleepIfEnabled(bool enable_timer_wakeup)
{
#if CONFIG_WQN_DEEP_SLEEP_ENABLE
    if (g_battery_shutdown_requested.exchange(false, std::memory_order_acq_rel) ||
        (IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered())) {
        RunBatteryEmergencyShutdown();
        return;
    }
    // The charger status pins are also deep-sleep wake sources. This explicit
    // guard prevents beginning quiesce while USB is already present; the USB
    // SleepLease additionally keeps automatic light sleep out of serial and
    // charging sessions.
    // [sleep-race] Sample the interaction generation BEFORE the idle checks:
    // an interaction landing after IsUiIdleForSleep() but before the sample
    // would otherwise become the baseline and slip through every later
    // validation. A button consumed after the idle check can arm a persist
    // effect whose reserve then fails (quiesce rejects new leases), leaving
    // the input staged in RAM only -- sleeping would silently drop it.
    const uint32_t activity_generation_before =
        g_user_activity_generation.load(std::memory_order_acquire);

    if (IsUsbPowered()) {
        g_deep_sleep_clock_yield.store(false, std::memory_order_release);
        return;
    }
    // [power-fix] Unpaired (or 401-cleared) identity on battery no longer
    // refuses deep sleep forever: it used to be an always-on brick draining
    // the cell while showing the pairing screen. On battery it sleeps with a
    // 15-minute quiet maintenance cadence; any button is an armed ext1 wake
    // that returns to the pairing UI, and USB power keeps the old behavior so
    // the SoftAP pairing portal stays reachable. While a provisioning portal
    // is actually serving, its kConnectivity lease blocks quiesce anyway.
    const bool has_usable_token = services::HasUsableStoredToken();
    if (esp_timer_get_time() <
            g_sleep_retry_not_before_us.load(std::memory_order_relaxed) ||
        !IsUiIdleForSleep()) {
        g_deep_sleep_clock_yield.store(false, std::memory_order_release);
        return;
    }
    // Validate before closing lease acquisition...
    if (g_user_activity_generation.load(std::memory_order_acquire) !=
        activity_generation_before) {
        g_deep_sleep_clock_yield.store(false, std::memory_order_release);
        return;
    }

    // Publish only when display is the sole remaining blocker. A cloud/storage
    // lease must never freeze the visible clock while unrelated work is still
    // legitimately keeping the device awake.
    g_deep_sleep_clock_yield.store(
        DisplayIsOnlyActiveSleepBlocker(), std::memory_order_release);
    const uint32_t generation = NextSleepGeneration();
    if (!runtime::TryBeginSleepQuiesce(generation)) {
        return;
    }
    // ...and again right after: a bump inside this window means an armed
    // effect may just have failed its reserve against the closed gate.
    if (g_user_activity_generation.load(std::memory_order_acquire) !=
        activity_generation_before) {
        RollbackSleepPreparation(generation, "user-activity-during-quiesce");
        return;
    }

    power::PrepareSleepCommand command;
    command.generation = generation;
    command.mode = power::SleepMode::kIdle;
    command.deadline_us = esp_timer_get_time() + kPrepareSleepTimeoutUs;
    const power::PrepareSleepResults results = BroadcastPrepareSleep(command);
    if (!AllServicesReady(command, results)) {
        RollbackSleepPreparation(generation, "service-denied-or-timeout");
        return;
    }
    if (PreemptIdleSleepForBatteryEmergency(generation)) {
        return;
    }

    // Re-evaluate after every service has quiesced. A sync failure can publish
    // its retry deadline immediately before releasing the online-sync lease;
    // relying only on the UI task's earlier preference sample could then sleep
    // with no timer on a non-clock screen and strand the retry indefinitely.
    const uint32_t display_wakeup_seconds = enable_timer_wakeup
        ? CONFIG_WQN_DEEP_SLEEP_TIMER_WAKE_SEC
        : 0;
    uint32_t timer_wakeup_seconds = display_wakeup_seconds;
    uint32_t sync_wakeup_seconds = services::SecondsUntilNextSyncWake();
    // [power-fix] Without a usable identity there is nothing to sync; ignore
    // sync-derived deadlines (claim polling would otherwise pin the cadence
    // at ~1s) and hold a slow 15-minute maintenance rhythm instead.
    if (!has_usable_token) {
        sync_wakeup_seconds = 0;
        timer_wakeup_seconds =
            kUnpairedBatteryMaintenanceWakeSec;
    } else if (sync_wakeup_seconds != 0 &&
        (timer_wakeup_seconds == 0 ||
         sync_wakeup_seconds < timer_wakeup_seconds)) {
        timer_wakeup_seconds = sync_wakeup_seconds;
    }
    // [gap-1] Fold any sub-floor wake interval up to the floor. A sync retry
    // or content deadline of a few seconds used to become a boot-per-cycle
    // micro-wake loop with the radio on (the dominant battery drain in the
    // 2026-08-19 audit). A deadline landing inside the floor fires one
    // interval late; admission at boot still gates whether the radio starts.
    // [power-fix] An unpaired maintenance wake is neither a display nor a
    // sync wake: classify it as background so the boot path skips panel
    // init for it (IsBackgroundSyncTimerWake) and the pairing screen stays
    // exactly as the user left it.
    const bool timer_wakeup_for_display = has_usable_token &&
        display_wakeup_seconds != 0 &&
        (sync_wakeup_seconds == 0 || display_wakeup_seconds <= sync_wakeup_seconds);
    uint32_t wake_floor_seconds = CONFIG_WQN_SLEEP_TIMER_WAKE_FLOOR_SEC;
    // [gap-1] Unattended background-maintenance wakes escalate: after enough
    // consecutive sync-source cycles with zero user interaction, widen the
    // floor so a stuck deadline can burn at most one radio window per 15
    // minutes instead of one per interval. Display/clock wakes neither count
    // nor reset the streak.
    bool sync_wake_escalated = false;
    if (has_usable_token && !timer_wakeup_for_display &&
        sync_wakeup_seconds != 0 &&
        g_unattended_sync_wakes >= kUnattendedSyncWakeEscalationAfter &&
        wake_floor_seconds != 0 &&
        wake_floor_seconds < kEscalatedSyncWakeFloorSec) {
        wake_floor_seconds = kEscalatedSyncWakeFloorSec;
        sync_wake_escalated = true;
    }
    bool wake_floor_applied = false;
    if (wake_floor_seconds != 0 && timer_wakeup_seconds != 0 &&
        timer_wakeup_seconds < wake_floor_seconds) {
        timer_wakeup_seconds = wake_floor_seconds;
        wake_floor_applied = true;
    }
    ESP_LOGI(kTag,
             "deep-sleep wake plan: display_sec=%u sync_sec=%u chosen=%u "
             "source=%s%s%s",
             static_cast<unsigned>(display_wakeup_seconds),
             static_cast<unsigned>(sync_wakeup_seconds),
             static_cast<unsigned>(timer_wakeup_seconds),
             !has_usable_token ? "unpaired-maintenance"
                 : (timer_wakeup_for_display ? "display"
                 : (sync_wakeup_seconds != 0 ? "sync" : "off")),
             wake_floor_applied ? " floor-clamped" : "",
             sync_wake_escalated ? "+unattended-escalated" : "");
#if CONFIG_WQN_RTC_TIMEKEEP_ENABLE
    // [rtc-timekeep] Persist the wall clock after every service has quiesced
    // (shared I2C bus idle) and before wake-source assembly reprograms the
    // PCF8563 timer. Deliberately fault-tolerant and non-rollbackable: a
    // failed write only costs one sleep cycle of clock freshness, and a
    // rollback that retries the sleep simply overwrites the record.
    if (!power::timekeep::PersistSystemTimeToRtc(generation)) {
        ESP_LOGW(kTag, "RTC time persist skipped; next boot falls back to build-time seeding");
    }
#endif
    const power::WakeArmResult wake =
        power::ArmWakeSources(timer_wakeup_seconds, command.deadline_us);
    if (wake.error != ESP_OK) {
        RollbackSleepPreparation(generation, esp_err_to_name(wake.error));
        return;
    }
    if (PreemptIdleSleepForBatteryEmergency(generation)) {
        return;
    }

    PrepareBoardPowerState(power::SleepMode::kIdle);
    // [sleep-race] Late gate: every service is quiesced and wake sources are
    // armed, but an interaction that slipped in during PrepareSleep/
    // ArmWakeSources may have staged state that would die with this RAM
    // image. The FINAL check runs inside CommitDeepSleep, after the UART
    // flush + 50 ms settle, under g_activity_gate.
    if (g_user_activity_generation.load(std::memory_order_acquire) !=
        activity_generation_before) {
        RollbackSleepPreparation(generation, "user-activity-before-commit");
        return;
    }
    ConsecutiveSleepCyclesRef().fetch_add(1, std::memory_order_relaxed);
    if (timer_wakeup_seconds != 0 && !timer_wakeup_for_display) {
        ++g_unattended_sync_wakes;
    }
    runtime::SleepSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.mode = power::SleepMode::kIdle;
    snapshot.timer_wakeup_enabled = timer_wakeup_seconds != 0;
    snapshot.timer_wakeup_for_display = timer_wakeup_for_display;
    snapshot.consecutive_cycles = ConsecutiveSleepCyclesRef().load(std::memory_order_relaxed);
    snapshot.wake_gpio_mask = wake.wake_gpio_mask;
    runtime::CommitSleepSnapshot(snapshot);
    if (CommitDeepSleep(command, &activity_generation_before)) {
        // The interaction landed after the pre-commit check, i.e. possibly
        // after this cycle's fetch_add: re-zero so a correctly-cancelled sleep
        // never leaves the consecutive counter at 1.
        ConsecutiveSleepCyclesRef().store(0, std::memory_order_relaxed);
        RollbackSleepPreparation(generation, "user-activity-at-commit");
    }
#else
    (void)enable_timer_wakeup;
#endif
}

void SetDeepSleepTimerWakePreference(bool enabled)
{
    g_timer_wakeup_preference.store(enabled, std::memory_order_release);
}

static void PowerCoordinatorTask(void*)
{
    ESP_LOGI(kTag, "power coordinator task started: stack_free=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    while (true) {
        RefreshUsbPowerSleepPolicy();
        runtime::LogLongHeldSleepLeases(esp_timer_get_time(), kLeaseWarningAfterUs);
        EnterDeepSleepIfEnabled(g_timer_wakeup_preference.load(std::memory_order_acquire));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

esp_err_t StartPowerCoordinator()
{
    if (g_power_coordinator_task != nullptr) {
        return ESP_OK;
    }
    runtime::SleepSnapshot snapshot;
    if (runtime::LoadSleepSnapshot(&snapshot)) {
        g_next_sleep_generation = snapshot.generation + 1;
        if (g_next_sleep_generation == 0) {
            g_next_sleep_generation = 1;
        }
        ConsecutiveSleepCyclesRef().store(
            snapshot.consecutive_cycles, std::memory_order_relaxed);
    }
    const BaseType_t created =
        xTaskCreate(PowerCoordinatorTask, "wqn_power_coord", 8192, nullptr, 4, &g_power_coordinator_task);
    if (created != pdPASS) {
        g_power_coordinator_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ShutdownForBatteryDepleted()
{
    if (!g_battery_shutdown_requested.exchange(true, std::memory_order_acq_rel)) {
        ESP_LOGW(kTag, "battery depleted: emergency shutdown requested");
    }
    if (g_power_coordinator_task != nullptr) {
        xTaskNotifyGive(g_power_coordinator_task);
    }
}

}  // namespace wqn
