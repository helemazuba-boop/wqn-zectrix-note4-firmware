#pragma once

#include <cstdint>

namespace wqn {

enum class UiScreen : uint8_t {
    // Values are persisted in RTC slow memory across deep sleep. Keep retained
    // IDs stable so firmware updates can reject removed prototype screens 7-12
    // instead of reinterpreting them as another page.
    kAi = 0,
    kTodo = 1,
    kSettings = 2,
    kHome = 3,
    kTime = 4,
    kWord = 5,
    kNote = 6,
    kProvisioning = 13,
    // [agent] Reserved. The standalone OpenCode Agent page was demoted into
    // the AI page as AiTier::kAgent, so this screen is no longer reachable
    // from the page ring. The ID stays occupied for the same reason 7-12 do:
    // a device that wakes from deep sleep with 14 in RTC memory must reject it
    // (see IsRestorableScreen in ui_state.cpp) instead of rendering a page
    // that no longer exists.
    kOpenCode = 14,
};

}  // namespace wqn
