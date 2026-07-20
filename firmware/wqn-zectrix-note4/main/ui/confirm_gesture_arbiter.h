#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "button_input.h"

namespace wqn::ui {

inline constexpr int64_t kConfirmDoublePressWindowMs = 300;

// A bounded result is used because an unrelated physical event may first
// settle a pending single-confirm and then be delivered itself.
struct ConfirmGestureBatch {
    std::array<ButtonEvent, 2> events = {};
    size_t count = 0;

    void Push(const ButtonEvent& event)
    {
        if (event.HasEvent() && count < events.size()) {
            events[count++] = event;
        }
    }
};

// Converts the derived short-confirm stream into exactly one single or double
// semantic event. Raw Press/Hold/Release events are never delayed, which keeps
// Flash PTT independent of click classification.
class ConfirmGestureArbiter final {
public:
    ConfirmGestureBatch Process(
        const ButtonEvent& physical_event,
        int64_t event_time_ms,
        uint32_t context);
    ConfirmGestureBatch Poll(int64_t now_ms, uint32_t context);
    void Reset();

    bool has_pending_single() const { return pending_; }

private:
    bool FlushPendingIfDue(
        int64_t now_ms,
        uint32_t context,
        ConfirmGestureBatch* batch);
    void EmitPending(ConfirmGestureBatch* batch);

    bool pending_ = false;
    ButtonEvent pending_event_ = {};
    int64_t pending_since_ms_ = 0;
    uint32_t pending_context_ = 0;
};

bool RunConfirmGestureArbiterSelfTest();

}  // namespace wqn::ui
