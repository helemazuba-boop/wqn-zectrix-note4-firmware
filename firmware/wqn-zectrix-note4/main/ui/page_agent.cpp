// OpenCode Agent page: compact vibecoding-style prompt/reply split with a
// mandatory transcript confirmation state. The renderer consumes only the
// backend-neutral AiFeatureUiState projection.

#include "ui_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include "ai_feature.h"
#include "display_service.h"

namespace device_ui_internal {

namespace {

constexpr int kTopBarBottom = 27;
constexpr int kContextY = 34;
constexpr int kPromptTitleY = 56;
constexpr int kPromptBodyY = 76;
constexpr int kPromptDividerY = 122;
constexpr int kReplyTitleY = 128;
constexpr int kReplyBodyY = 150;
constexpr int kLineHeight = 18;
constexpr int kBodyWidth = 376;

#define AGENT_DRAW_TEXT(...) do { (void)wqn::DrawUtf8Text(__VA_ARGS__); } while (0)

std::string OneLine(const std::string& text, int width)
{
    const std::vector<std::string> lines = wqn::WrapUtf8TextToWidth(text, width, 1);
    return lines.empty() ? std::string() : lines.front();
}

void DrawTopBar(const wqn::AgentSessionState& state, const wqn::HomeSummary& home)
{
    AGENT_DRAW_TEXT(8, 6, "OpenCode", true);
    const std::string status = state.ui.status_label.empty()
        ? wqn::AiFeaturePhaseLabel(state.ui.phase)
        : state.ui.status_label;
    const std::string status_line = OneLine(status, 142);
    const int status_width = wqn::MeasureUtf8TextWidth(status_line.c_str());
    AGENT_DRAW_TEXT(std::max(130, 312 - status_width), 6, status_line.c_str(), true);
    const std::string network = home.wifi_connected ? "WiFi" : "离线";
    AGENT_DRAW_TEXT(330, 6, network.c_str(), true);
    DrawHorizontalLine(0, kTopBarBottom, wqn::kEpdWidth);
}

void DrawSessionPicker(const wqn::AgentSessionState& state)
{
    AGENT_DRAW_TEXT(12, kContextY, "选择 Session · ↑/↓移动 · 确认锁定", true);
    DrawHorizontalLine(8, 52, 384);
    if (state.sessions.empty()) {
        AGENT_DRAW_TEXT(12, 82, state.ui.activity_text.c_str(), true);
    } else {
        const size_t selected = std::min(state.selected_session, state.sessions.size() - 1);
        const size_t start = selected > 3 ? selected - 3 : 0;
        const size_t end = std::min(state.sessions.size(), start + static_cast<size_t>(7));
        int y = 60;
        for (size_t index = start; index < end; ++index) {
            const bool focused = index == selected;
            if (focused) {
                FillRoundedRect(8, y - 3, 384, 25, 5);
            }
            const std::string title = OneLine(
                state.sessions[index].title.empty() ? state.sessions[index].id
                                                    : state.sessions[index].title,
                360);
            AGENT_DRAW_TEXT(16, y, title.c_str(), !focused);
            y += 28;
        }
    }
}

void DrawInteraction(const wqn::AgentSessionState& state)
{
    const std::string context = state.current_session_title.empty()
        ? state.current_session_id
        : state.current_session_title;
    AGENT_DRAW_TEXT(12, kContextY, OneLine(context, 370).c_str(), true);

    AGENT_DRAW_TEXT(12, kPromptTitleY, "输入", true);
    const std::string action = OneLine(
        state.ui.action_hint.empty() ? "长按确认录音" : state.ui.action_hint,
        300);
    const int action_width = wqn::MeasureUtf8TextWidth(action.c_str());
    AGENT_DRAW_TEXT(std::max(70, 392 - action_width), kPromptTitleY, action.c_str(), true);
    const std::string prompt = state.ui.prompt_text.empty()
        ? "长按确认键录音，转写后需要再次确认"
        : state.ui.prompt_text;
    const std::vector<std::string> prompt_lines =
        wqn::WrapUtf8TextToWidth(prompt, kBodyWidth, 2);
    int y = kPromptBodyY;
    for (const std::string& line : prompt_lines) {
        AGENT_DRAW_TEXT(12, y, line.c_str(), true);
        y += kLineHeight;
    }

    DrawHorizontalLine(8, kPromptDividerY, 384);
    AGENT_DRAW_TEXT(12, kReplyTitleY, "Agent 回复", true);
    if (!state.ui.activity_text.empty()) {
        const std::string activity = OneLine(state.ui.activity_text, 238);
        const int width = wqn::MeasureUtf8TextWidth(activity.c_str());
        AGENT_DRAW_TEXT(std::max(150, 392 - width), kReplyTitleY, activity.c_str(), true);
    }

    const std::string reply = state.ui.response_text.empty()
        ? (wqn::AiFeaturePhaseIsBusy(state.ui.phase)
               ? "OpenCode 正在分析代码与执行工具…"
               : "暂无 Agent 输出")
        : state.ui.response_text;
    const std::vector<std::string> reply_lines =
        wqn::WrapUtf8TextToWidth(reply, kBodyWidth, 128);
    const size_t max_visible = 8;
    const size_t max_offset = reply_lines.size() > max_visible
        ? reply_lines.size() - max_visible
        : 0;
    const size_t distance_from_latest = std::min<size_t>(
        static_cast<size_t>(std::max<int32_t>(0, state.ui.scroll_offset_lines)),
        max_offset);
    const size_t offset = max_offset - distance_from_latest;
    y = kReplyBodyY;
    for (size_t index = offset;
         index < reply_lines.size() && index < offset + max_visible;
         ++index) {
        AGENT_DRAW_TEXT(12, y, reply_lines[index].c_str(), true);
        y += kLineHeight;
    }
}

}  // namespace

esp_err_t RenderAgentToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    wqn::ClearEpdFramebuffer(true);
    DrawTopBar(frame.agent, frame.home);
    if (!frame.agent.session_locked) {
        DrawSessionPicker(frame.agent);
    } else {
        DrawInteraction(frame.agent);
    }
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
