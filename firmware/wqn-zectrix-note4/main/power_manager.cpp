#include "power_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
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
#include "runtime/sleep_diagnostics.h"
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

#ifndef CONFIG_WQN_RETAINED_STANDBY_IDLE_MS
#define CONFIG_WQN_RETAINED_STANDBY_IDLE_MS 60000
#endif

#ifndef CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS
#define CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS 300000
#endif

#ifndef CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV
#define CONFIG_WQN_BATTERY_LOW_THRESHOLD_MV 3450
#endif

namespace {

using wqn::DeepSleepUiPolicy;

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

int64_t g_last_user_activity_ms = 0;
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
// g_last_user_activity_ms stays a current-boot monotonic value; retaining it
// across reset would compare timestamps from different esp_timer epochs.
// Both plain shared values use std::atomic_ref so the int64 cannot tear on
// this 32-bit core and the UI-task reset of the RTC cycle counter is safe.
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
std::atomic<DeepSleepUiPolicy> g_deep_sleep_ui_policy{
    DeepSleepUiPolicy::kRetainedStandbyOnly};
// Published by the UI after it has drained all immediate work and switched to
// an event/deadline-driven wait. This is observability/polling policy only:
// retained standby deliberately keeps SleepLease admission open, so any new
// work can wake automatic light sleep and acquire its normal owner lease.
std::atomic<bool> g_retained_standby_ui_ready{false};
std::atomic<bool> g_battery_shutdown_requested{false};
// [power-fix] Set by the settings-page confirm; consumed (and re-set on a
// busy quiesce) by the PowerCoordinator. See RunUserPowerOffShutdown.
std::atomic<bool> g_user_poweroff_requested{false};
// Published by the power task after all non-display deep-sleep admission
// checks pass. The UI task reads only this scalar; it must not call
// HasUsableStoredToken(), which performs a synchronous storage read.
std::atomic<bool> g_deep_sleep_clock_yield{false};
uint32_t g_next_sleep_generation = 1;
std::atomic<int64_t> g_sleep_retry_not_before_us{0};
wqn::runtime::SleepLease g_usb_power_lease;
bool g_usb_power_policy_sampled = false;
bool g_usb_power_policy_invariant_failed = false;
bool g_full_only_charge_status_logged = false;
bool g_sleep_diag_policy_initialized = false;
uint16_t g_sleep_diag_last_policy_flags = 0;
bool g_sleep_diag_dumped_for_host = false;
int64_t g_sleep_diag_dump_not_before_us = 0;
bool g_sleep_diag_admission_blocked = false;
uint32_t g_sleep_diag_last_blocker_mask = 0;

constexpr int64_t kPrepareSleepTimeoutUs = 5 * 1000 * 1000;
constexpr int64_t kSleepDiagnosticUsbDumpDelayUs = 2 * 1000 * 1000;
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
constexpr TickType_t kActiveCoordinatorPollTicks = pdMS_TO_TICKS(1000);
constexpr TickType_t kRetainedCoordinatorPollTicks = pdMS_TO_TICKS(30000);

enum class DeepSleepCommitAbortReason : uint8_t {
    kUserActivity = 0,
    kExternalPower,
    kUiPolicy,
};

constexpr bool DeepSleepAllowedByUiPolicy(DeepSleepUiPolicy policy)
{
    return policy != DeepSleepUiPolicy::kRetainedStandbyOnly;
}

constexpr uint32_t ApplyMinimumWakeFloor(uint32_t seconds, uint32_t floor_seconds)
{
    return seconds != 0 && floor_seconds != 0 && seconds < floor_seconds
        ? floor_seconds
        : seconds;
}

const char* DeepSleepUiPolicyName(DeepSleepUiPolicy policy)
{
    switch (policy) {
        case DeepSleepUiPolicy::kRetainedStandbyOnly:
            return "retained-standby";
        case DeepSleepUiPolicy::kDeepSleepNoDisplayTimer:
            return "deep-background";
    }
    return "unknown";
}

// CHRG_L is evidence that a charger is actively supplying power. /STDBY is
// only a charge-complete status: Note4 HIL shows that it can remain asserted
// after USB removal, so it must not independently own the USB sleep lease.
// A PC connection is detected separately from USB Serial/JTAG SOF traffic.
constexpr bool ShouldBlockSleepForExternalPower(
    bool host_connected,
    bool charging,
    bool /*fully_charged*/)
{
    return host_connected || charging;
}

static_assert(!ShouldBlockSleepForExternalPower(false, false, false));
static_assert(!ShouldBlockSleepForExternalPower(false, false, true));
static_assert(ShouldBlockSleepForExternalPower(true, false, false));
static_assert(ShouldBlockSleepForExternalPower(false, true, false));

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

uint32_t ActiveSleepBlockerMask()
{
    uint32_t mask = 0;
    for (uint8_t value = 0;
         value < static_cast<uint8_t>(wqn::runtime::SleepBlocker::kCount);
         ++value) {
        const auto blocker = static_cast<wqn::runtime::SleepBlocker>(value);
        if (wqn::runtime::ActiveSleepBlockerCount(blocker) != 0) {
            mask |= 1U << value;
        }
    }
    return mask;
}

uint16_t CurrentSleepDiagnosticFlags()
{
    uint16_t flags = 0;
    if (wqn::IsUsbHostConnected()) {
        flags |= wqn::runtime::kSleepDiagUsbHost;
    }
    if (wqn::IsCharging()) {
        flags |= wqn::runtime::kSleepDiagCharging;
    }
    if (wqn::IsFullyCharged()) {
        flags |= wqn::runtime::kSleepDiagFull;
    }
    if (g_usb_power_lease) {
        flags |= wqn::runtime::kSleepDiagUsbLease;
    }
    if (wqn::runtime::GetWakeContext().deep_sleep_resume) {
        flags |= wqn::runtime::kSleepDiagDeepResume;
    }
    if (wqn::runtime::IsSleepQuiescing()) {
        flags |= wqn::runtime::kSleepDiagQuiescing;
    }
    return flags;
}

wqn::runtime::SleepDiagnosticEvent MakeSleepDiagnosticEvent(
    wqn::runtime::SleepDiagnosticEventKind kind)
{
    const wqn::runtime::WakeContext& wake = wqn::runtime::GetWakeContext();
    wqn::runtime::SleepDiagnosticEvent event;
    event.kind = kind;
    event.wake_kind = static_cast<uint8_t>(wake.kind);
    event.reset_reason = static_cast<uint8_t>(wake.reset_reason);
    event.sleep_mode = static_cast<uint8_t>(wake.previous_sleep_mode);
    event.charge_full_gpio = static_cast<uint8_t>(gpio_get_level(kChargeFull));
    event.charge_detect_gpio = static_cast<uint8_t>(gpio_get_level(kChargeDetect));
    event.flags = CurrentSleepDiagnosticFlags();
    event.app_uptime_ms = static_cast<uint32_t>(NowMs());
    const std::time_t wall_time = std::time(nullptr);
    if (wall_time > 0 && static_cast<uint64_t>(wall_time) <= UINT32_MAX) {
        event.wall_time_sec = static_cast<uint32_t>(wall_time);
    }
    event.blocker_mask = ActiveSleepBlockerMask();
    const wqn::services::ConnectivitySnapshot connectivity =
        wqn::services::GetConnectivitySnapshot();
    event.connectivity_state =
        static_cast<uint8_t>(connectivity.state);
    event.connectivity_demand_count = connectivity.demand_count;
    event.connectivity_demand_priority =
        static_cast<uint8_t>(connectivity.demand_priority);
    event.connectivity_demand_mask = connectivity.demand_mask;
    event.radio_on_total_ms = wqn::GetWifiRadioOnTotalMs();
    event.consecutive_cycles =
        ConsecutiveSleepCyclesRef().load(std::memory_order_relaxed);
    return event;
}

void SetSleepDiagnosticReason(
    wqn::runtime::SleepDiagnosticEvent* event,
    const char* reason)
{
    if (event == nullptr || reason == nullptr) {
        return;
    }
    std::strncpy(event->reason, reason, sizeof(event->reason) - 1);
    event->reason[sizeof(event->reason) - 1] = '\0';
}

constexpr uint16_t kSleepDiagnosticPowerPolicyFlags =
    wqn::runtime::kSleepDiagUsbHost |
    wqn::runtime::kSleepDiagCharging |
    wqn::runtime::kSleepDiagFull |
    wqn::runtime::kSleepDiagUsbLease;

void RefreshSleepDiagnosticPowerPolicy()
{
    wqn::runtime::SleepDiagnosticEvent event = MakeSleepDiagnosticEvent(
        wqn::runtime::SleepDiagnosticEventKind::kPowerPolicy);
    const uint16_t policy_flags =
        event.flags & kSleepDiagnosticPowerPolicyFlags;
    if (!g_sleep_diag_policy_initialized ||
        policy_flags != g_sleep_diag_last_policy_flags) {
        event.battery_mv = wqn::GetBatteryVoltageMv();
        SetSleepDiagnosticReason(
            &event, g_sleep_diag_policy_initialized ? "changed" : "initial");
        wqn::runtime::RecordSleepDiagnosticEvent(event);
        g_sleep_diag_last_policy_flags = policy_flags;
        g_sleep_diag_policy_initialized = true;
    }

    const bool host_connected =
        (event.flags & wqn::runtime::kSleepDiagUsbHost) != 0;
    if (!host_connected) {
        g_sleep_diag_dumped_for_host = false;
        g_sleep_diag_dump_not_before_us = 0;
        return;
    }
    if (g_sleep_diag_dumped_for_host) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (g_sleep_diag_dump_not_before_us == 0) {
        // Give the host listener time to open after USB enumeration; the USB
        // policy lease already prevents a sleep during this short delay.
        g_sleep_diag_dump_not_before_us =
            now_us + kSleepDiagnosticUsbDumpDelayUs;
        return;
    }
    if (now_us >= g_sleep_diag_dump_not_before_us) {
        wqn::runtime::DumpSleepDiagnosticsToLog();
        g_sleep_diag_dumped_for_host = true;
    }
}

void ValidateUsbPowerSleepPolicy(
    bool host_connected,
    bool charging,
    bool full)
{
    const bool external_power_present = ShouldBlockSleepForExternalPower(
        host_connected, charging, full);
    const bool lease_active = static_cast<bool>(g_usb_power_lease);
    const uint32_t blocker_count = wqn::runtime::ActiveSleepBlockerCount(
        wqn::runtime::SleepBlocker::kUsbPower);
    const bool ownership_mismatch = lease_active
        ? blocker_count != 1
        : blocker_count != 0;
    const bool stale_without_power =
        !external_power_present && (lease_active || blocker_count != 0);
    const bool invariant_failed = ownership_mismatch || stale_without_power;

    if (invariant_failed && !g_usb_power_policy_invariant_failed) {
        ESP_LOGE(
            kTag,
            "USB sleep policy invariant failed: host=%d charging=%d full=%d lease=%d usb_blockers=%u",
            host_connected ? 1 : 0,
            charging ? 1 : 0,
            full ? 1 : 0,
            lease_active ? 1 : 0,
            static_cast<unsigned>(blocker_count));
        wqn::runtime::SleepDiagnosticEvent event = MakeSleepDiagnosticEvent(
            wqn::runtime::SleepDiagnosticEventKind::kPowerPolicy);
        event.battery_mv = wqn::GetBatteryVoltageMv();
        SetSleepDiagnosticReason(&event, "invariant-fail");
        wqn::runtime::RecordSleepDiagnosticEvent(event);
    } else if (!invariant_failed && g_usb_power_policy_invariant_failed) {
        ESP_LOGI(
            kTag,
            "USB sleep policy invariant recovered: host=%d charging=%d full=%d lease=%d usb_blockers=%u",
            host_connected ? 1 : 0,
            charging ? 1 : 0,
            full ? 1 : 0,
            lease_active ? 1 : 0,
            static_cast<unsigned>(blocker_count));
    }
    g_usb_power_policy_invariant_failed = invariant_failed;
}

}  // namespace

namespace wqn {

void LogWakeupCause()
{
    const runtime::WakeContext& wake = runtime::GetWakeContext();
    ESP_LOGI(kTag,
             "wake context: kind=%s raw=%s(%d) reset=%d ext1=0x%llx pcf_valid=%d "
             "pcf_af=%d pcf_tf=%d sleep_snapshot=%d last_mode=%s "
             "sleep_generation=%u "
             "sleep_cycles=%u timer_requested=%d display_timer=%d panel_cache=%s",
             runtime::WakeKindName(wake.kind), WakeupCauseName(wake.raw_cause),
             static_cast<int>(wake.raw_cause), static_cast<int>(wake.reset_reason),
             static_cast<unsigned long long>(wake.ext1_status),
             wake.pcf_flags_valid ? 1 : 0,
             wake.pcf_alarm ? 1 : 0,
             wake.pcf_timer ? 1 : 0,
             wake.sleep_snapshot_valid ? 1 : 0,
             power::SleepModeName(wake.previous_sleep_mode),
             static_cast<unsigned>(wake.sleep_generation),
             static_cast<unsigned>(wake.consecutive_sleep_cycles),
             wake.requested_timer_wakeup ? 1 : 0,
             wake.requested_display_timer_wakeup ? 1 : 0,
             wake.panel_cache_trusted ? "trusted" : "untrusted");

    runtime::SleepDiagnosticEvent event = MakeSleepDiagnosticEvent(
        runtime::SleepDiagnosticEventKind::kBoot);
    event.generation = wake.sleep_generation;
    event.battery_mv = GetBatteryVoltageMv();
    if (wake.requested_timer_wakeup) {
        event.flags |= runtime::kSleepDiagTimerRequested;
    }
    if (wake.requested_display_timer_wakeup) {
        event.flags |= runtime::kSleepDiagDisplayTimer;
    }
    runtime::RecordSleepDiagnosticEvent(event);
    g_sleep_diag_last_policy_flags =
        event.flags & kSleepDiagnosticPowerPolicyFlags;
    g_sleep_diag_policy_initialized = true;
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
    // The button producer runs before the event enters the UI ring. Wake the
    // coordinator at the same linearization point so a retained-standby poll
    // cannot remain parked for its longer diagnostic interval after input.
    if (g_power_coordinator_task != nullptr) {
        xTaskNotifyGive(g_power_coordinator_task);
    }
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

static bool IsUiIdleForThresholdMs(int threshold_ms)
{
    // If we woke up by a timer and there has been no user interaction in this boot session,
    // we should sleep immediately.
    if (runtime::GetWakeContext().kind == runtime::WakeKind::kScheduledTimer &&
        !g_user_interacted_current_boot.load(std::memory_order_relaxed)) {
        return true;
    }

    const int64_t now_ms = NowMs();
    const int64_t last_activity_ms =
        UserActivityMsRef().load(std::memory_order_relaxed);
    // [power-fix] Only the last user activity drives the deep-sleep idle
    // timer. NoteEpdActivity() runs on the clock screen's minute rollover, so
    // including it in `std::max(user, epd)` made the threshold unreachable and
    // permanently pinned the
    // device in active mode. EPD activity is still tracked separately for
    // the EPD rail power-off path.
    return now_ms >= last_activity_ms &&
        (now_ms - last_activity_ms) >= threshold_ms;
}

bool IsUiIdleForSleepEx(int extra_idle_ms)
{
    int threshold_ms = CONFIG_WQN_DEEP_SLEEP_IDLE_MS;
    /* Temporarily commented out for fast testing/verification over USB
    if (IsCharging()) {
        threshold_ms += CONFIG_WQN_CHARGING_DEEP_SLEEP_EXTRA_MS;
    }
    */
    threshold_ms += extra_idle_ms;
    return IsUiIdleForThresholdMs(threshold_ms);
}

bool IsUiIdleForRetainedStandby()
{
    return IsUiIdleForThresholdMs(CONFIG_WQN_RETAINED_STANDBY_IDLE_MS);
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
    snapshot->usb_host_connected = IsUsbHostConnected();
    snapshot->charging = gpio_get_level(kChargeDetect) == 0;
    snapshot->fully_charged = gpio_get_level(kChargeFull) == 0;
    snapshot->external_power_present = ShouldBlockSleepForExternalPower(
        snapshot->usb_host_connected,
        snapshot->charging,
        snapshot->fully_charged);
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
    // /STDBY is trustworthy as charge-complete status only while another
    // signal confirms external power. HIL shows it can remain asserted after
    // cable removal; in that state the ADC curve must remain authoritative.
    snapshot->battery_percent =
        snapshot->fully_charged && snapshot->external_power_present
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
    return ShouldBlockSleepForExternalPower(
        IsUsbHostConnected(), IsCharging(), IsFullyCharged());
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
    const bool external_power_present = ShouldBlockSleepForExternalPower(
        host_connected, charging, full);

    const bool full_only = full && !external_power_present;
    if (full_only && !g_full_only_charge_status_logged) {
        ESP_LOGW(
            kTag,
            "/STDBY asserted without USB SOF or active charging; treating full=1 as status-only and allowing sleep");
    }
    g_full_only_charge_status_logged = full_only;

    if (external_power_present && !g_usb_power_lease) {
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
            RefreshSleepDiagnosticPowerPolicy();
            return;
        }
        g_usb_power_lease = std::move(lease);
        ESP_LOGI(
            kTag,
            "USB/charger detected: host=%d charging=%d full=%d; light/deep sleep blocked",
            host_connected ? 1 : 0,
            charging ? 1 : 0,
            full ? 1 : 0);
    } else if (!external_power_present && g_usb_power_lease) {
        const uint32_t blocker_count_before = runtime::ActiveSleepBlockerCount(
            runtime::SleepBlocker::kUsbPower);
        g_usb_power_lease.Reset();
        const uint32_t blocker_count_after = runtime::ActiveSleepBlockerCount(
            runtime::SleepBlocker::kUsbPower);
        ESP_LOGI(
            kTag,
            "USB/charger removed: host=%d charging=%d full=%d; lease released usb_blockers=%u->%u",
            host_connected ? 1 : 0,
            charging ? 1 : 0,
            full ? 1 : 0,
            static_cast<unsigned>(blocker_count_before),
            static_cast<unsigned>(blocker_count_after));
    } else if (!g_usb_power_policy_sampled && !external_power_present) {
        ESP_LOGI(
            kTag,
            "USB/charger not detected: host=0 charging=0 full=%d; normal sleep policy active",
            full ? 1 : 0);
    }
    g_usb_power_policy_sampled = true;
    ValidateUsbPowerSleepPolicy(host_connected, charging, full);
    RefreshSleepDiagnosticPowerPolicy();
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
        std::strncmp(reason, "user-activity", 13) == 0 ||
        std::strncmp(reason, "ui-policy", 9) == 0;
    if (user_activity_rollback) {
        g_sleep_retry_escalation = 0;
    } else if (g_sleep_retry_escalation <
               static_cast<uint8_t>(kSleepRetryBackoffLadderSize - 1)) {
        ++g_sleep_retry_escalation;
    }
    const int64_t retry_backoff_us = SleepRetryBackoffUs();
    runtime::SleepDiagnosticEvent diagnostic = MakeSleepDiagnosticEvent(
        runtime::SleepDiagnosticEventKind::kRollback);
    diagnostic.generation = generation;
    diagnostic.retry_ms = static_cast<uint32_t>(retry_backoff_us / 1000);
    SetSleepDiagnosticReason(&diagnostic, reason);
    runtime::RecordSleepDiagnosticEvent(diagnostic);
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
// (the same critical section NoteUserActivity publishes through). The idle
// path also performs the last external-power and UI-policy samples in that
// final critical section. Returns only when the sleep was aborted; the caller
// must roll back according to the returned reason. The battery-emergency path
// passes nullptr: it must power down regardless of input, UI policy or power.
static DeepSleepCommitAbortReason CommitDeepSleep(
    const power::PrepareSleepCommand& command,
    const uint32_t* gate_on_activity_baseline)
{
    runtime::SleepDiagnosticEvent diagnostic = MakeSleepDiagnosticEvent(
        runtime::SleepDiagnosticEventKind::kCommit);
    diagnostic.generation = command.generation;
    diagnostic.sleep_mode = static_cast<uint8_t>(command.mode);
    diagnostic.battery_mv = GetBatteryVoltageMv();
    runtime::RecordSleepDiagnosticEvent(diagnostic);
    ESP_LOGI(kTag, "deep-sleep commit: generation=%u mode=%s consecutive=%u stack_free=%u radio_on_total_ms=%u",
             static_cast<unsigned>(command.generation),
             power::SleepModeName(command.mode),
             static_cast<unsigned>(ConsecutiveSleepCyclesRef().load(std::memory_order_relaxed)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(wqn::GetWifiRadioOnTotalMs()));
    uart_wait_tx_idle_polling(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM));
    vTaskDelay(pdMS_TO_TICKS(50));

    // The idle path validates activity and external power one last time under
    // g_activity_gate. usb_serial_jtag_is_connected() is a non-blocking read
    // of the IDF connection monitor's volatile SOF state, so this check does
    // not introduce a wait inside the critical section. The emergency path
    // (nullptr baseline) sleeps unconditionally. Both converge on the SINGLE
    // deep-sleep entry below (the M8 gate enforces this firmware-wide).
    if (gate_on_activity_baseline != nullptr) {
        taskENTER_CRITICAL(&g_activity_gate);
        if (g_user_activity_generation.load(std::memory_order_acquire) !=
            *gate_on_activity_baseline) {
            taskEXIT_CRITICAL(&g_activity_gate);
            return DeepSleepCommitAbortReason::kUserActivity;
        }
        if (IsUsbPowered()) {
            taskEXIT_CRITICAL(&g_activity_gate);
            return DeepSleepCommitAbortReason::kExternalPower;
        }
        if (!DeepSleepAllowedByUiPolicy(
                g_deep_sleep_ui_policy.load(std::memory_order_acquire))) {
            taskEXIT_CRITICAL(&g_activity_gate);
            return DeepSleepCommitAbortReason::kUiPolicy;
        }
        // The gate is held THROUGH the deep-sleep entry (which never returns):
        // a racing NoteUserActivity can only publish after the key press has
        // become an armed wake source.
    }
    // noreturn: nothing after this call is reachable.
    esp_deep_sleep_start();
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

// [power-fix] User-initiated power-off (settings page). Same hard power-cut
// as the battery-emergency path, but the panel is whited FIRST so the device
// visibly shuts down instead of freezing its last screen. The display clear
// runs on the EPD owner task via PrepareDisplayForShutdown; a display fault
// is logged and never blocks the shutdown. Runs with quiesce closed, so no
// new leases (sync/audio/AI) can start mid-sequence.
static void RunUserPowerOffShutdown()
{
    ESP_LOGW(kTag, "user power-off requested");
    const uint32_t generation = NextSleepGeneration();
    if (!runtime::BeginEmergencySleepQuiesce(generation)) {
        // Something still holds a lease (AI session, sync round, portal).
        // Re-arm and retry on the next coordinator tick rather than force-
        // cutting under live work; the UI notice keeps the user informed.
        g_user_poweroff_requested.store(true, std::memory_order_release);
        return;
    }

    const int64_t storage_deadline_us = esp_timer_get_time() + kEmergencyStorageTimeoutUs;
    while (runtime::ActiveSleepBlockerCount(runtime::SleepBlocker::kStorage) != 0 &&
           esp_timer_get_time() < storage_deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Bound how long the request may wait to be claimed by the EPD owner.
    // Once claimed, the hardware operation is non-cancellable and this caller
    // waits for its bounded terminal result before cutting the board latch.
    constexpr int64_t kShutdownClearClaimDeadlineUs = 12LL * 1000 * 1000;
    const esp_err_t clear_result =
        wqn::PrepareDisplayForShutdown(
            esp_timer_get_time() + kShutdownClearClaimDeadlineUs);
    if (clear_result != ESP_OK) {
        ESP_LOGW(kTag, "shutdown display clear failed; continuing: %s",
                 esp_err_to_name(clear_result));
    }

    power::PrepareSleepCommand command;
    command.generation = generation;
    command.mode = power::SleepMode::kBatteryEmergency;
    command.deadline_us = esp_timer_get_time() + kEmergencyHardwareTimeoutUs;
    const power::PrepareSleepResults results = BroadcastPrepareSleep(command);
    for (const power::PrepareSleepResult& result : results) {
        if (result.status != power::SleepPrepareStatus::kReady) {
            ESP_LOGW(kTag, "user power-off continuing after service failure: service=%s status=%s",
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

static void EnterDeepSleepIfEnabled(DeepSleepUiPolicy ui_policy)
{
#if CONFIG_WQN_DEEP_SLEEP_ENABLE
    if (g_battery_shutdown_requested.exchange(false, std::memory_order_acq_rel) ||
        (IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered())) {
        RunBatteryEmergencyShutdown();
        return;
    }
    if (!DeepSleepAllowedByUiPolicy(ui_policy)) {
        g_deep_sleep_clock_yield.store(false, std::memory_order_release);
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
        const uint32_t blocker_mask = ActiveSleepBlockerMask();
        if (!g_sleep_diag_admission_blocked ||
            blocker_mask != g_sleep_diag_last_blocker_mask) {
            runtime::SleepDiagnosticEvent diagnostic = MakeSleepDiagnosticEvent(
                runtime::SleepDiagnosticEventKind::kAdmissionBlocked);
            diagnostic.generation = generation;
            diagnostic.blocker_mask = blocker_mask;
            SetSleepDiagnosticReason(
                &diagnostic,
                runtime::IsSleepQuiescing() ? "quiescing" : "active-leases");
            runtime::RecordSleepDiagnosticEvent(diagnostic);
            g_sleep_diag_admission_blocked = true;
            g_sleep_diag_last_blocker_mask = blocker_mask;
        }
        return;
    }
    g_sleep_diag_admission_blocked = false;
    // ...and again right after: a bump inside this window means an armed
    // effect may just have failed its reserve against the closed gate.
    if (g_user_activity_generation.load(std::memory_order_acquire) !=
        activity_generation_before) {
        RollbackSleepPreparation(generation, "user-activity-during-quiesce");
        return;
    }
    if (!DeepSleepAllowedByUiPolicy(
            g_deep_sleep_ui_policy.load(std::memory_order_acquire))) {
        RollbackSleepPreparation(generation, "ui-policy-during-quiesce");
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
    // No current UI state has a durable hibernate/display-wake contract.
    // Deep sleep is limited to background provisioning maintenance; paired
    // application pages remain in retained standby.
    constexpr uint32_t display_wakeup_seconds = 0;
    uint32_t sync_wakeup_seconds = services::SecondsUntilNextSyncWake();
    uint32_t effective_sync_wakeup_seconds = sync_wakeup_seconds;
    uint32_t timer_wakeup_seconds = display_wakeup_seconds;
    // [power-fix] Without a usable identity there is nothing to sync; ignore
    // sync-derived deadlines (claim polling would otherwise pin the cadence
    // at ~1s) and hold a slow 15-minute maintenance rhythm instead.
    if (!has_usable_token) {
        sync_wakeup_seconds = 0;
        effective_sync_wakeup_seconds = 0;
        timer_wakeup_seconds =
            kUnpairedBatteryMaintenanceWakeSec;
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
    uint32_t wake_floor_seconds = CONFIG_WQN_SLEEP_TIMER_WAKE_FLOOR_SEC;
    bool wake_floor_applied = false;
    if (has_usable_token) {
        const uint32_t floored_sync = ApplyMinimumWakeFloor(
            effective_sync_wakeup_seconds, wake_floor_seconds);
        wake_floor_applied = floored_sync != effective_sync_wakeup_seconds;
        effective_sync_wakeup_seconds = floored_sync;
    }
    // Compare display against the already floor-clamped sync deadline. A raw
    // sync retry at 1 s and a display deadline at 60 s both become due at 60 s;
    // display wins that tie and must never be counted as unattended sync.
    constexpr bool timer_wakeup_for_display = false;
    // [gap-1] Unattended background-maintenance wakes escalate: after enough
    // consecutive sync-source cycles with zero user interaction, widen the
    // floor so a stuck deadline can burn at most one radio window per 15
    // minutes instead of one per interval. Display/clock wakes neither count
    // nor reset the streak.
    bool sync_wake_escalated = false;
    if (has_usable_token && !timer_wakeup_for_display &&
        effective_sync_wakeup_seconds != 0 &&
        g_unattended_sync_wakes >= kUnattendedSyncWakeEscalationAfter &&
        wake_floor_seconds != 0 &&
        wake_floor_seconds < kEscalatedSyncWakeFloorSec) {
        effective_sync_wakeup_seconds = std::max(
            effective_sync_wakeup_seconds, kEscalatedSyncWakeFloorSec);
        sync_wake_escalated = true;
        wake_floor_applied = true;
    }
    if (has_usable_token) {
        if (timer_wakeup_for_display) {
            timer_wakeup_seconds = display_wakeup_seconds;
        } else {
            timer_wakeup_seconds = effective_sync_wakeup_seconds;
        }
    }
    ESP_LOGI(kTag,
             "deep-sleep wake plan: display_sec=%u sync_sec=%u sync_effective_sec=%u chosen=%u "
             "source=%s%s%s",
             static_cast<unsigned>(display_wakeup_seconds),
             static_cast<unsigned>(sync_wakeup_seconds),
             static_cast<unsigned>(effective_sync_wakeup_seconds),
             static_cast<unsigned>(timer_wakeup_seconds),
             !has_usable_token ? "unpaired-maintenance"
                 : (timer_wakeup_for_display ? "display"
                 : (sync_wakeup_seconds != 0 ? "sync" : "off")),
             wake_floor_applied && !timer_wakeup_for_display ? " floor-clamped" : "",
             sync_wake_escalated ? "+unattended-escalated" : "");
    runtime::SleepDiagnosticEvent wake_diagnostic = MakeSleepDiagnosticEvent(
        runtime::SleepDiagnosticEventKind::kWakePlan);
    wake_diagnostic.generation = generation;
    wake_diagnostic.sleep_mode = static_cast<uint8_t>(command.mode);
    wake_diagnostic.display_wake_sec = display_wakeup_seconds;
    wake_diagnostic.sync_wake_sec = sync_wakeup_seconds;
    wake_diagnostic.chosen_wake_sec = timer_wakeup_seconds;
    if (timer_wakeup_seconds != 0) {
        wake_diagnostic.flags |= runtime::kSleepDiagTimerRequested;
    }
    if (timer_wakeup_for_display) {
        wake_diagnostic.flags |= runtime::kSleepDiagDisplayTimer;
    }
    if (wake_floor_applied && !timer_wakeup_for_display) {
        wake_diagnostic.flags |= runtime::kSleepDiagWakeFloorApplied;
    }
    if (sync_wake_escalated) {
        wake_diagnostic.flags |= runtime::kSleepDiagSyncEscalated;
    }
    if (has_usable_token) {
        wake_diagnostic.flags |= runtime::kSleepDiagUsableToken;
    }
    SetSleepDiagnosticReason(
        &wake_diagnostic,
        !has_usable_token ? "unpaired"
            : (timer_wakeup_for_display ? "display"
            : (sync_wakeup_seconds != 0 ? "sync" : "off")));
    runtime::RecordSleepDiagnosticEvent(wake_diagnostic);
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
    // A USB host or active charger can arrive while services are preparing.
    // The policy lease cannot be reacquired while quiesce admission is closed,
    // so observe the physical source directly and roll the transaction back.
    if (IsUsbPowered()) {
        RollbackSleepPreparation(generation, "usb-power-during-prepare");
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
    if (!DeepSleepAllowedByUiPolicy(
            g_deep_sleep_ui_policy.load(std::memory_order_acquire))) {
        RollbackSleepPreparation(generation, "ui-policy-before-commit");
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
    const DeepSleepCommitAbortReason abort_reason =
        CommitDeepSleep(command, &activity_generation_before);
    // An abort can land after this cycle's fetch_add and snapshot commit.
    // Re-zero so a correctly-cancelled sleep never leaves the consecutive
    // counter at 1; rollback also invalidates the staged snapshot.
    ConsecutiveSleepCyclesRef().store(0, std::memory_order_relaxed);
    RollbackSleepPreparation(
        generation,
        abort_reason == DeepSleepCommitAbortReason::kExternalPower
            ? "usb-power-at-commit"
            : (abort_reason == DeepSleepCommitAbortReason::kUiPolicy
                ? "ui-policy-at-commit"
                : "user-activity-at-commit"));
#else
    (void)ui_policy;
#endif
}

void SetDeepSleepUiPolicy(DeepSleepUiPolicy policy)
{
    bool changed = false;
    taskENTER_CRITICAL(&g_activity_gate);
    const DeepSleepUiPolicy previous =
        g_deep_sleep_ui_policy.load(std::memory_order_relaxed);
    if (previous != policy) {
        g_deep_sleep_ui_policy.store(policy, std::memory_order_release);
        changed = true;
    }
    taskEXIT_CRITICAL(&g_activity_gate);
    if (!changed) {
        return;
    }
    ESP_LOGI(kTag, "UI sleep policy: %s", DeepSleepUiPolicyName(policy));
    if (g_power_coordinator_task != nullptr) {
        xTaskNotifyGive(g_power_coordinator_task);
    }
}

void SetRetainedStandbyUiReady(bool ready)
{
    const bool previous = g_retained_standby_ui_ready.exchange(
        ready, std::memory_order_acq_rel);
    if (previous == ready) {
        return;
    }
    ESP_LOGI(kTag, "retained standby UI: %s", ready ? "ready" : "active");
    runtime::SleepDiagnosticEvent diagnostic = MakeSleepDiagnosticEvent(
        runtime::SleepDiagnosticEventKind::kPowerPolicy);
    SetSleepDiagnosticReason(
        &diagnostic, ready ? "retained-enter" : "retained-exit");
    runtime::RecordSleepDiagnosticEvent(diagnostic);
    if (g_power_coordinator_task != nullptr) {
        xTaskNotifyGive(g_power_coordinator_task);
    }
}

void RequestUserPowerOff()
{
    ESP_LOGI(kTag, "user power-off requested from UI");
    g_user_poweroff_requested.store(true, std::memory_order_release);
}

static void PowerCoordinatorTask(void*)
{
    ESP_LOGI(kTag, "power coordinator task started: stack_free=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    while (true) {
        RefreshUsbPowerSleepPolicy();
        runtime::LogLongHeldSleepLeases(esp_timer_get_time(), kLeaseWarningAfterUs);
        if (g_user_poweroff_requested.exchange(false, std::memory_order_acq_rel)) {
            // Consumes the flag; RunUserPowerOffShutdown re-sets it and
            // returns only when quiesce was busy, so the retry is one tick
            // later and never spins hot.
            RunUserPowerOffShutdown();
        }
        EnterDeepSleepIfEnabled(
            g_deep_sleep_ui_policy.load(std::memory_order_acquire));
        const TickType_t wait_ticks =
            g_retained_standby_ui_ready.load(std::memory_order_acquire)
            ? kRetainedCoordinatorPollTicks
            : kActiveCoordinatorPollTicks;
        ulTaskNotifyTake(pdTRUE, wait_ticks);
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
    // A monotonic esp_timer timestamp is meaningful only within this boot.
    // Treat startup as the initial activity epoch so an untouched device can
    // enter retained standby after the normal idle threshold.
    UserActivityMsRef().store(NowMs(), std::memory_order_relaxed);
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
