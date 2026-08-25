#include "runtime/sleep_diagnostics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "power/sleep_protocol.h"
#include "runtime/wake_context.h"

namespace {

constexpr char kTag[] = "wqn_sleep_diag";
constexpr uint32_t kEntryMagic = 0x57514447;  // WQDG
constexpr uint16_t kEntryVersion = 2;

struct RtcSleepDiagnosticEntry {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t sequence;
    wqn::runtime::SleepDiagnosticEvent event;
    uint32_t crc32;
};

// RTC_DATA is reinitialized on software/panic resets, so use RTC_NOINIT for
// the battery-only/deep-sleep history. ESP32-S3 USB_UART_CHIP_RESET (0x15) is
// a documented exception which destroys RTC slow memory; diagnostic export
// must therefore use the repository's IDF Monitor --no-reset wrappers.
// Per-entry magic, version, and CRC make uninitialized/torn contents safe to
// ignore.
RTC_NOINIT_ATTR std::array<
    RtcSleepDiagnosticEntry,
    wqn::runtime::kSleepDiagnosticCapacity> g_sleep_diagnostic_entries;
static_assert(sizeof(g_sleep_diagnostic_entries) <= 6 * 1024);

uint32_t EntryCrc(const RtcSleepDiagnosticEntry& entry)
{
    return esp_rom_crc32_le(
        0,
        reinterpret_cast<const uint8_t*>(&entry),
        offsetof(RtcSleepDiagnosticEntry, crc32));
}

bool IsKnownEvent(wqn::runtime::SleepDiagnosticEventKind kind)
{
    using wqn::runtime::SleepDiagnosticEventKind;
    switch (kind) {
        case SleepDiagnosticEventKind::kBoot:
        case SleepDiagnosticEventKind::kPowerPolicy:
        case SleepDiagnosticEventKind::kAdmissionBlocked:
        case SleepDiagnosticEventKind::kWakePlan:
        case SleepDiagnosticEventKind::kRollback:
        case SleepDiagnosticEventKind::kCommit:
            return true;
        default:
            return false;
    }
}

bool IsValid(const RtcSleepDiagnosticEntry& entry)
{
    return entry.magic == kEntryMagic &&
        entry.version == kEntryVersion &&
        entry.sequence != 0 &&
        IsKnownEvent(entry.event.kind) &&
        entry.crc32 == EntryCrc(entry);
}

bool SequenceAfter(uint32_t lhs, uint32_t rhs)
{
    return static_cast<int32_t>(lhs - rhs) > 0;
}

const char* EventName(wqn::runtime::SleepDiagnosticEventKind kind)
{
    using wqn::runtime::SleepDiagnosticEventKind;
    switch (kind) {
        case SleepDiagnosticEventKind::kBoot:
            return "boot";
        case SleepDiagnosticEventKind::kPowerPolicy:
            return "power-policy";
        case SleepDiagnosticEventKind::kAdmissionBlocked:
            return "admission-blocked";
        case SleepDiagnosticEventKind::kWakePlan:
            return "wake-plan";
        case SleepDiagnosticEventKind::kRollback:
            return "rollback";
        case SleepDiagnosticEventKind::kCommit:
            return "commit-attempt";
        default:
            return "unknown";
    }
}

const char* ResetReasonName(uint8_t raw_reason)
{
    switch (static_cast<esp_reset_reason_t>(raw_reason)) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt-wdt";
        case ESP_RST_TASK_WDT:
            return "task-wdt";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep-sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power-glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu-lockup";
        default:
            return "unknown";
    }
}

}  // namespace

namespace wqn::runtime {

void RecordSleepDiagnosticEvent(const SleepDiagnosticEvent& event)
{
    if (!IsKnownEvent(event.kind)) {
        return;
    }

    uint32_t latest_sequence = 0;
    bool found = false;
    for (const RtcSleepDiagnosticEntry& entry : g_sleep_diagnostic_entries) {
        if (IsValid(entry) &&
            (!found || SequenceAfter(entry.sequence, latest_sequence))) {
            latest_sequence = entry.sequence;
            found = true;
        }
    }

    uint32_t sequence = found ? latest_sequence + 1 : 1;
    if (sequence == 0) {
        sequence = 1;
    }
    const size_t slot = (sequence - 1) % g_sleep_diagnostic_entries.size();

    RtcSleepDiagnosticEntry entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.magic = kEntryMagic;
    entry.version = kEntryVersion;
    entry.sequence = sequence;
    entry.event = event;
    entry.event.reason[sizeof(entry.event.reason) - 1] = '\0';
    entry.crc32 = EntryCrc(entry);
    // Copy the complete object representation, including the zeroed padding
    // covered by the CRC. Member-wise assignment may leave destination
    // padding unchanged and make an otherwise complete record look torn.
    std::memcpy(&g_sleep_diagnostic_entries[slot], &entry, sizeof(entry));
}

void DumpSleepDiagnosticsToLog()
{
    std::array<const RtcSleepDiagnosticEntry*, kSleepDiagnosticCapacity> ordered{};
    size_t count = 0;
    uint32_t boot_count = 0;
    uint32_t blocked_count = 0;
    uint32_t rollback_count = 0;
    uint32_t commit_count = 0;
    uint32_t deep_sleep_boot_count = 0;
    uint32_t software_boot_count = 0;
    uint32_t usb_jtag_boot_count = 0;
    uint32_t fault_boot_count = 0;

    for (const RtcSleepDiagnosticEntry& entry : g_sleep_diagnostic_entries) {
        if (!IsValid(entry)) {
            continue;
        }
        size_t position = count;
        while (position > 0 &&
               SequenceAfter(ordered[position - 1]->sequence, entry.sequence)) {
            ordered[position] = ordered[position - 1];
            --position;
        }
        ordered[position] = &entry;
        ++count;

        switch (entry.event.kind) {
            case SleepDiagnosticEventKind::kBoot:
                ++boot_count;
                switch (static_cast<esp_reset_reason_t>(entry.event.reset_reason)) {
                    case ESP_RST_DEEPSLEEP:
                        ++deep_sleep_boot_count;
                        break;
                    case ESP_RST_SW:
                        ++software_boot_count;
                        break;
                    case ESP_RST_USB:
                    case ESP_RST_JTAG:
                        ++usb_jtag_boot_count;
                        break;
                    case ESP_RST_PANIC:
                    case ESP_RST_INT_WDT:
                    case ESP_RST_TASK_WDT:
                    case ESP_RST_WDT:
                    case ESP_RST_BROWNOUT:
                    case ESP_RST_PWR_GLITCH:
                    case ESP_RST_CPU_LOCKUP:
                        ++fault_boot_count;
                        break;
                    default:
                        break;
                }
                break;
            case SleepDiagnosticEventKind::kAdmissionBlocked:
                ++blocked_count;
                break;
            case SleepDiagnosticEventKind::kRollback:
                ++rollback_count;
                break;
            case SleepDiagnosticEventKind::kCommit:
                ++commit_count;
                break;
            default:
                break;
        }
    }

    ESP_LOGI(
        kTag,
        "sleep-diag begin: records=%u capacity=%u boots=%u deep_boots=%u sw_boots=%u "
        "usb_jtag_boots=%u fault_boots=%u commit_attempts=%u rollbacks=%u blocked=%u",
        static_cast<unsigned>(count),
        static_cast<unsigned>(kSleepDiagnosticCapacity),
        static_cast<unsigned>(boot_count),
        static_cast<unsigned>(deep_sleep_boot_count),
        static_cast<unsigned>(software_boot_count),
        static_cast<unsigned>(usb_jtag_boot_count),
        static_cast<unsigned>(fault_boot_count),
        static_cast<unsigned>(commit_count),
        static_cast<unsigned>(rollback_count),
        static_cast<unsigned>(blocked_count));
    ESP_LOGI(
        kTag,
        "sleep-diag blocker bits: display=0 todo=1 word=2 note=3 sync=4 provisioning=5 "
        "audio=6 ai=7 flash=8 storage=9 connectivity=10 usb=11");

    uint32_t previous_radio_ms = 0;
    bool have_previous_radio = false;
    for (size_t i = 0; i < count; ++i) {
        const RtcSleepDiagnosticEntry& entry = *ordered[i];
        const SleepDiagnosticEvent& event = entry.event;
        const uint32_t radio_delta_ms = have_previous_radio &&
                event.radio_on_total_ms >= previous_radio_ms
            ? event.radio_on_total_ms - previous_radio_ms
            : 0;
        previous_radio_ms = event.radio_on_total_ms;
        have_previous_radio = true;

        ESP_LOGI(
            kTag,
            "sleep-diag seq=%u event=%s app_ms=%u wall=%u wake=%s(%u) reset=%s(%u) mode=%s(%u) "
            "gpio1=%u gpio2=%u batt_mv=%u flags=0x%03x host=%u charging=%u full=%u "
            "usb_lease=%u deep=%u timer=%u display_timer=%u floor=%u escalated=%u token=%u "
            "quiescing=%u blockers=0x%03x gen=%u display_sec=%u sync_sec=%u chosen_sec=%u "
            "radio_ms=%u radio_delta_ms=%u retry_ms=%u cycles=%u reason=%s",
            static_cast<unsigned>(entry.sequence),
            EventName(event.kind),
            static_cast<unsigned>(event.app_uptime_ms),
            static_cast<unsigned>(event.wall_time_sec),
            WakeKindName(static_cast<WakeKind>(event.wake_kind)),
            static_cast<unsigned>(event.wake_kind),
            ResetReasonName(event.reset_reason),
            static_cast<unsigned>(event.reset_reason),
            power::SleepModeName(
                static_cast<power::SleepMode>(event.sleep_mode)),
            static_cast<unsigned>(event.sleep_mode),
            static_cast<unsigned>(event.charge_full_gpio),
            static_cast<unsigned>(event.charge_detect_gpio),
            static_cast<unsigned>(event.battery_mv),
            static_cast<unsigned>(event.flags),
            (event.flags & kSleepDiagUsbHost) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagCharging) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagFull) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagUsbLease) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagDeepResume) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagTimerRequested) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagDisplayTimer) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagWakeFloorApplied) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagSyncEscalated) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagUsableToken) != 0 ? 1U : 0U,
            (event.flags & kSleepDiagQuiescing) != 0 ? 1U : 0U,
            static_cast<unsigned>(event.blocker_mask),
            static_cast<unsigned>(event.generation),
            static_cast<unsigned>(event.display_wake_sec),
            static_cast<unsigned>(event.sync_wake_sec),
            static_cast<unsigned>(event.chosen_wake_sec),
            static_cast<unsigned>(event.radio_on_total_ms),
            static_cast<unsigned>(radio_delta_ms),
            static_cast<unsigned>(event.retry_ms),
            static_cast<unsigned>(event.consecutive_cycles),
            event.reason[0] == '\0' ? "-" : event.reason);
    }
    ESP_LOGI(kTag, "sleep-diag end");
}

}  // namespace wqn::runtime
