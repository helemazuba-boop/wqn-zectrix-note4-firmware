#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn {

enum class ButtonId {
    kNone = 0,
    kUp,
    kDownPower,
    kConfirm,
};

enum class ButtonEventType {
    kNone = 0,
    // Edge events (FIFO from the button task). These fire on the debounced
    // press / release transitions and are the canonical signal for PTT-style
    // "press-to-talk" flows where every millisecond of leading-edge audio
    // matters. Older derived events (kShortPress / kLongPress / kLongRelease
    // / kDoublePress) are still emitted on the release transition for
    // non-PTT flows that want delayed-press semantics.
    kPress,
    kRelease,
    // [mistouch] One-shot hold threshold (200 ms). Flash PTT starts here,
    // not on raw kPress, so short/double taps never enter capture/"识别".
    kHoldPress,
    kShortPress,
    kLongPress,
    kLongRelease,
    kDoublePress,
};

struct ButtonEvent {
    ButtonId button = ButtonId::kNone;
    ButtonEventType type = ButtonEventType::kNone;
    int64_t duration_ms = 0;
    // [longpress-fix] True for the 2nd+ kLongPress while the button is still
    // held (the driver auto-repeats every kLongPressRepeatMs). Consumers that
    // want one action per physical hold must gate on this flag -- inferring
    // it from duration_ms broke silently when kLongPressMs changed (650+260
    // = 910 ms slipped under the old 1150 ms duration gate and every hold
    // past ~910 ms fired the long-press action twice).
    bool repeat = false;
    // [input-capture] Production timestamp (ms since boot) and monotonic
    // sequence, stamped by the button task at classification time. Consumers
    // MUST use occurred_at_ms for anything time-derived (idle timers, stale
    // gesture rejection): a UI task wedged for seconds later replays the ring
    // and consumption time would corrupt every window computation.
    int64_t occurred_at_ms = 0;
    uint32_t seq = 0;

    bool HasEvent() const { return type != ButtonEventType::kNone; }
};

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
