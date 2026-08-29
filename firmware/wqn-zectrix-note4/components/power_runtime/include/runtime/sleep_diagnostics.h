#pragma once

#include <cstddef>
#include <cstdint>

namespace wqn::runtime {

enum class SleepDiagnosticEventKind : uint8_t {
    kBoot = 1,
    kPowerPolicy,
    kAdmissionBlocked,
    kWakePlan,
    kRollback,
    kCommit,
};

enum SleepDiagnosticFlag : uint16_t {
    kSleepDiagUsbHost = 1U << 0,
    kSleepDiagCharging = 1U << 1,
    kSleepDiagFull = 1U << 2,
    kSleepDiagUsbLease = 1U << 3,
    kSleepDiagDeepResume = 1U << 4,
    kSleepDiagTimerRequested = 1U << 5,
    kSleepDiagDisplayTimer = 1U << 6,
    kSleepDiagWakeFloorApplied = 1U << 7,
    kSleepDiagSyncEscalated = 1U << 8,
    kSleepDiagUsableToken = 1U << 9,
    kSleepDiagQuiescing = 1U << 10,
};

// Fixed-size event copied into an individually CRC-protected RTC slow-memory
// no-init ring. It survives deep sleep and conventional software resets, but
// ESP32-S3 USB_UART_CHIP_RESET destroys RTC slow memory, so export must use a
// no-reset monitor. Callers supply board/policy observations so power_runtime
// remains independent of the Note4 GPIO and service layers.
struct SleepDiagnosticEvent {
    SleepDiagnosticEventKind kind = SleepDiagnosticEventKind::kBoot;
    uint8_t wake_kind = 0;
    uint8_t reset_reason = 0;
    uint8_t sleep_mode = 0;
    uint8_t charge_full_gpio = 0;
    uint8_t charge_detect_gpio = 0;
    uint8_t connectivity_state = 0;
    uint8_t connectivity_demand_count = 0;
    uint8_t connectivity_demand_priority = 0;
    uint8_t reserved = 0;
    uint16_t flags = 0;
    uint16_t battery_mv = 0;
    uint32_t generation = 0;
    uint32_t app_uptime_ms = 0;
    uint32_t wall_time_sec = 0;
    uint32_t blocker_mask = 0;
    uint32_t connectivity_demand_mask = 0;
    uint32_t display_wake_sec = 0;
    uint32_t sync_wake_sec = 0;
    uint32_t chosen_wake_sec = 0;
    uint32_t radio_on_total_ms = 0;
    uint32_t retry_ms = 0;
    uint32_t consecutive_cycles = 0;
    char reason[16] = {};
};

constexpr size_t kSleepDiagnosticCapacity = 64;

// Recording is silent so battery-only operation does not depend on a live
// console. Dumping is deliberately explicit and is triggered by the Note4
// power policy only after USB SOF proves that a logging host is connected.
void RecordSleepDiagnosticEvent(const SleepDiagnosticEvent& event);
void DumpSleepDiagnosticsToLog();

}  // namespace wqn::runtime
