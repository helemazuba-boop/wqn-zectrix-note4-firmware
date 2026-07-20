#include "ui/confirm_gesture_arbiter.h"

namespace {

wqn::ButtonEvent MakeEvent(
    wqn::ButtonId button,
    wqn::ButtonEventType type,
    int64_t duration_ms = 0)
{
    wqn::ButtonEvent event;
    event.button = button;
    event.type = type;
    event.duration_ms = duration_ms;
    return event;
}

bool IsEvent(
    const wqn::ui::ConfirmGestureBatch& batch,
    size_t index,
    wqn::ButtonId button,
    wqn::ButtonEventType type)
{
    return index < batch.count && batch.events[index].button == button &&
           batch.events[index].type == type;
}

}  // namespace

namespace wqn::ui {

void ConfirmGestureArbiter::Reset()
{
    pending_ = false;
    pending_event_ = {};
    pending_since_ms_ = 0;
    pending_context_ = 0;
}

void ConfirmGestureArbiter::EmitPending(ConfirmGestureBatch* batch)
{
    if (batch != nullptr && pending_) {
        batch->Push(pending_event_);
    }
    Reset();
}

bool ConfirmGestureArbiter::FlushPendingIfDue(
    int64_t now_ms,
    uint32_t context,
    ConfirmGestureBatch* batch)
{
    if (!pending_) {
        return false;
    }
    if (context != pending_context_) {
        // The original target no longer exists. Never replay its confirm into
        // a new page or a different business mode.
        Reset();
        return false;
    }
    if (now_ms - pending_since_ms_ < kConfirmDoublePressWindowMs) {
        return false;
    }
    EmitPending(batch);
    return true;
}

ConfirmGestureBatch ConfirmGestureArbiter::Process(
    const ButtonEvent& physical_event,
    int64_t event_time_ms,
    uint32_t context)
{
    ConfirmGestureBatch batch;
    if (!physical_event.HasEvent()) {
        return Poll(event_time_ms, context);
    }

    FlushPendingIfDue(event_time_ms, context, &batch);

    const bool short_confirm =
        physical_event.button == ButtonId::kConfirm &&
        physical_event.type == ButtonEventType::kShortPress;
    if (short_confirm) {
        if (pending_) {
            if (context == pending_context_ &&
                event_time_ms - pending_since_ms_ <=
                    kConfirmDoublePressWindowMs) {
                const int64_t duration_ms = physical_event.duration_ms;
                Reset();
                batch.Push(MakeEvent(
                    ButtonId::kConfirm,
                    ButtonEventType::kDoublePress,
                    duration_ms));
                return batch;
            }
            // Context changes are normally handled above. Keep this branch
            // defensive if a future caller uses non-monotonic timestamps.
            Reset();
        }
        pending_ = true;
        pending_event_ = physical_event;
        pending_since_ms_ = event_time_ms;
        pending_context_ = context;
        return batch;
    }

    // An event from another physical button happened after the first confirm,
    // so settle the single first and then preserve the physical event order.
    if (pending_ && physical_event.button != ButtonId::kConfirm) {
        if (context == pending_context_) {
            EmitPending(&batch);
        } else {
            Reset();
        }
    }

    batch.Push(physical_event);
    return batch;
}

ConfirmGestureBatch ConfirmGestureArbiter::Poll(
    int64_t now_ms,
    uint32_t context)
{
    ConfirmGestureBatch batch;
    FlushPendingIfDue(now_ms, context, &batch);
    return batch;
}

bool RunConfirmGestureArbiterSelfTest()
{
    constexpr uint32_t kFront = 0x0502;
    constexpr uint32_t kBack = 0x0503;
    ConfirmGestureArbiter arbiter;

    ConfirmGestureBatch batch = arbiter.Process(
        MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
        1000,
        kFront);
    if (batch.count != 0 ||
        arbiter.Poll(1299, kFront).count != 0) {
        return false;
    }
    batch = arbiter.Poll(1300, kFront);
    if (batch.count != 1 ||
        !IsEvent(batch, 0, ButtonId::kConfirm, ButtonEventType::kShortPress)) {
        return false;
    }

    arbiter.Process(
        MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
        2000,
        kFront);
    batch = arbiter.Process(
        MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
        2240,
        kFront);
    if (batch.count != 1 ||
        !IsEvent(batch, 0, ButtonId::kConfirm, ButtonEventType::kDoublePress) ||
        arbiter.has_pending_single()) {
        return false;
    }

    arbiter.Process(
        MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
        3000,
        kFront);
    batch = arbiter.Process(
        MakeEvent(ButtonId::kUp, ButtonEventType::kShortPress),
        3050,
        kFront);
    if (batch.count != 2 ||
        !IsEvent(batch, 0, ButtonId::kConfirm, ButtonEventType::kShortPress) ||
        !IsEvent(batch, 1, ButtonId::kUp, ButtonEventType::kShortPress)) {
        return false;
    }

    arbiter.Process(
        MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
        4000,
        kFront);
    if (arbiter.Poll(4300, kBack).count != 0 ||
        arbiter.has_pending_single()) {
        return false;
    }

    const ButtonEventType raw_types[] = {
        ButtonEventType::kPress,
        ButtonEventType::kHoldPress,
        ButtonEventType::kRelease,
    };
    for (size_t i = 0; i < 3; ++i) {
        batch = arbiter.Process(
            MakeEvent(ButtonId::kConfirm, raw_types[i]),
            5000 + static_cast<int64_t>(i),
            kFront);
        if (batch.count != 1 ||
            !IsEvent(batch, 0, ButtonId::kConfirm, raw_types[i])) {
            return false;
        }
    }

    // Deterministic 500-group mixed gate: every group must result in exactly
    // one semantic confirm, with doubles never leaking a preceding single.
    int singles = 0;
    int doubles = 0;
    int longs = 0;
    int64_t now_ms = 10000;
    for (int i = 0; i < 500; ++i) {
        const int kind = i % 3;
        if (kind == 0) {
            batch = arbiter.Process(
                MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
                now_ms,
                kFront);
            if (batch.count != 0) return false;
            batch = arbiter.Poll(
                now_ms + kConfirmDoublePressWindowMs,
                kFront);
            if (!IsEvent(batch, 0, ButtonId::kConfirm,
                         ButtonEventType::kShortPress) || batch.count != 1) {
                return false;
            }
            ++singles;
        } else if (kind == 1) {
            if (arbiter.Process(
                    MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
                    now_ms,
                    kFront).count != 0) {
                return false;
            }
            batch = arbiter.Process(
                MakeEvent(ButtonId::kConfirm, ButtonEventType::kShortPress),
                now_ms + 180,
                kFront);
            if (!IsEvent(batch, 0, ButtonId::kConfirm,
                         ButtonEventType::kDoublePress) || batch.count != 1) {
                return false;
            }
            ++doubles;
        } else {
            batch = arbiter.Process(
                MakeEvent(ButtonId::kConfirm, ButtonEventType::kLongPress, 1000),
                now_ms,
                kFront);
            if (!IsEvent(batch, 0, ButtonId::kConfirm,
                         ButtonEventType::kLongPress) || batch.count != 1) {
                return false;
            }
            ++longs;
        }
        now_ms += 1000;
    }
    return singles == 167 && doubles == 167 && longs == 166 &&
           !arbiter.has_pending_single();
}

}  // namespace wqn::ui
