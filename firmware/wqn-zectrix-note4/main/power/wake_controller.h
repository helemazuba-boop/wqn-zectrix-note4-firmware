#pragma once

#include <cstdint>

#include "esp_err.h"

namespace wqn::power {

enum class TimerWakeSource : uint8_t {
    kDisabled = 0,
    kPcf8563,
    kEsp32,
};

struct WakeArmResult {
    esp_err_t error = ESP_FAIL;
    uint64_t wake_gpio_mask = 0;
    uint64_t active_gpio_mask = 0;
    TimerWakeSource timer_source = TimerWakeSource::kDisabled;
};

void SetPcf8563WakeAvailable(bool available);
void CaptureWakeContext();
WakeArmResult ArmWakeSources(uint32_t timer_wakeup_seconds, int64_t deadline_us);
void DisarmWakeSources();
const char* TimerWakeSourceName(TimerWakeSource source);

}  // namespace wqn::power
