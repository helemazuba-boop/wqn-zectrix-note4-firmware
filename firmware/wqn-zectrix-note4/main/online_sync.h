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

// [power-fix] Returns true when the device has a usable access token
// (i.e. pairing finished). Power manager uses this to refuse deep sleep
// while the device is still in provisioning mode, which would otherwise
// tear down the SoftAP / captive portal mid-flow.
bool HasUsableStoredToken();

#if CONFIG_WQN_WIFI_STA_ENABLE
extern TaskHandle_t g_wqn_online_task;
esp_err_t StartWqnOnlineTask();
#endif

}  // namespace wqn
