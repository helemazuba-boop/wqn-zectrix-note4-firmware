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
    kShortPress,
    kLongPress,
    kLongRelease,
    kDoublePress,
};

struct ButtonEvent {
    ButtonId button = ButtonId::kNone;
    ButtonEventType type = ButtonEventType::kNone;
    int64_t duration_ms = 0;

    bool HasEvent() const { return type != ButtonEventType::kNone; }
};

esp_err_t InitButtonInput();
ButtonEvent PollButtonInput();

}  // namespace wqn
