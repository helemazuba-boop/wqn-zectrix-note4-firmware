#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"

namespace wqn {

struct OnlineSyncSnapshot {
    bool task_running = false;
    bool last_round_success = false;
    uint32_t interval_minutes = 0;
    uint32_t success_count = 0;
    uint32_t failure_count = 0;
    int64_t last_started_ms = 0;
    int64_t last_finished_ms = 0;
    char status[64] = {};
};

void NotifyOnlineSyncRequested();
void RequestOnlineSyncNow();
void GetOnlineSyncSnapshot(OnlineSyncSnapshot* snapshot);
TickType_t GetConfiguredOnlineSyncDelayTicks();

}  // namespace wqn
