#include "button_input.h"

#include <array>

#include "driver/gpio.h"
#include "esp_timer.h"

namespace {

constexpr gpio_num_t kUpPin = GPIO_NUM_39;
constexpr gpio_num_t kDownPowerPin = GPIO_NUM_18;
constexpr gpio_num_t kConfirmPin = GPIO_NUM_0;

constexpr int64_t kDebounceMs = 40;
constexpr int64_t kHoldPressMs = 200;  // [mistouch] Flash PTT start threshold
constexpr int64_t kLongPressMs = 1000;
constexpr int64_t kLongPressRepeatMs = 260;
struct ButtonState {
    wqn::ButtonId id;
    gpio_num_t pin;
    bool raw_pressed = false;
    bool stable_pressed = false;
    bool long_press_reported = false;
    bool hold_press_reported = false;  // [mistouch] one-shot kHoldPress at 200ms
    int64_t raw_changed_at_ms = 0;
    int64_t stable_changed_at_ms = 0;
    int64_t last_long_press_event_at_ms = 0;
    int64_t last_reported_at_ms = 0;
    // [ptt-fix] When a debounced press or release transition is first observed,
    // schedule an edge event (kPress / kRelease) for the *next* poll. The
    // Press event is delivered immediately (priority over the existing
    // long/short release logic, so PTT reacts with zero latency), and the
    // existing kShortPress/kLongRelease/kLongPress machinery
    // is left untouched for every other UI consumer.
    bool edge_event_pending = false;
    wqn::ButtonEventType pending_edge = wqn::ButtonEventType::kNone;
};

std::array<ButtonState, 3> g_buttons = {{
    {wqn::ButtonId::kUp, kUpPin},
    {wqn::ButtonId::kDownPower, kDownPowerPin},
    {wqn::ButtonId::kConfirm, kConfirmPin},
}};

int64_t NowMs()
{
    return esp_timer_get_time() / 1000;
}

bool ReadPressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

wqn::ButtonEvent MakeEvent(wqn::ButtonId id, wqn::ButtonEventType type, int64_t duration_ms)
{
    wqn::ButtonEvent event;
    event.button = id;
    event.type = type;
    event.duration_ms = duration_ms;
    return event;
}

}  // namespace

namespace wqn {

esp_err_t InitButtonInput()
{
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << kUpPin) | (1ULL << kDownPowerPin) | (1ULL << kConfirmPin);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    const int64_t now_ms = NowMs();
    for (ButtonState& button : g_buttons) {
        const bool pressed = ReadPressed(button.pin);
        button.raw_pressed = pressed;
        button.stable_pressed = pressed;
        button.long_press_reported = false;
        button.hold_press_reported = false;
        button.raw_changed_at_ms = now_ms;
        button.stable_changed_at_ms = now_ms;
        button.last_long_press_event_at_ms = now_ms;
    }

    return ESP_OK;
}

ButtonEvent PollButtonInput()
{
    const int64_t now_ms = NowMs();

    for (ButtonState& button : g_buttons) {
        // [ptt-fix] Drain any queued press/release edge event first. This runs
        // before the GPIO sampling so a freshly-armed edge is delivered on the
        // very next poll (one-button-input-period latency, ~50 ms) without
        // waiting for the long-press threshold.
        if (button.edge_event_pending) {
            button.edge_event_pending = false;
            const wqn::ButtonEventType et = button.pending_edge;
            button.pending_edge = wqn::ButtonEventType::kNone;
            return MakeEvent(button.id, et, 0);
        }

        const bool pressed = ReadPressed(button.pin);
        if (pressed != button.raw_pressed) {
            button.raw_pressed = pressed;
            button.raw_changed_at_ms = now_ms;
        }

        if (button.raw_pressed != button.stable_pressed &&
            now_ms - button.raw_changed_at_ms >= kDebounceMs) {
            const int64_t previous_stable_changed_at_ms = button.stable_changed_at_ms;
            button.stable_pressed = button.raw_pressed;
            button.stable_changed_at_ms = now_ms;

            if (button.stable_pressed) {
                button.long_press_reported = false;
                button.hold_press_reported = false;
                button.last_long_press_event_at_ms = now_ms;
                // [ptt-fix] Defer the kPress edge to the next poll so long-
                // press consumers still see kLongPress after the debounce
                // window passes, but PTT consumers see kPress within ~50 ms
                // instead of waiting for the 1-second long-press threshold.
                button.pending_edge = wqn::ButtonEventType::kPress;
                button.edge_event_pending = true;
            } else {
                const int64_t duration_ms = now_ms - previous_stable_changed_at_ms;
                if (!button.long_press_reported) {
                    // Double-confirm classification belongs to the UI shell.
                    // Emit every physical short release exactly once here.
                    button.pending_edge = wqn::ButtonEventType::kRelease;
                    button.edge_event_pending = true;
                    return MakeEvent(button.id, ButtonEventType::kShortPress, duration_ms);
                }
                // [ptt-fix] Long release path keeps the legacy event for
                // settings menu exit etc., but also queues kRelease so the
                // PTT capture stop happens on the press transition.
                button.pending_edge = wqn::ButtonEventType::kRelease;
                button.edge_event_pending = true;
                return MakeEvent(button.id, ButtonEventType::kLongRelease, duration_ms);
            }
        }

        // [mistouch] One-shot 200ms hold event before the legacy 1s long-press.
        // Flash PTT uses this; all other UI paths ignore kHoldPress.
        if (button.raw_pressed && button.stable_pressed && !button.hold_press_reported &&
            now_ms - button.stable_changed_at_ms >= kHoldPressMs) {
            button.hold_press_reported = true;
            return MakeEvent(button.id, ButtonEventType::kHoldPress,
                             now_ms - button.stable_changed_at_ms);
        }

        if (button.raw_pressed && button.stable_pressed && now_ms - button.stable_changed_at_ms >= kLongPressMs &&
            (!button.long_press_reported || now_ms - button.last_long_press_event_at_ms >= kLongPressRepeatMs)) {
            button.long_press_reported = true;
            button.last_long_press_event_at_ms = now_ms;
            return MakeEvent(
                button.id,
                ButtonEventType::kLongPress,
                now_ms - button.stable_changed_at_ms);
        }
    }

    return {};
}

}  // namespace wqn
