#include "ui_internal.h"
#include "ai_session.h"
#include "ai_native_text.h"

namespace device_ui_internal {

// Use the Agent decision renderer and its two-slot window unchanged. The
// projection contains only presentation fields; STD never enters OpenCode.
wqn::AgentSessionState StdDecisionSurface(const wqn::AiSessionState& ai)
{
    wqn::AgentSessionState surface;
    surface.ui.requires_confirmation = ai.status == wqn::AiSessionStatus::kAwaitingConfirmation;
    surface.ui.prompt_text = ai.pending_input;
    surface.pending_question_id = ai.interaction.id;
    surface.pending_question_title = ai.interaction.prompt;
    surface.question_allow_custom = ai.interaction.allow_custom;
    for (const auto& option : ai.interaction.options)
        surface.pending_question_options.push_back({option.value, option.label});
    return surface;
}

int StdAiBottomReserve(const wqn::AiSessionState& ai)
{
    return ai.status == wqn::AiSessionStatus::kAwaitingConfirmation || !ai.interaction.id.empty()
               ? kAiBottomBandH
               : 0;
}

void DrawStdAiInteraction(const wqn::UiFrame& frame)
{
    const auto& ai = frame.ai;
    if (ai.status == wqn::AiSessionStatus::kTranscribing) {
        DrawAgentPendingBubble(ai.user_partial.empty() ? "正在识别刚才说的话..." : ai.user_partial,
                               "转写中");
    } else if (ai.status == wqn::AiSessionStatus::kAwaitingConfirmation) {
        const auto surface = StdDecisionSurface(ai);
        DrawAgentPendingBubble(ai.pending_input, "确认发送", 12);
        DrawAgentOptionBar(AgentOptionMode::kConfirmSend, frame.agent_option.focused, surface);
    } else if (!ai.interaction.id.empty()) {
        const auto surface = StdDecisionSurface(ai);
        const char* title = ai.interaction.sending           ? "正在提交回答"
                            : !ai.interaction.error.empty()  ? "回答未确认，请重试"
                            : ai.interaction.kind == "write" ? "确认执行"
                                                             : "提问";
        DrawAgentPendingBubble(ai.interaction.prompt, title, 12);
        DrawAgentOptionBar(AgentOptionMode::kQuestion, frame.agent_option.focused, surface);
    } else if (ai.status == wqn::AiSessionStatus::kError && !ai.assistant_text.empty()) {
        DrawAgentPendingBubble(ai.assistant_text, "未完成");
    }
}

bool ApplyStdAiInteractionEvent(const wqn::ButtonEvent& event, int64_t now_ms, wqn::UiState* state,
                                RefreshSchedule* schedule)
{
    if (state->screen != wqn::UiScreen::kAi || state->ai.tier != wqn::AiTier::kStd)
        return false;
    const bool input = state->ai.status == wqn::AiSessionStatus::kAwaitingConfirmation;
    if (!input && state->ai.interaction.id.empty())
        return false;
    *schedule = RefreshSchedule::kNone;
    if (state->ai.interaction.sending)
        return true;
    const int64_t armed_at =
        input ? state->ai.input_armed_at_ms : state->ai.interaction.armed_at_ms;
    auto& pressed_at = state->gestures.std_decision_press_ms;
    if (event.button == wqn::ButtonId::kConfirm) {
        if (event.type == wqn::ButtonEventType::kPress) {
            pressed_at = now_ms >= armed_at ? now_ms : 0;
            return true;
        }
        if (event.type == wqn::ButtonEventType::kHoldPress ||
            event.type == wqn::ButtonEventType::kRelease)
            pressed_at = 0;
        if (event.type == wqn::ButtonEventType::kShortPress ||
            event.type == wqn::ButtonEventType::kDoublePress) {
            const bool fresh_press = pressed_at > 0 && pressed_at >= armed_at;
            pressed_at = 0;
            if (!fresh_press)
                return true;
        }
    }
    auto& focus = state->agent_option.focused;
    const int count = input ? 2
                            : static_cast<int>(state->ai.interaction.options.size()) +
                                  (state->ai.interaction.allow_custom ? 1 : 0);
    if (count <= 0)
        return true;
    if (focus >= count)
        focus = 0;
    if ((event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower) &&
        event.type == wqn::ButtonEventType::kShortPress) {
        if (state->ai.interaction.reply_value.empty()) {
            const int delta = event.button == wqn::ButtonId::kUp ? -1 : 1;
            focus = static_cast<uint8_t>((focus + delta + count) % count);
            *schedule = RefreshSchedule::kAi;
        }
    } else if (event.button == wqn::ButtonId::kConfirm &&
               (event.type == wqn::ButtonEventType::kShortPress ||
                event.type == wqn::ButtonEventType::kDoublePress)) {
        esp_err_t result = ESP_OK;
        if (input) {
            if (focus == 0)
                result = wqn::ConfirmAiPrompt(now_ms);
            else
                wqn::CancelAiPrompt();
        } else
            result = wqn::ReplyAiInteraction(focus);
        if (result == ESP_OK) {
            wqn::AiSessionState snapshot;
            if (wqn::CopyAiSessionToUi(&snapshot))
                state->ai = std::move(snapshot);
            *schedule = RefreshSchedule::kAi;
        }
    }
    // A modal decision owns the entire physical gesture, including hold and
    // release tails. None of them can start recording or edit the status bar.
    return true;
}

} // namespace device_ui_internal
