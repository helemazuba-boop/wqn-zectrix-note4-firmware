#include "runtime/wake_context.h"

#include "runtime/sleep_snapshot.h"

namespace {

wqn::runtime::WakeContext g_wake_context;

bool IsResetCacheSafe(esp_reset_reason_t reason)
{
    // A CRC-valid snapshot is necessary but not sufficient: only a reset
    // explicitly reported as deep-sleep resume proves that panel power was
    // removed through PowerCoordinator's committed transaction.
    return reason == ESP_RST_DEEPSLEEP;
}

}  // namespace

namespace wqn::runtime {

void CaptureWakeContext(
    uint64_t user_input_mask,
    uint64_t rtc_interrupt_mask,
    bool pcf_flags_valid,
    bool pcf_alarm,
    bool pcf_timer)
{
    WakeContext context;
    context.raw_cause = esp_sleep_get_wakeup_cause();
    context.reset_reason = esp_reset_reason();
    context.ext1_status = esp_sleep_get_ext1_wakeup_status();
    context.undefined_wakeup = context.raw_cause == ESP_SLEEP_WAKEUP_UNDEFINED;
    context.pcf_flags_valid = pcf_flags_valid;
    context.pcf_alarm = pcf_alarm;
    context.pcf_timer = pcf_timer;
    SleepSnapshot sleep_snapshot;
    context.sleep_snapshot_valid = LoadSleepSnapshot(&sleep_snapshot);
    if (context.sleep_snapshot_valid) {
        context.sleep_generation = sleep_snapshot.generation;
        context.consecutive_sleep_cycles = sleep_snapshot.consecutive_cycles;
        context.requested_timer_wakeup = sleep_snapshot.timer_wakeup_enabled;
        context.requested_display_timer_wakeup =
            sleep_snapshot.timer_wakeup_for_display;
    }

    if (context.raw_cause == ESP_SLEEP_WAKEUP_TIMER) {
        context.kind = WakeKind::kScheduledTimer;
        context.deep_sleep_resume = true;
    } else if (context.raw_cause == ESP_SLEEP_WAKEUP_EXT1) {
        context.deep_sleep_resume = true;
        if ((context.ext1_status & user_input_mask) != 0) {
            context.kind = WakeKind::kUserInput;
        } else if ((context.ext1_status & rtc_interrupt_mask) != 0) {
            context.kind = pcf_flags_valid && (pcf_alarm || pcf_timer)
                ? WakeKind::kScheduledTimer
                : WakeKind::kUnknown;
        } else {
            context.kind = WakeKind::kExternal;
        }
    } else if (context.raw_cause == ESP_SLEEP_WAKEUP_EXT0 ||
               context.raw_cause == ESP_SLEEP_WAKEUP_GPIO ||
               context.raw_cause == ESP_SLEEP_WAKEUP_UART ||
               context.raw_cause == ESP_SLEEP_WAKEUP_TOUCHPAD ||
               context.raw_cause == ESP_SLEEP_WAKEUP_ULP) {
        context.kind = WakeKind::kUserInput;
        context.deep_sleep_resume = true;
    } else if (context.raw_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // USB/JTAG resets can obscure the original wake cause on this board,
        // but guessing "timer" can suppress a required refresh. Treat missing
        // evidence as a conservative cold/unknown boot.
        if (context.reset_reason == ESP_RST_POWERON) {
            context.kind = WakeKind::kColdBoot;
        } else if (context.reset_reason == ESP_RST_USB || context.reset_reason == ESP_RST_JTAG) {
            context.kind = WakeKind::kUsbReset;
        } else if (context.reset_reason == ESP_RST_BROWNOUT ||
                   context.reset_reason == ESP_RST_PWR_GLITCH) {
            context.kind = WakeKind::kPowerFault;
        } else {
            context.kind = WakeKind::kUnknown;
        }
    } else {
        context.kind = WakeKind::kExternal;
        context.deep_sleep_resume = true;
    }

    context.panel_cache_trusted =
        context.deep_sleep_resume && context.sleep_snapshot_valid &&
        IsResetCacheSafe(context.reset_reason);
    g_wake_context = context;
}

const WakeContext& GetWakeContext()
{
    // app_main captures explicitly. The default is intentionally untrusted
    // so an accidental early read cannot skip a physical refresh.
    return g_wake_context;
}

const char* WakeKindName(WakeKind kind)
{
    switch (kind) {
        case WakeKind::kColdBoot:
            return "cold-boot";
        case WakeKind::kScheduledTimer:
            return "scheduled-timer";
        case WakeKind::kUserInput:
            return "user-input";
        case WakeKind::kExternal:
            return "external";
        case WakeKind::kUsbReset:
            return "usb-reset";
        case WakeKind::kPowerFault:
            return "power-fault";
        case WakeKind::kUnknown:
        default:
            return "unknown";
    }
}

}  // namespace wqn::runtime
