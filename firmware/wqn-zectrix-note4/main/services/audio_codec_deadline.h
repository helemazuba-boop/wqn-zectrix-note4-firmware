#pragma once

#include <algorithm>
#include <cstdint>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn::services::detail {

// One absolute budget across a codec call. Driver/reset/cleanup internals can
// overrun; this controls admission and rejects late success, not hard WCET.
class CodecDeadline {
public:
    explicit CodecDeadline(int64_t deadline_us) : deadline_us_(deadline_us) {}

    bool expired() const { return esp_timer_get_time() >= deadline_us_; }

    TickType_t lock_ticks() const
    {
        const int64_t ms = std::clamp<int64_t>(
            (deadline_us_ - esp_timer_get_time()) / 1000, 0, 100);
        return pdMS_TO_TICKS(ms);
    }

    int transfer_ms() const
    {
        return static_cast<int>(std::clamp<int64_t>(
            (deadline_us_ - esp_timer_get_time()) / 1000, 0, 100));
    }

    esp_err_t result(esp_err_t value) const
    {
        return expired() ? ESP_ERR_TIMEOUT : value;
    }

    bool delay(TickType_t ticks) const
    {
        const int64_t max_wait_us =
            (int64_t{ticks} * 1000000 + configTICK_RATE_HZ - 1) / configTICK_RATE_HZ;
        if (deadline_us_ - esp_timer_get_time() <= max_wait_us) return false;
        vTaskDelay(ticks);
        return !expired();
    }

private:
    int64_t deadline_us_;
};

}  // namespace wqn::services::detail
