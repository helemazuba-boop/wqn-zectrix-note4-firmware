#include "button_input.h"

#include <array>

#include "driver/gpio.h"
#include "esp_timer.h"

namespace {

constexpr gpio_num_t kUpPin = GPIO_NUM_39;
constexpr gpio_num_t kDownPowerPin = GPIO_NUM_18;
constexpr gpio_num_t kConfirmPin = GPIO_NUM_0;

constexpr int64_t kDebounceMs = 40;
constexpr int64_t kLongPressMs = 1000;
constexpr int64_t kLongPressRepeatMs = 260;
constexpr int64_t kDoublePressWindowMs = 300;

int64_t g_last_short_release_at_ms = 0;
wqn::ButtonId g_last_short_release_button = wqn::ButtonId::kNone;

struct ButtonState {
    wqn::ButtonId id;
    gpio_num_t pin;
    bool raw_pressed = false;
    bool stable_pressed = false;
    bool long_press_reported = false;
    int64_t raw_changed_at_ms = 0;
    int64_t stable_changed_at_ms = 0;
    int64_t last_long_press_event_at_ms = 0;
    int64_t last_reported_at_ms = 0;
    bool double_armed = false;
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
                button.last_long_press_event_at_ms = now_ms;
            } else {
                const int64_t duration_ms = now_ms - previous_stable_changed_at_ms;
                if (!button.long_press_reported) {
                    if (g_last_short_release_button == button.id &&
                        now_ms - g_last_short_release_at_ms <= kDoublePressWindowMs) {
                        g_last_short_release_button = wqn::ButtonId::kNone;
                        g_last_short_release_at_ms = 0;
                        return MakeEvent(button.id, ButtonEventType::kDoublePress, duration_ms);
                    }
                    g_last_short_release_button = button.id;
                    g_last_short_release_at_ms = now_ms;
                    return MakeEvent(button.id, ButtonEventType::kShortPress, duration_ms);
                }
                return MakeEvent(button.id, ButtonEventType::kLongRelease, duration_ms);
            }
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
