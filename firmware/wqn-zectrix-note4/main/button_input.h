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
