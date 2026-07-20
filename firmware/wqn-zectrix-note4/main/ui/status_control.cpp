#include "ui/status_control.h"

#include "ai_session.h"
#include "esp_log.h"
#include "flash_session.h"
#include "services/sync_service.h"
#include "ui/ui_internal.h"

namespace device_ui_internal {
namespace {

constexpr char kTag[] = "wqn_status_shell";

RefreshSchedule ProviderRefreshSchedule(wqn::StatusControlProvider provider)
{
    return provider == wqn::StatusControlProvider::kWord
        ? RefreshSchedule::kStatus
        : RefreshSchedule::kSelection;
}

RefreshSchedule CloseAndRefresh(wqn::UiState* state)
{
    const wqn::StatusControlProvider provider = state == nullptr
        ? wqn::StatusControlProvider::kNone
        : state->status_edit.provider;
    CloseStatusControls(state);
    return ProviderRefreshSchedule(provider);
}

void CycleAiToggle(wqn::UiState* state, StatusControlAction action)
{
    switch (action) {
        case StatusControlAction::kAiThinking: {
            const int count = static_cast<int>(wqn::ThinkingLevel::kCount);
            const int next =
                (static_cast<int>(state->ai.thinking_level) + 1) % count;
            state->ai.thinking_level = static_cast<wqn::ThinkingLevel>(next);
            wqn::SetAiThinkingLevel(state->ai.thinking_level);
            break;
        }
        case StatusControlAction::kAiTts:
            state->ai.tts_on = !state->ai.tts_on;
            wqn::SetAiTtsOn(state->ai.tts_on);
            break;
        case StatusControlAction::kAiExpand:
            state->ai.expand_content = !state->ai.expand_content;
            wqn::SetAiExpandContent(state->ai.expand_content);
            break;
        default:
            break;
    }
}

RefreshSchedule InvokeControl(
    const StatusControlDescriptor& descriptor,
    wqn::UiState* state)
{
    if (!descriptor.enabled) return RefreshSchedule::kNone;
    switch (descriptor.action) {
        case StatusControlAction::kAiTier: {
            const wqn::AiTier previous = state->ai.tier;
            const wqn::AiTier next = wqn::NextAiTier(previous);
            state->ai.tier = next;
            wqn::SetAiTier(next);
            if (previous == wqn::AiTier::kFlash && next != wqn::AiTier::kFlash) {
                wqn::StopFlashSession();
            }
            CloseStatusControls(state);
            wqn::RequestForceFullRefresh();
            ESP_LOGI(kTag, "AI tier changed: tier=%d", static_cast<int>(next));
            return RefreshSchedule::kAi;
        }
        case StatusControlAction::kAiThinking:
        case StatusControlAction::kAiTts:
        case StatusControlAction::kAiExpand:
            CycleAiToggle(state, descriptor.action);
            return RefreshSchedule::kSelection;
        case StatusControlAction::kAiClearContext:
            wqn::ClearAiConversationContext();
            CloseStatusControls(state);
            wqn::RequestForceFullRefresh();
            return RefreshSchedule::kSelection;
        case StatusControlAction::kWordScope:
        {
            const std::string previous_scope =
                state->word_app.preferred_scope_deck_id;
            const bool previous_pending = state->word_app.scope_change_pending;
            wqn::CycleWordScopeForNextSession(&state->word_app);
            const esp_err_t result = wqn::SaveWordScopePreference(
                state->word_app.preferred_scope_deck_id);
            if (result != ESP_OK) {
                state->word_app.preferred_scope_deck_id = previous_scope;
                state->word_app.scope_change_pending = previous_pending;
                state->word_app.message = "词库范围保存失败";
                ESP_LOGW(kTag, "save word scope failed: %s", esp_err_to_name(result));
            }
            return RefreshSchedule::kStatus;
        }
        case StatusControlAction::kWordSync:
            wqn::services::RequestSyncNow();
            state->status.last_sync_status = "同步中";
            state->settings.sync_status = "同步中";
            state->word_app.message = "已请求同步";
            return RefreshSchedule::kStatus;
        case StatusControlAction::kNone:
            return RefreshSchedule::kNone;
    }
    return RefreshSchedule::kNone;
}

}  // namespace

StatusControlProvider ResolveStatusControlProvider(const wqn::UiState& state)
{
    if (state.screen == wqn::UiScreen::kAi) {
        return {
            wqn::StatusControlProvider::kAi,
            static_cast<uint8_t>(state.ai.tier == wqn::AiTier::kFlash ? 1 : 5),
        };
    }
    if (state.screen == wqn::UiScreen::kWord) {
        return {wqn::StatusControlProvider::kWord, 2};
    }
    return {};
}

StatusControlDescriptor DescribeStatusControl(
    const wqn::UiState& state,
    uint8_t index)
{
    const StatusControlProvider provider = ResolveStatusControlProvider(state);
    if (index >= provider.control_count) return {};
    if (provider.id == wqn::StatusControlProvider::kAi) {
        switch (index) {
            case 0:
                return {StatusControlAction::kAiTier, "模式", wqn::AiTierLabel(state.ai.tier), true};
            case 1:
                return {StatusControlAction::kAiThinking, "思考", std::to_string(static_cast<int>(state.ai.thinking_level)), true};
            case 2:
                return {StatusControlAction::kAiTts, "朗读", state.ai.tts_on ? "开" : "关", true};
            case 3:
                return {StatusControlAction::kAiExpand, "展开", state.ai.expand_content ? "开" : "关", true};
            case 4:
                return {StatusControlAction::kAiClearContext, "清空", "", true};
            default:
                return {};
        }
    }
    if (provider.id == wqn::StatusControlProvider::kWord) {
        if (index == 0) {
            return {
                StatusControlAction::kWordScope,
                "范围",
                wqn::WordScopeControlLabel(state.word_app),
                !state.word_app.pack_index.pack_identities.empty(),
            };
        }
        return {
            StatusControlAction::kWordSync,
            "同步",
            state.status.last_sync_status,
            true,
        };
    }
    return {};
}

bool OpenStatusControls(int64_t now_ms, wqn::UiState* state)
{
    if (state == nullptr) return false;
    const StatusControlProvider provider = ResolveStatusControlProvider(*state);
    if (provider.id == wqn::StatusControlProvider::kNone ||
        provider.control_count == 0) {
        return false;
    }
    state->status_edit.active = true;
    state->status_edit.provider = provider.id;
    state->status_edit.owner_screen = state->screen;
    state->status_edit.selected = 0;
    state->status_edit.last_action_ms = now_ms;
    ESP_LOGI(
        kTag,
        "status controls opened: provider=%d screen=%d",
        static_cast<int>(provider.id),
        static_cast<int>(state->screen));
    return true;
}

void CloseStatusControls(wqn::UiState* state)
{
    if (state == nullptr) return;
    state->status_edit = {};
}

RefreshSchedule ApplyStatusControlEvent(
    const wqn::ButtonEvent& event,
    int64_t now_ms,
    wqn::UiState* state)
{
    if (state == nullptr || !state->status_edit.active || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }
    if (state->screen != state->status_edit.owner_screen ||
        ResolveStatusControlProvider(*state).id != state->status_edit.provider) {
        CloseStatusControls(state);
        return RefreshSchedule::kNone;
    }

    if (event.button == wqn::ButtonId::kConfirm) {
        if (event.type == wqn::ButtonEventType::kDoublePress ||
            event.type == wqn::ButtonEventType::kLongRelease) {
            ESP_LOGI(kTag, "status controls closed");
            return CloseAndRefresh(state);
        }
        if (event.type == wqn::ButtonEventType::kShortPress) {
            state->status_edit.last_action_ms = now_ms;
            return InvokeControl(
                DescribeStatusControl(*state, state->status_edit.selected),
                state);
        }
        return RefreshSchedule::kNone;
    }

    if ((event.button == wqn::ButtonId::kUp ||
         event.button == wqn::ButtonId::kDownPower) &&
        (event.type == wqn::ButtonEventType::kShortPress ||
         event.type == wqn::ButtonEventType::kLongPress)) {
        const StatusControlProvider provider =
            ResolveStatusControlProvider(*state);
        if (provider.control_count == 0) return RefreshSchedule::kNone;
        const int delta = event.button == wqn::ButtonId::kUp ? -1 : 1;
        int selected = static_cast<int>(state->status_edit.selected) + delta;
        if (selected < 0) selected = provider.control_count - 1;
        if (selected >= provider.control_count) selected = 0;
        state->status_edit.selected = static_cast<uint8_t>(selected);
        state->status_edit.last_action_ms = now_ms;
        return ProviderRefreshSchedule(state->status_edit.provider);
    }
    return RefreshSchedule::kNone;
}

bool RunStatusControlShellSelfTest()
{
    const wqn::WordAppMode modes[] = {
        wqn::WordAppMode::kHome,
        wqn::WordAppMode::kReviewFront,
        wqn::WordAppMode::kReviewBack,
        wqn::WordAppMode::kDictionary,
    };
    for (const wqn::WordAppMode mode : modes) {
        wqn::UiState state;
        state.screen = wqn::UiScreen::kWord;
        state.word_app.mode = mode;
        if (!OpenStatusControls(100, &state) ||
            !state.status_edit.active || state.word_app.mode != mode) {
            return false;
        }
        wqn::ButtonEvent exit;
        exit.button = wqn::ButtonId::kConfirm;
        exit.type = wqn::ButtonEventType::kDoublePress;
        ApplyStatusControlEvent(exit, 200, &state);
        if (state.status_edit.active || state.word_app.mode != mode) {
            return false;
        }
    }
    wqn::UiState home;
    home.screen = wqn::UiScreen::kHome;
    return !OpenStatusControls(100, &home) && !home.status_edit.active;
}

}  // namespace device_ui_internal
