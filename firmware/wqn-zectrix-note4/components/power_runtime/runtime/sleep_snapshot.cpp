#include "runtime/sleep_snapshot.h"

#include <cstddef>
#include <cstring>

#include "esp_attr.h"
#include "esp_rom_crc.h"

namespace {

constexpr uint32_t kSleepSnapshotMagic = 0x57514E53;  // WQNS
constexpr uint16_t kSleepSnapshotVersion = 2;
constexpr uint8_t kTimerWakeDisabled = 0;
constexpr uint8_t kTimerWakeBackground = 1;
constexpr uint8_t kTimerWakeDisplay = 2;

struct RtcSleepSnapshotRecord {
    uint32_t magic;
    uint16_t version;
    uint8_t mode;
    uint8_t timer_wakeup_kind;
    uint32_t generation;
    uint32_t consecutive_cycles;
    uint64_t wake_gpio_mask;
    uint32_t crc32;
};

RTC_DATA_ATTR RtcSleepSnapshotRecord g_sleep_snapshot_record = {};

uint32_t SnapshotCrc(const RtcSleepSnapshotRecord& record)
{
    return esp_rom_crc32_le(
        0,
        reinterpret_cast<const uint8_t*>(&record),
        offsetof(RtcSleepSnapshotRecord, crc32));
}

bool IsKnownMode(uint8_t mode)
{
    return mode == static_cast<uint8_t>(wqn::power::SleepMode::kIdle) ||
        mode == static_cast<uint8_t>(wqn::power::SleepMode::kBatteryEmergency);
}

}  // namespace

namespace wqn::runtime {

void CommitSleepSnapshot(const SleepSnapshot& snapshot)
{
    RtcSleepSnapshotRecord record = {};
    record.magic = kSleepSnapshotMagic;
    record.version = kSleepSnapshotVersion;
    record.mode = static_cast<uint8_t>(snapshot.mode);
    record.timer_wakeup_kind = !snapshot.timer_wakeup_enabled
        ? kTimerWakeDisabled
        : snapshot.timer_wakeup_for_display
            ? kTimerWakeDisplay
            : kTimerWakeBackground;
    record.generation = snapshot.generation;
    record.consecutive_cycles = snapshot.consecutive_cycles;
    record.wake_gpio_mask = snapshot.wake_gpio_mask;
    record.crc32 = SnapshotCrc(record);
    g_sleep_snapshot_record = record;
}

bool LoadSleepSnapshot(SleepSnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return false;
    }
    *snapshot = {};

    const RtcSleepSnapshotRecord record = g_sleep_snapshot_record;
    if (record.magic != kSleepSnapshotMagic ||
        record.version != kSleepSnapshotVersion ||
        !IsKnownMode(record.mode) ||
        record.timer_wakeup_kind > kTimerWakeDisplay ||
        record.crc32 != SnapshotCrc(record)) {
        return false;
    }

    snapshot->generation = record.generation;
    snapshot->mode = static_cast<power::SleepMode>(record.mode);
    snapshot->timer_wakeup_enabled = record.timer_wakeup_kind != kTimerWakeDisabled;
    snapshot->timer_wakeup_for_display =
        record.timer_wakeup_kind == kTimerWakeDisplay;
    snapshot->consecutive_cycles = record.consecutive_cycles;
    snapshot->wake_gpio_mask = record.wake_gpio_mask;
    return true;
}

void InvalidateSleepSnapshot()
{
    std::memset(&g_sleep_snapshot_record, 0, sizeof(g_sleep_snapshot_record));
}

}  // namespace wqn::runtime
