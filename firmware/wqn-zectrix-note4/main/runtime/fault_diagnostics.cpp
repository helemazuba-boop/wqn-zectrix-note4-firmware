#include "runtime/fault_diagnostics.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <type_traits>

#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "runtime/wall_clock.h"
#include "sdkconfig.h"

namespace wqn::runtime {
namespace {
constexpr char kTag[] = "wqn_fault";
constexpr uint32_t kMagic = 0x57464C54; // WFLT
constexpr uint32_t kVersion = 1;
struct Journal {
    uint32_t magic;
    uint32_t version;
    uint32_t revision;
    uint32_t next;
    uint32_t fault_sequence;
    uint32_t reserved;
    FaultDiagnosticsSnapshot view;
    uint32_t crc;
};
static_assert(std::is_trivial_v<Journal> && std::is_standard_layout_v<Journal>);
static_assert(sizeof(Journal) * 2 <= 1024);
RTC_NOINIT_ATTR Journal g_copies[2];
Journal g_current{};
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_initialized = false; // all accesses under g_lock
unsigned g_active_copy = 1;

uint32_t Crc(const Journal& journal)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&journal), offsetof(Journal, crc));
}

uint32_t Increment(uint32_t value) { return value == UINT32_MAX ? value : value + 1; }
uint32_t NextId(uint32_t value) { return value == UINT32_MAX ? 1 : value + 1; }
bool After(uint32_t lhs, uint32_t rhs) { return static_cast<int32_t>(lhs - rhs) > 0; }

bool Valid(const Journal& journal)
{
    if (journal.magic != kMagic || journal.version != kVersion || journal.revision == 0 ||
        journal.crc != Crc(journal) || journal.next >= kFaultRecordDepth ||
        journal.view.count > kFaultRecordDepth || journal.view.boot_id == 0 ||
        journal.view.boot_phase > BootPhase::kStartupComplete ||
        journal.view.previous_boot_phase > BootPhase::kStartupComplete ||
        journal.view.build[15] != '\0' || journal.view.previous_build[15] != '\0') return false;
    for (const auto& record : journal.view.records) {
        if (record.build[15] != '\0' || record.domain > FaultDomain::kSync ||
            record.phase > FaultPhase::kStorageQueueTimeout ||
            record.boot_phase > static_cast<uint8_t>(BootPhase::kStartupComplete) ||
            record.clock_source > static_cast<uint8_t>(WallClockSource::kRtcSntp) ||
            record.rtc_continuity > 1) return false;
    }
    return true;
}

bool Nonempty(const Journal& journal)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&journal);
    for (size_t i = 0; i < sizeof(journal); ++i) if (bytes[i]) return true;
    return false;
}

bool MayRetain(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_SW: case ESP_RST_PANIC: case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT: case ESP_RST_WDT: case ESP_RST_DEEPSLEEP:
        case ESP_RST_CPU_LOCKUP: return true;
        default: return false; // cold/brownout/USB/JTAG/unknown cannot establish retention
    }
}

void CommitLocked()
{
    g_current.magic = kMagic;
    g_current.version = kVersion;
    g_current.revision = NextId(g_current.revision);
    g_current.crc = Crc(g_current);
    const unsigned target = 1 - g_active_copy;
    // Preserve the other complete copy. Invalidate before changing the body,
    // and publish the aligned marker only after body/CRC are in RTC memory.
    auto* marker = reinterpret_cast<volatile uint32_t*>(&g_copies[target].magic);
    *marker = 0;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::memcpy(reinterpret_cast<uint8_t*>(&g_copies[target]) + sizeof(uint32_t),
                reinterpret_cast<const uint8_t*>(&g_current) + sizeof(uint32_t),
                sizeof(Journal) - sizeof(uint32_t));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    *marker = kMagic;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    g_active_copy = target;
}

void InitLocked()
{
    if (g_initialized) return;
    const auto reason = esp_reset_reason();
    bool found = false;
    uint32_t rejected = 0;
    if (MayRetain(reason)) {
        Journal candidate;
        for (unsigned i = 0; i < 2; ++i) {
            std::memcpy(&candidate, &g_copies[i], sizeof(candidate));
            if (Valid(candidate)) {
                if (!found || After(candidate.revision, g_current.revision)) {
                    std::memcpy(&g_current, &candidate, sizeof(g_current));
                    g_active_copy = i;
                    found = true;
                }
            } else if (Nonempty(candidate)) {
                ++rejected;
            }
        }
    }
    if (!found) {
        std::memset(&g_current, 0, sizeof(g_current));
        std::memset(g_copies, 0, sizeof(g_copies));
        g_active_copy = 1;
    }
    auto& view = g_current.view;
    view.previous_boot_id = found ? view.boot_id : 0;
    view.previous_boot_phase = view.boot_phase;
    std::memcpy(view.previous_build, view.build, sizeof(view.previous_build));
    view.boot_id = NextId(view.boot_id);
    view.reset_reason = static_cast<uint32_t>(reason);
    view.boot_phase = BootPhase::kPlatform;
    view.recovered = found ? 1 : 0;
    for (uint32_t i = 0; i < rejected; ++i) view.integrity_rejections = Increment(view.integrity_rejections);
    std::memset(view.build, 0, sizeof(view.build));
    std::strncpy(view.build, WQN_GIT_COMMIT, sizeof(view.build) - 1);
    CommitLocked();
    g_initialized = true;
}
} // namespace

void InitFaultDiagnostics()
{
    taskENTER_CRITICAL(&g_lock);
    InitLocked();
    taskEXIT_CRITICAL(&g_lock);
}

void SetFaultBootPhase(BootPhase phase)
{
    if (phase > BootPhase::kStartupComplete) return;
    taskENTER_CRITICAL(&g_lock);
    InitLocked();
    if (g_current.view.boot_phase != phase) {
        g_current.view.boot_phase = phase;
        CommitLocked();
    }
    taskEXIT_CRITICAL(&g_lock);
}

void RecordFault(FaultDomain domain, FaultPhase phase, int32_t code,
                 uint32_t operation, uint32_t generation)
{
    if (domain > FaultDomain::kSync || phase > FaultPhase::kStorageQueueTimeout) return;
    FaultRecord record{};
    record.uptime_ms = esp_timer_get_time() / 1000;
    record.unix_sec = std::time(nullptr);
    const auto clock = GetWallClockSnapshot();
    record.clock_epoch = clock.epoch;
    record.clock_source = static_cast<uint8_t>(clock.source);
    record.rtc_continuity = clock.rtc_continuity ? 1 : 0;
    record.domain = domain;
    record.phase = phase;
    record.code = code;
    record.operation = operation;
    record.generation = generation;
    taskENTER_CRITICAL(&g_lock);
    InitLocked();
    auto& view = g_current.view;
    record.sequence = g_current.fault_sequence = NextId(g_current.fault_sequence);
    record.boot_id = view.boot_id;
    record.reset_reason = static_cast<uint8_t>(view.reset_reason);
    record.boot_phase = static_cast<uint8_t>(view.boot_phase);
    std::memcpy(record.build, view.build, sizeof(record.build));
    std::memcpy(&view.records[g_current.next], &record, sizeof(record));
    g_current.next = (g_current.next + 1) % kFaultRecordDepth;
    if (view.count < kFaultRecordDepth) ++view.count;
    else view.overwritten = Increment(view.overwritten);
    view.reports = Increment(view.reports);
    CommitLocked();
    taskEXIT_CRITICAL(&g_lock);
}

void RecordLegacyFault(const char* tag)
{
    if (tag == nullptr) return;
    FaultDomain domain = FaultDomain::kOther;
    if (!std::strcmp(tag, "audio") || !std::strcmp(tag, "audio_cap")) domain = FaultDomain::kAudio;
    else if (!std::strcmp(tag, "rtc")) domain = FaultDomain::kRtc;
    else if (!std::strcmp(tag, "provision") || !std::strcmp(tag, "wifi")) domain = FaultDomain::kNetwork;
    else if (!std::strcmp(tag, "ai")) domain = FaultDomain::kAi;
    else if (!std::strcmp(tag, "ui")) domain = FaultDomain::kUi;
    else if (!std::strcmp(tag, "persist")) domain = FaultDomain::kPersist;
    else if (!std::strcmp(tag, "sync")) domain = FaultDomain::kSync;
    RecordFault(domain, FaultPhase::kReportedError, kFaultCodeUnavailable);
}

bool CopyFaultDiagnostics(FaultDiagnosticsSnapshot* out)
{
    if (out == nullptr) return false;
    taskENTER_CRITICAL(&g_lock);
    InitLocked();
    std::memcpy(out, &g_current.view, sizeof(*out));
    const size_t first = (g_current.next + kFaultRecordDepth - out->count) % kFaultRecordDepth;
    for (size_t i = 0; i < out->count; ++i) {
        std::memcpy(&out->records[i], &g_current.view.records[(first + i) % kFaultRecordDepth], sizeof(FaultRecord));
    }
    taskEXIT_CRITICAL(&g_lock);
    return true;
}

void DumpFaultDiagnosticsToLog()
{
    FaultDiagnosticsSnapshot view{};
    CopyFaultDiagnostics(&view);
#if CONFIG_WQN_MEMORY_DIAGNOSTICS
    constexpr unsigned memory_diagnostics = 1;
#else
    constexpr unsigned memory_diagnostics = 0;
#endif
    ESP_LOGI(kTag, "FAULT:boot build=%s boot=%lu reset=%lu phase=%lu recovered=%lu previous_build=%s previous_boot=%lu previous_phase=%lu reports=%lu overwritten=%lu integrity_rejections=%lu count=%lu mem_diag=%u",
        view.build, static_cast<unsigned long>(view.boot_id), static_cast<unsigned long>(view.reset_reason),
        static_cast<unsigned long>(view.boot_phase), static_cast<unsigned long>(view.recovered), view.previous_build,
        static_cast<unsigned long>(view.previous_boot_id), static_cast<unsigned long>(view.previous_boot_phase),
        static_cast<unsigned long>(view.reports), static_cast<unsigned long>(view.overwritten),
        static_cast<unsigned long>(view.integrity_rejections), static_cast<unsigned long>(view.count), memory_diagnostics);
    for (size_t i = 0; i < view.count; ++i) {
        const auto& r = view.records[i];
        ESP_LOGI(kTag, "FAULT:record seq=%lu build=%s boot=%lu reset=%u boot_phase=%u domain=%u phase=%u code=%ld operation=%lu generation=%lu uptime_ms=%lld unix_sec=%lld clock_source=%u epoch=%lu continuity=%u",
            static_cast<unsigned long>(r.sequence), r.build, static_cast<unsigned long>(r.boot_id), r.reset_reason,
            r.boot_phase, static_cast<unsigned>(r.domain), static_cast<unsigned>(r.phase), static_cast<long>(r.code),
            static_cast<unsigned long>(r.operation), static_cast<unsigned long>(r.generation),
            static_cast<long long>(r.uptime_ms), static_cast<long long>(r.unix_sec), r.clock_source,
            static_cast<unsigned long>(r.clock_epoch), r.rtc_continuity);
    }
}
} // namespace wqn::runtime
