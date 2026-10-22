#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn::runtime {

// A task can call vTaskDelay immediately before a tick boundary. Round up the
// requested duration and add one tick so even that phase leaves the requested
// physical interval. Poll loops that only need to block should use one tick.
template <uint32_t milliseconds>
constexpr TickType_t MinimumDelayTicks()
{
    constexpr uint64_t ticks =
        (uint64_t{milliseconds} * configTICK_RATE_HZ + 999) / 1000 + 1;
    static_assert(milliseconds > 0 && ticks < portMAX_DELAY);
    return static_cast<TickType_t>(ticks);
}

template <uint32_t milliseconds>
void DelayAtLeastMs()
{
    vTaskDelay(MinimumDelayTicks<milliseconds>());
}

}  // namespace wqn::runtime
