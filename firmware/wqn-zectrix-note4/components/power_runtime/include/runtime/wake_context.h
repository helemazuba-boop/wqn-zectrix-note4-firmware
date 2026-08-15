#pragma once

#include <cstdint>

#include "esp_sleep.h"
#include "esp_system.h"

namespace wqn::runtime {

enum class WakeKind : uint8_t {
    kColdBoot = 0,
    kScheduledTimer,
    kUserInput,
    kExternal,
    kUsbReset,
    kPowerFault,
    kUnknown,
};

struct WakeContext {
    esp_sleep_wakeup_cause_t raw_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
    esp_reset_reason_t reset_reason = ESP_RST_UNKNOWN;
    uint64_t ext1_status = 0;
    WakeKind kind = WakeKind::kUnknown;
    bool deep_sleep_resume = false;
    bool undefined_wakeup = true;
    bool pcf_flags_valid = false;
    bool pcf_alarm = false;
    bool pcf_timer = false;
    bool sleep_snapshot_valid = false;
    uint32_t sleep_generation = 0;
    uint32_t consecutive_sleep_cycles = 0;
    bool requested_timer_wakeup = false;
    bool requested_display_timer_wakeup = false;
    bool panel_cache_trusted = false;
};

// Capture once during app_main before tasks start. Pin masks are supplied by
// the Note4 platform boundary so runtime classification does not own pin IDs.
void CaptureWakeContext(
    uint64_t user_input_mask,
    uint64_t rtc_interrupt_mask,
    bool pcf_flags_valid,
    bool pcf_alarm,
    bool pcf_timer);
const WakeContext& GetWakeContext();
const char* WakeKindName(WakeKind kind);

}  // namespace wqn::runtime
