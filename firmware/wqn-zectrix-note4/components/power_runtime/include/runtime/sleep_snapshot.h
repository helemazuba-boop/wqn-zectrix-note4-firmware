#pragma once

#include <cstdint>

#include "power/sleep_protocol.h"

namespace wqn::runtime {

struct SleepSnapshot {
    uint32_t generation = 0;
    power::SleepMode mode = power::SleepMode::kIdle;
    bool timer_wakeup_enabled = false;
    uint32_t consecutive_cycles = 0;
    uint64_t wake_gpio_mask = 0;
};

// RTC slow-memory snapshot. A record is visible only when its magic, version
// and CRC all match; a torn write is therefore treated as an untrusted boot.
void CommitSleepSnapshot(const SleepSnapshot& snapshot);
bool LoadSleepSnapshot(SleepSnapshot* snapshot);
void InvalidateSleepSnapshot();

}  // namespace wqn::runtime
