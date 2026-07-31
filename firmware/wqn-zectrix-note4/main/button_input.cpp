#include "button_input.h"

#include <array>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "power_manager.h"  // NoteUserActivityAtMs (sleep-race linearization)

namespace {

constexpr char kTag[] = "wqn_buttons";

constexpr gpio_num_t kUpPin = GPIO_NUM_39;
constexpr gpio_num_t kDownPowerPin = GPIO_NUM_18;
constexpr gpio_num_t kConfirmPin = GPIO_NUM_0;

constexpr int64_t kDebounceMs = 40;
constexpr int64_t kHoldPressMs = 200;  // [mistouch] Flash PTT start threshold
// [feel] Long presses fire while the button is still held (no release wait),
// so this threshold is the whole long-press latency. 1000 ms read as "is it
// broken?": HIL logs show users giving up mid-hold at ~930 ms and getting a
// stray short press instead. 650 ms catches those holds while staying above
// the slowest observed intentional short press (~630 ms).
constexpr int64_t kLongPressMs = 650;
constexpr int64_t kLongPressRepeatMs = 260;
constexpr int64_t kDoublePressWindowMs = 300;

// [input-capture] Active-window sampling period. Only runs while some key is
// inside a debounce/hold/double window; a fully idle keypad parks the task on
// a portMAX_DELAY notify wait (the low-level ISR is the only waker), so this
// period costs nothing in standby.
constexpr TickType_t kActiveSamplePeriod = pdMS_TO_TICKS(15);
constexpr uint32_t kButtonTaskStackBytes = 3072;
constexpr UBaseType_t kButtonTaskPriority = 6;

int64_t g_last_short_release_at_ms = 0;
wqn::ButtonId g_last_short_release_button = wqn::ButtonId::kNone;

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
    bool double_armed = false;
    // [ptt-fix] When a debounced press or release transition is first observed,
    // schedule an edge event (kPress / kRelease) for the *next* poll. The
    // Press event is delivered immediately (priority over the existing
    // long/short release logic, so PTT reacts with zero latency), and the
    // existing kShortPress/kLongRelease/kDoublePress / kLongPress machinery
    // is left untouched for every other UI consumer.
    bool edge_event_pending = false;
    wqn::ButtonEventType pending_edge = wqn::ButtonEventType::kNone;
    // [input-capture] The low-level ISR disables its own interrupt (a held key
    // would otherwise storm level interrupts); the task re-enables it once the
    // key is back to idle and every window has drained.
    bool isr_disabled = false;
};

std::array<ButtonState, 3> g_buttons = {{
    {wqn::ButtonId::kUp, kUpPin},
    {wqn::ButtonId::kDownPower, kDownPowerPin},
    {wqn::ButtonId::kConfirm, kConfirmPin},
}};

// ---- Event ring -------------------------------------------------------------
// Custom static ring instead of a FreeRTOS queue: the loss contract requires
// coalescing tail repeats and evicting repeats before critical events, which
// xQueueSend cannot express. All head/tail mutation happens inside the
// spinlock; producer is the button task, consumer is the UI task.
//
// Capacity contract: one short press produces up to 3 events (Press +
// ShortPress + Release). 64 slots hold 20 full short presses (60 events) plus
// margin while the UI is blocked for 12 s -- the design burst from the review.
constexpr size_t kEventRingCapacity = 64;
wqn::ButtonEvent g_event_ring[kEventRingCapacity];
size_t g_ring_head = 0;  // next slot to write
size_t g_ring_tail = 0;  // next slot to read
size_t g_ring_count = 0;
portMUX_TYPE g_ring_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_dropped_critical = 0;
uint32_t g_coalesced_repeat = 0;
uint32_t g_ring_high_water = 0;
uint32_t g_event_seq = 0;

TaskHandle_t g_button_task = nullptr;
TaskHandle_t g_ui_task_to_notify = nullptr;

bool IsRepeatEvent(const wqn::ButtonEvent& event)
{
    return event.type == wqn::ButtonEventType::kLongPress && event.repeat;
}

// Caller must hold g_ring_lock.
size_t RingIndexFromNewest(size_t offset_from_newest)
{
    return (g_ring_head + kEventRingCapacity - 1 - offset_from_newest) %
           kEventRingCapacity;
}

// Push with the documented loss policy. Runs on the button task only.
void PushButtonEvent(wqn::ButtonEvent event)
{
    taskENTER_CRITICAL(&g_ring_lock);
    event.seq = ++g_event_seq;
    // Tail coalescing: a repeat directly following a repeat of the same key
    // replaces it (menus must not replay a burst of held-key steps after the
    // UI unblocks).
    if (IsRepeatEvent(event) && g_ring_count > 0) {
        wqn::ButtonEvent& newest = g_event_ring[RingIndexFromNewest(0)];
        if (IsRepeatEvent(newest) && newest.button == event.button) {
            newest = event;
            ++g_coalesced_repeat;
            taskEXIT_CRITICAL(&g_ring_lock);
            return;
        }
    }
    if (g_ring_count == kEventRingCapacity) {
        if (IsRepeatEvent(event)) {
            // Full ring never spends a slot on a new repeat.
            ++g_coalesced_repeat;
            taskEXIT_CRITICAL(&g_ring_lock);
            return;
        }
        // Evict the oldest repeat to make room for a critical event.
        bool evicted = false;
        for (size_t i = 0; i < g_ring_count; ++i) {
            const size_t idx = (g_ring_tail + i) % kEventRingCapacity;
            if (IsRepeatEvent(g_event_ring[idx])) {
                for (size_t j = i; j + 1 < g_ring_count; ++j) {
                    g_event_ring[(g_ring_tail + j) % kEventRingCapacity] =
                        g_event_ring[(g_ring_tail + j + 1) % kEventRingCapacity];
                }
                g_ring_head =
                    (g_ring_head + kEventRingCapacity - 1) % kEventRingCapacity;
                --g_ring_count;
                ++g_coalesced_repeat;
                evicted = true;
                break;
            }
        }
        if (!evicted) {
            // Best effort exhausted: count it loudly instead of blocking the
            // sampler (a blocked producer stops capturing altogether).
            ++g_dropped_critical;
            taskEXIT_CRITICAL(&g_ring_lock);
            return;
        }
    }
    g_event_ring[g_ring_head] = event;
    g_ring_head = (g_ring_head + 1) % kEventRingCapacity;
    ++g_ring_count;
    if (g_ring_count > g_ring_high_water) {
        g_ring_high_water = static_cast<uint32_t>(g_ring_count);
    }
    // [sleep-race] Publish user activity the instant a critical event becomes
    // visible in the ring, still holding g_ring_lock, and BEFORE the UI
    // dequeues it. This is the linearization point the deep-sleep commit gate
    // validates against: the power task cannot both pass its final generation
    // check and sleep while a dequeued-but-unreduced release/short-press is
    // pending (that GPIO is no longer held low, so EXT1 can't re-wake it).
    // Repeats are not interactions worth blocking sleep for (they only occur
    // while a key is physically held, which itself keeps the wake source
    // asserted), and they get coalesced/dropped anyway.
    if (!IsRepeatEvent(event)) {
        wqn::NoteUserActivityAtMs(event.occurred_at_ms);
    }
    taskEXIT_CRITICAL(&g_ring_lock);
}

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
    event.occurred_at_ms = NowMs();
    return event;
}

// ---- Classification state machine (unchanged semantics) ---------------------
// One pass over the three keys; returns at most one event. Identical logic to
// the old UI-task PollButtonInput, now hosted by the dedicated button task.
wqn::ButtonEvent ClassifyButtonsOnce()
{
    const int64_t now_ms = NowMs();

    for (ButtonState& button : g_buttons) {
        // [ptt-fix] Drain any queued press/release edge event first, so a
        // freshly-armed edge is delivered on the very next pass.
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
                button.pending_edge = wqn::ButtonEventType::kPress;
                button.edge_event_pending = true;
            } else {
                const int64_t duration_ms = now_ms - previous_stable_changed_at_ms;
                if (!button.long_press_reported) {
                    if (g_last_short_release_button == button.id &&
                        now_ms - g_last_short_release_at_ms <= kDoublePressWindowMs) {
                        g_last_short_release_button = wqn::ButtonId::kNone;
                        g_last_short_release_at_ms = 0;
                        button.pending_edge = wqn::ButtonEventType::kRelease;
                        button.edge_event_pending = true;
                        return MakeEvent(button.id, wqn::ButtonEventType::kDoublePress, duration_ms);
                    }
                    g_last_short_release_button = button.id;
                    g_last_short_release_at_ms = now_ms;
                    button.pending_edge = wqn::ButtonEventType::kRelease;
                    button.edge_event_pending = true;
                    return MakeEvent(button.id, wqn::ButtonEventType::kShortPress, duration_ms);
                }
                button.pending_edge = wqn::ButtonEventType::kRelease;
                button.edge_event_pending = true;
                return MakeEvent(button.id, wqn::ButtonEventType::kLongRelease, duration_ms);
            }
        }

        // [mistouch] One-shot 200ms hold event before the long-press.
        if (button.raw_pressed && button.stable_pressed && !button.hold_press_reported &&
            now_ms - button.stable_changed_at_ms >= kHoldPressMs) {
            button.hold_press_reported = true;
            return MakeEvent(button.id, wqn::ButtonEventType::kHoldPress,
                             now_ms - button.stable_changed_at_ms);
        }

        if (button.raw_pressed && button.stable_pressed && now_ms - button.stable_changed_at_ms >= kLongPressMs &&
            (!button.long_press_reported || now_ms - button.last_long_press_event_at_ms >= kLongPressRepeatMs)) {
            // [longpress-fix] Mark auto-repeats explicitly; duration-based
            // inference at the consumer broke when this threshold changed.
            const bool is_repeat = button.long_press_reported;
            button.long_press_reported = true;
            button.last_long_press_event_at_ms = now_ms;
            wqn::ButtonEvent event = MakeEvent(
                button.id,
                wqn::ButtonEventType::kLongPress,
                now_ms - button.stable_changed_at_ms);
            event.repeat = is_repeat;
            return event;
        }
    }

    return {};
}

// True while any key still needs periodic sampling: raw/stable pressed,
// deferred edge undelivered, debounce settling, or the double-press window of
// the last short release is still open.
bool AnyButtonWindowActive()
{
    const int64_t now_ms = NowMs();
    if (g_last_short_release_button != wqn::ButtonId::kNone &&
        now_ms - g_last_short_release_at_ms <= kDoublePressWindowMs) {
        return true;
    }
    for (const ButtonState& button : g_buttons) {
        if (button.raw_pressed || button.stable_pressed ||
            button.edge_event_pending ||
            button.raw_pressed != button.stable_pressed) {
            return true;
        }
    }
    return false;
}

// ---- ISR + task -------------------------------------------------------------

// Low-level ISR: a held key would storm level interrupts, so the handler
// disables its own line and wakes the sampler; the task re-enables the line
// once the key returns to idle. Low-level (not edge) triggering doubles as
// the light-sleep wake configuration, so standby key presses wake the chip.
void IRAM_ATTR ButtonIsrHandler(void* arg)
{
    gpio_num_t pin = static_cast<gpio_num_t>(reinterpret_cast<intptr_t>(arg));
    gpio_intr_disable(pin);
    for (ButtonState& button : g_buttons) {
        if (button.pin == pin) {
            button.isr_disabled = true;
            break;
        }
    }
    BaseType_t higher_priority_woken = pdFALSE;
    if (g_button_task != nullptr) {
        vTaskNotifyGiveFromISR(g_button_task, &higher_priority_woken);
    }
    portYIELD_FROM_ISR(higher_priority_woken);
}

void ReenableIdleButtonInterrupts()
{
    for (ButtonState& button : g_buttons) {
        if (button.isr_disabled && !button.raw_pressed && !button.stable_pressed) {
            button.isr_disabled = false;
            gpio_intr_enable(button.pin);
        }
    }
}

void ButtonInputTask(void*)
{
    ESP_LOGI(kTag, "button task started: ring=%u sample_ms=%u",
             static_cast<unsigned>(kEventRingCapacity),
             static_cast<unsigned>(pdTICKS_TO_MS(kActiveSamplePeriod)));
    while (true) {
        if (!AnyButtonWindowActive()) {
            ReenableIdleButtonInterrupts();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        const wqn::ButtonEvent event = ClassifyButtonsOnce();
        if (event.HasEvent()) {
            PushButtonEvent(event);
            if (g_ui_task_to_notify != nullptr) {
                xTaskNotifyGive(g_ui_task_to_notify);
            }
        }
        vTaskDelay(kActiveSamplePeriod);
    }
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
    config.intr_type = GPIO_INTR_LOW_LEVEL;

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

esp_err_t StartButtonInputTask(TaskHandle_t ui_task_to_notify)
{
    if (g_button_task != nullptr) {
        g_ui_task_to_notify = ui_task_to_notify;
        return ESP_OK;
    }
    g_ui_task_to_notify = ui_task_to_notify;

    // Shared ISR service may already be installed by another component.
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    for (ButtonState& button : g_buttons) {
        err = gpio_isr_handler_add(
            button.pin, ButtonIsrHandler,
            reinterpret_cast<void*>(static_cast<intptr_t>(button.pin)));
        if (err != ESP_OK) {
            return err;
        }
        // Light-sleep wake: low-level trigger matches the runtime ISR type,
        // so a standby key press wakes the chip out of automatic light sleep
        // (without this, zero-polling capture would be a regression: the
        // device would sleep through presses entirely).
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            gpio_wakeup_enable(button.pin, GPIO_INTR_LOW_LEVEL));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_gpio_wakeup());

    const BaseType_t created = xTaskCreate(
        ButtonInputTask, "wqn_buttons", kButtonTaskStackBytes, nullptr,
        kButtonTaskPriority, &g_button_task);
    if (created != pdPASS) {
        g_button_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ReceiveButtonEvent(ButtonEvent* out)
{
    if (out == nullptr) {
        return false;
    }
    bool has_event = false;
    taskENTER_CRITICAL(&g_ring_lock);
    if (g_ring_count > 0) {
        *out = g_event_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) % kEventRingCapacity;
        --g_ring_count;
        has_event = true;
    }
    taskEXIT_CRITICAL(&g_ring_lock);
    return has_event;
}

uint32_t ButtonEventsDroppedCritical()
{
    return g_dropped_critical;
}

uint32_t ButtonEventsCoalescedRepeat()
{
    return g_coalesced_repeat;
}

uint32_t ButtonEventRingHighWater()
{
    return g_ring_high_water;
}

}  // namespace wqn
