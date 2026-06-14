// AI session page rendering: chat bubbles, input bar, status labels.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "ai_session.h"
#include "epd_display.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr size_t kAiAssistantCharsPerPage = 92;

const char* AiStatusLabel(wqn::AiSessionStatus status)
{
    switch (status) {
        case wqn::AiSessionStatus::kListening:
            return "录音";
        case wqn::AiSessionStatus::kWaitingReply:
            return "识别";
        case wqn::AiSessionStatus::kReplyReady:
            return "完成";
        case wqn::AiSessionStatus::kError:
            return "错误";
        case wqn::AiSessionStatus::kIdle:
        default:
            return "空闲";
    }
}

std::string AiAssistantFallbackText(const wqn::AiSessionState& ai)
{
    if (ai.status == wqn::AiSessionStatus::kListening) {
        return "正在录音，松手后上传识别。";
    }
    if (ai.status == wqn::AiSessionStatus::kWaitingReply) {
        return ai.pending_text.empty() ? "正在上传并等待模型回复..." : ai.pending_text;
    }
    if (ai.status == wqn::AiSessionStatus::kError) {
        return "请求失败，请长按确认重试。";
    }
    return "我会在这里显示转写和回答。";
}

esp_err_t DrawAiBubble(int x, int y, int width, int height, const std::string& text, bool me, bool pending)
{
    if (me) {
        FillRect(x, y, width, height, true);
    } else if (pending) {
        DrawRect(x, y, width, height);
        DrawRect(x + 2, y + 2, width - 4, height - 4);
    } else {
        DrawRect(x, y, width, height);
    }
    const int max_lines = std::max(1, (height - 14) / 18);
    return DrawWrappedText(x + 8, y + 7, width - 16, text, max_lines, !me);
}

esp_err_t DrawAiInputBar(const wqn::AiSessionState& ai, size_t page, size_t page_count)
{
    const bool listening = ai.status == wqn::AiSessionStatus::kListening;
    const bool waiting = ai.status == wqn::AiSessionStatus::kWaitingReply;
    const int x = 12;
    const int y = 252;
    const int width = 376;
    const int height = 36;
    if (listening) {
        FillRect(x, y, width, height, true);
    } else {
        DrawRect(x, y, width, height);
    }

    std::string label = "长按确认开始说话";
    std::string state = AiStatusLabel(ai.status);
    if (listening) {
        label = "正在输入";
        state = "录音";
    } else if (waiting) {
        label = "服务器处理中";
        state = "等待";
    } else if (ai.status == wqn::AiSessionStatus::kReplyReady) {
        label = "长按继续追问";
        state = "就绪";
    } else if (ai.status == wqn::AiSessionStatus::kError) {
        label = "长按重试";
        state = "错误";
    }
    if (page_count > 1 && (ai.status == wqn::AiSessionStatus::kReplyReady || ai.status == wqn::AiSessionStatus::kError)) {
        state = std::to_string(page + 1) + "/" + std::to_string(page_count);
    }

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x + 8, y + 9, label.c_str(), !listening), kTag, "draw AI input label");
    const int state_width = wqn::MeasureUtf8TextWidth(state.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(x + width - state_width - 8, y + 9, state.c_str(), !listening),
        kTag,
        "draw AI input state");

    if (listening) {
        const int wave_x = x + width - 74;
        const int base_y = y + 24;
        const int heights[] = {8, 17, 11, 15};
        for (int i = 0; i < 4; ++i) {
            FillRect(wave_x + i * 10, base_y - heights[i], 5, heights[i], false);
        }
    }
    return ESP_OK;
}

esp_err_t RenderAiToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::AiSessionState& ai = frame.ai;
    wqn::ClearEpdFramebuffer(true);
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 6, "AI", true), kTag, "draw AI title");
    std::string status = AiStatusLabel(ai.status);
    if (!frame.home.battery_label.empty()) {
        status += "  ";
        status += frame.home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true),
        kTag,
        "draw AI status");

    const bool has_user_text = !ai.user_text.empty();
    const std::string user_text = has_user_text ? ai.user_text : "长按确认键开始语音提问";
    const size_t text_page_count = wqn::AiSessionTextPageCount(ai);
    const size_t page_count = wqn::AiSessionPageCount(ai);
    const size_t page = std::min(ai.page, page_count > 0 ? page_count - 1 : 0);
    const bool action_page = !ai.function_call_summaries.empty() && page >= text_page_count;
    std::string assistant_text = action_page ? JoinAiFunctionCallSummaries(ai.function_call_summaries) : ai.assistant_text;
    if (assistant_text.empty()) {
        assistant_text = AiAssistantFallbackText(ai);
    }
    if (!action_page) {
        assistant_text = Utf8PageSlice(assistant_text, std::min(page, text_page_count - 1), kAiAssistantCharsPerPage);
    }
    if (action_page && assistant_text.empty()) {
        assistant_text = "没有云端动作。";
    }

    ESP_RETURN_ON_ERROR(
        DrawAiBubble(94, 44, 282, 58, user_text, true, ai.status == wqn::AiSessionStatus::kListening),
        kTag,
        "draw AI user bubble");
    ESP_RETURN_ON_ERROR(
        DrawAiBubble(24, 116, 352, 104, assistant_text, false, ai.status == wqn::AiSessionStatus::kWaitingReply),
        kTag,
        "draw AI assistant bubble");
    std::string detail_text;
    if ((ai.status == wqn::AiSessionStatus::kListening || ai.status == wqn::AiSessionStatus::kWaitingReply) &&
        !ai.pending_text.empty()) {
        detail_text = ai.pending_text;
    } else if (!ai.status_detail.empty()) {
        detail_text = ai.status_detail;
    } else if (!ai.function_call_summaries.empty()) {
        detail_text = ai.function_call_summaries.front();
    }
    if (!detail_text.empty()) {
        ESP_RETURN_ON_ERROR(DrawClippedText(28, 228, 344, detail_text), kTag, "draw AI detail text");
    }
    ESP_RETURN_ON_ERROR(DrawAiInputBar(ai, page, page_count), kTag, "draw AI input bar");
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
