#pragma once

#include <cstdint>

#include "button_event.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn {

esp_err_t InitButtonInput();

// [input-capture] Starts the dedicated sampling task. Buttons are captured by
// a GPIO low-level ISR (disabled-in-ISR, re-enabled once the key returns to
// idle) that wakes the task; the task samples the debounce/long-press/double
// windows at a fixed short period and sleeps forever when every key is idle,
// so standby adds zero polling wakeups. Every classified event is pushed into
// a static ring and `ui_task_to_notify` gets an xTaskNotifyGive.
// Loss contract: Press/Release and semantic events survive at least 20 full
// short presses (3 events each) while the UI is blocked; only consecutive
// kLongPress repeats are coalesced/dropped under pressure.
esp_err_t StartButtonInputTask(TaskHandle_t ui_task_to_notify);

// Non-blocking consume from the event ring (UI task side). Returns false when
// the ring is empty.
bool ReceiveButtonEvent(ButtonEvent* out);

// Diagnostics: critical events dropped (ring exhausted with nothing
// coalescable), repeats coalesced, and the ring occupancy high-water mark.
uint32_t ButtonEventsDroppedCritical();
uint32_t ButtonEventsCoalescedRepeat();
uint32_t ButtonEventRingHighWater();

}  // namespace wqn
