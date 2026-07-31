#include "power_manager.h"

#include <algorithm>
#include <atomic>
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
#include "freertos/task.h"
#include "services/sync_service.h"
#include "pcf8563.h"
#include "power/sleep_protocol.h"
#include "power/wake_controller.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/sleep_snapshot.h"
#include "runtime/wake_context.h"
#include "sdkconfig.h"
#include "storage.h"
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
static bool g_user_interacted_current_boot = false;
// [sleep-race] Monotonic count of user interactions. The power task samples it
// before TryBeginSleepQuiesce and re-checks right after quiesce succeeds and
// again before the final sleep commit: an interaction landing in that window
// (e.g. an answer whose persist reserve now fails because quiesce closed lease
// acquisition) cancels the sleep instead of losing the RAM-staged input.
// g_last_user_activity_ms itself is written by the UI task and read by the
// power task; accesses go through std::atomic_ref so the int64 cannot tear on
// this 32-bit core (the variable stays a plain RTC_DATA_ATTR int64_t).
static std::atomic<uint32_t> g_user_activity_generation{0};

inline std::atomic_ref<int64_t> UserActivityMsRef()
{
    return std::atomic_ref<int64_t>(g_last_user_activity_ms);
}

adc_oneshot_unit_handle_t g_adc_handle = nullptr;
adc_cali_handle_t g_adc_cali_handle = nullptr;
bool g_adc_initialized = false;

i2c_master_bus_handle_t g_i2c_bus = nullptr;

TaskHandle_t g_power_coordinator_task = nullptr;
std::atomic<bool> g_timer_wakeup_preference{true};
std::atomic<bool> g_battery_shutdown_requested{false};
uint32_t g_next_sleep_generation = 1;
int64_t g_sleep_retry_not_before_us = 0;
wqn::runtime::SleepLease g_usb_power_lease;
bool g_usb_power_policy_sampled = false;

constexpr int64_t kPrepareSleepTimeoutUs = 5 * 1000 * 1000;
constexpr int64_t kSleepRetryBackoffUs = 30 * 1000 * 1000;
constexpr int64_t kEmergencyStorageTimeoutUs = 2 * 1000 * 1000;
constexpr int64_t kEmergencyHardwareTimeoutUs = 2 * 1000 * 1000;
constexpr int64_t kLeaseWarningAfterUs = 60 * 1000 * 1000;

}  // namespace

namespace wqn {

void LogWakeupCause()
{
    const runtime::WakeContext& wake = runtime::GetWakeContext();
    ESP_LOGI(kTag,
             "wake context: kind=%s raw=%s(%d) reset=%d ext1=0x%llx pcf_valid=%d "
             "pcf_af=%d pcf_tf=%d sleep_snapshot=%d sleep_generation=%u "
             "sleep_cycles=%u timer_requested=%d panel_cache=%s",
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
             wake.panel_cache_trusted ? "trusted" : "untrusted");
}

void NoteUserActivity()
{
    g_user_interacted_current_boot = true;
    UserActivityMsRef().store(NowMs(), std::memory_order_relaxed);
    // Bump AFTER the timestamp so a generation observer that re-reads the
    // timestamp sees a value at least as fresh as the bump it noticed.
    g_user_activity_generation.fetch_add(1, std::memory_order_release);
    // A physical interaction starts a new HIL/product idle sequence.
    g_consecutive_sleep_cycles = 0;
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
        !g_user_interacted_current_boot) {
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
    // timer. NoteEpdActivity() is called on every partial refresh (e.g. the
    // clock screen's minute-rollover), so including it in `std::max(user,
    // epd)` made the threshold unreachable and permanently pinned the
    // device in active mode. EPD activity is still tracked separately for
    // the EPD rail power-off path.
    return last_activity_ms > 0 && (now_ms - last_activity_ms) >= threshold_ms;
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
    if (!g_adc_initialized) {
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
        return false;
    }

    snapshot->adc_raw = sum_raw / valid_samples;
    snapshot->adc_mv = sum_mv / valid_samples;
    snapshot->battery_mv = snapshot->adc_mv * 2;
    const int64_t mv = snapshot->battery_mv;
    const int64_t percent = (-mv * mv + 9016LL * mv - 19189000LL) / 10000LL;
    snapshot->battery_percent = std::clamp(static_cast<int>(percent), 0, 100);
    snapshot->valid = snapshot->battery_mv > 0;
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
    ESP_LOGW(kTag, "sleep rollback: generation=%u reason=%s retry_ms=30000",
             static_cast<unsigned>(generation), reason);
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
    g_sleep_retry_not_before_us = esp_timer_get_time() + kSleepRetryBackoffUs;
}

static uint32_t NextSleepGeneration()
{
    uint32_t generation = g_next_sleep_generation++;
    if (generation == 0) {
        generation = g_next_sleep_generation++;
    }
    return generation;
}

static void CommitDeepSleep(const power::PrepareSleepCommand& command)
{
    ESP_LOGI(kTag, "deep-sleep commit: generation=%u mode=%s consecutive=%u stack_free=%u",
             static_cast<unsigned>(command.generation),
             power::SleepModeName(command.mode),
             static_cast<unsigned>(g_consecutive_sleep_cycles),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    uart_wait_tx_idle_polling(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM));
    vTaskDelay(pdMS_TO_TICKS(50));

    // This is intentionally the only deep-sleep call site in the firmware.
    esp_deep_sleep_start();
    RollbackSleepPreparation(command.generation, "deep-sleep-returned");
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
    snapshot.consecutive_cycles = g_consecutive_sleep_cycles;
    runtime::CommitSleepSnapshot(snapshot);
    CommitDeepSleep(command);
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
    if (IsUsbPowered()) {
        return;
    }
    if (esp_timer_get_time() < g_sleep_retry_not_before_us ||
        !services::HasUsableStoredToken() || !IsUiIdleForSleep()) {
        return;
    }

    // [sleep-race] Sample the interaction generation BEFORE closing lease
    // acquisition. A button consumed between the idle check and quiesce can
    // arm a persist effect whose reserve then fails (quiesce rejects new
    // leases), leaving the input staged in RAM only -- sleeping now would
    // silently drop it. Re-check after quiesce and before the final commit.
    const uint32_t activity_generation_before =
        g_user_activity_generation.load(std::memory_order_acquire);

    const uint32_t generation = NextSleepGeneration();
    if (!runtime::TryBeginSleepQuiesce(generation)) {
        return;
    }
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

    const power::WakeArmResult wake =
        power::ArmWakeSources(enable_timer_wakeup, command.deadline_us);
    if (wake.error != ESP_OK) {
        RollbackSleepPreparation(generation, esp_err_to_name(wake.error));
        return;
    }
    if (PreemptIdleSleepForBatteryEmergency(generation)) {
        return;
    }

    PrepareBoardPowerState(power::SleepMode::kIdle);
    // [sleep-race] Final gate before the point of no return: every service is
    // quiesced and wake sources are armed, but an interaction that slipped in
    // during PrepareSleep/ArmWakeSources may have staged state that would die
    // with this RAM image. Abort; the retry path re-runs the idle checks.
    if (g_user_activity_generation.load(std::memory_order_acquire) !=
        activity_generation_before) {
        RollbackSleepPreparation(generation, "user-activity-before-commit");
        return;
    }
    ++g_consecutive_sleep_cycles;
    runtime::SleepSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.mode = power::SleepMode::kIdle;
    snapshot.timer_wakeup_enabled = enable_timer_wakeup;
    snapshot.consecutive_cycles = g_consecutive_sleep_cycles;
    snapshot.wake_gpio_mask = wake.wake_gpio_mask;
    runtime::CommitSleepSnapshot(snapshot);
    CommitDeepSleep(command);
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
        g_consecutive_sleep_cycles = snapshot.consecutive_cycles;
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
