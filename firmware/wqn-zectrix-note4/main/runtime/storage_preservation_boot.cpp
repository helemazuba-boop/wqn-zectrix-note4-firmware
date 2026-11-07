#include "runtime/storage_preservation_boot.h"
#include "board_zectrix_note4.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn::runtime {
esp_err_t CheckStoragePreservationBoot(bool* requested)
{
    if (requested == nullptr) return ESP_ERR_INVALID_ARG;
    *requested = false;
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) return ESP_OK;
    const int first = ReadNote4BootPageUpLevel();
    if (first == 1) return ESP_OK;
    if (first != 0) return ESP_ERR_INVALID_RESPONSE;
    const int64_t started = esp_timer_get_time();
    if (started < 0) return ESP_ERR_INVALID_STATE;
    int64_t previous = started;
    const TickType_t period = pdMS_TO_TICKS(10) != 0 ? pdMS_TO_TICKS(10) : 1;
    // A broken timer cannot leave the boot task polling indefinitely. This is
    // an iteration bound, not a claim about worst-case scheduler latency.
    for (unsigned sample = 0; sample < 512; ++sample) {
        vTaskDelay(period);
        const int64_t now = esp_timer_get_time();
        if (now < previous) return ESP_ERR_INVALID_STATE;
        previous = now;
        const int level = ReadNote4BootPageUpLevel();
        if (level == 1) return ESP_OK;
        if (level != 0) return ESP_ERR_INVALID_RESPONSE;
        if (now-started >= 3000000) { *requested = true; return ESP_OK; }
    }
    return ESP_ERR_TIMEOUT;
}
}
