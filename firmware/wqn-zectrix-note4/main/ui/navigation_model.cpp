#include "navigation_model.h"

namespace wqn {

NavigationAction ReduceNavigationInput(
    NavigationState* state, UiScreen screen, const ButtonEvent& event,
    int64_t event_time_ms, bool can_open)
{
    if (state == nullptr || !event.HasEvent() || event.button == ButtonId::kNone) {
        return NavigationAction::kPass;
    }
    const uint8_t bit = 1U << static_cast<unsigned>(event.button);
    if (event.type == ButtonEventType::kPress) {
        state->held_mask |= bit;
        // A second Confirm tap must not activate the destination page or
        // enter AI status editing after the first tap accepted navigation.
        if (!state->visible && event.button == ButtonId::kConfirm &&
            state->guard_confirm_double) {
            const int64_t elapsed = event_time_ms - state->confirmed_at_ms;
            if (elapsed >= 0 && elapsed <= kButtonDoublePressWindowMs) {
                state->blocked_mask |= bit;
            }
            state->guard_confirm_double = false;
        }
    }
    if (state->blocked_mask & bit) {
        if (event.type == ButtonEventType::kRelease) {
            state->blocked_mask &= ~bit;
            state->held_mask &= ~bit;
        }
        return NavigationAction::kConsumed;
    }
    if (event.type == ButtonEventType::kRelease) state->held_mask &= ~bit;

    const bool page_key = event.button == ButtonId::kUp ||
                          event.button == ButtonId::kDownPower;
    const bool long_press = event.type == ButtonEventType::kLongPress && !event.repeat;
    if (!state->visible) {
        const size_t current = NavigationPageIndex(screen);
        if (!can_open || !page_key || !long_press || current == kNavigationPages.size()) {
            return NavigationAction::kPass;
        }
        state->visible = true;
        state->selected = static_cast<uint8_t>(current);
        state->blocked_mask |= state->held_mask | bit;
        state->guard_confirm_double = false;
        return NavigationAction::kRepaint;
    }

    if (page_key && long_press) {
        state->visible = false;
        state->blocked_mask |= state->held_mask | bit;
        return NavigationAction::kRepaint;
    }
    const bool click = event.type == ButtonEventType::kShortPress ||
                       event.type == ButtonEventType::kDoublePress;
    if (click && page_key) {
        // The first tap was already a ShortPress; DoublePress represents
        // only the second tap, so rapid navigation still advances once/tap.
        const int direction = event.button == ButtonId::kUp ? -1 : 1;
        state->selected = static_cast<uint8_t>(
            (state->selected + kNavigationPages.size() + direction) % kNavigationPages.size());
        return NavigationAction::kRepaint;
    }
    if (click && event.button == ButtonId::kConfirm) {
        state->visible = false;
        state->blocked_mask |= state->held_mask | bit;
        state->guard_confirm_double = true;
        state->confirmed_at_ms = event_time_ms;
        return NavigationAction::kConfirm;
    }
    return NavigationAction::kConsumed;
}

}  // namespace wqn
