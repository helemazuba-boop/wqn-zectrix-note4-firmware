#pragma once

#include <cstdint>

#include "esp_err.h"

namespace wqn {

enum class ButtonId {
    kNone = 0,
    kUp,
    kDownPower,
    kConfirm,
};

enum class ButtonEventType {
    kNone = 0,
    // Edge events (FIFO from PollButtonInput). These fire on the debounced
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

    bool HasEvent() const { return type != ButtonEventType::kNone; }
};

esp_err_t InitButtonInput();
ButtonEvent PollButtonInput();

}  // namespace wqn
