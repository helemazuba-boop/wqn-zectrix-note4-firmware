// AI session page rendering: chat bubbles, input bar, status labels.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "ai_session.h"
#include "epd_display.h"
#include "esp_log.h"
#include "flash_session.h"

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
        case wqn::AiSessionStatus::kStreaming:
            return "流式";
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
    if (ai.status == wqn::AiSessionStatus::kStreaming) {
        return ai.pending_text.empty() ? "服务器处理中…" : ai.pending_text;
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
    } else if (ai.status == wqn::AiSessionStatus::kStreaming) {
        // Show the latest tool chip on the input bar while SSE is live.
        if (!ai.status_detail.empty()) {
            label = ai.status_detail;
            // [tool-display] Add a blinker on the right side of the input bar
            // so the user can tell a tool is still mid-execution (the server
            // hasn't pushed the matching tool.result/tool.error event yet).
            state = "执行中▏";
        } else {
            label = "服务器处理中";
            state = "流式";
        }
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
    const char* tier_label = wqn::AiTierLabel(ai.tier);
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(10 + 30, 6, tier_label, true),
        kTag,
        "draw AI tier label");
    std::string status = AiStatusLabel(ai.status);

    // Flash realtime mode rendering
    if (ai.tier == wqn::AiTier::kFlash) {
        // Read Flash status from the local state copy, NOT from
        // wqn::GetFlashStatus() — calling that would re-acquire the mutex
        // inside the render path and stall the UI thread.
        if (ai.flash_is_streaming) {
            status = "实时";
        } else if (ai.status == wqn::AiSessionStatus::kListening) {
            status = "连接中";
        } else if (ai.status == wqn::AiSessionStatus::kError) {
            status = "错误";
        } else {
            status = "就绪";
        }

        if (!frame.home.battery_label.empty()) {
            status += "  ";
            status += frame.home.battery_label;
        }
        const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
        ESP_RETURN_ON_ERROR(
            wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true),
            kTag,
            "draw flash status");

        // Recording indicator wave
        if (ai.flash_is_streaming) {
            const int wave_x = 180;
            const int base_y = 140;
            const int heights[] = {10, 20, 14, 18, 8};
            for (int i = 0; i < 5; ++i) {
                FillRect(wave_x + i * 12, base_y - heights[i], 6, heights[i], true);
            }
            ESP_RETURN_ON_ERROR(
                wqn::DrawUtf8Text(28, 60, "正在录音...", true),
                kTag, "draw flash recording label");
        } else if (ai.status == wqn::AiSessionStatus::kListening) {
            ESP_RETURN_ON_ERROR(
                wqn::DrawUtf8Text(28, 60, ai.flash_pending.empty() ? "正在连接..." : ai.flash_pending.c_str(), false),
                kTag, "draw flash connecting");
        } else if (ai.status == wqn::AiSessionStatus::kError) {
            std::string err = ai.flash_error.empty() ? "连接失败" : ai.flash_error;
            ESP_RETURN_ON_ERROR(
                wqn::DrawUtf8Text(28, 60, ("错误: " + err).c_str(), false),
                kTag, "draw flash error");
        } else {
            ESP_RETURN_ON_ERROR(
                wqn::DrawUtf8Text(28, 60, "长按开始说话", false),
                kTag, "draw flash idle");
        }

        // Show transcript
        if (!ai.flash_transcript.empty()) {
            ESP_RETURN_ON_ERROR(
                DrawClippedText(28, 160, 344, ("你说: " + ai.flash_transcript).c_str()),
                kTag, "draw flash transcript");
        }
        if (!ai.assistant_text.empty()) {
            ESP_RETURN_ON_ERROR(
                DrawClippedText(28, 200, 344, ("助手: " + ai.assistant_text).c_str()),
                kTag, "draw flash assistant");
        }

        // Flash input bar
        const int x = 12;
        const int y = 252;
        const int width = 376;
        const int height = 36;
        const char* bar_label = nullptr;
        bool bar_filled = false;
        if (ai.flash_is_streaming) {
            bar_label = "松开停止，短按发送";
            bar_filled = true;
        } else if (ai.status == wqn::AiSessionStatus::kListening) {
            bar_label = "连接中...";
        } else if (ai.status == wqn::AiSessionStatus::kError) {
            bar_label = "长按重试";
        } else {
            bar_label = "长按说话";
        }
        if (bar_filled) {
            FillRect(x, y, width, height, true);
            ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x + 8, y + 9, bar_label, false), kTag, "draw flash bar label");
        } else {
            DrawRect(x, y, width, height);
            ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x + 8, y + 9, bar_label, true), kTag, "draw flash bar label");
        }
        return RefreshFrame(frame, schedule);
    }

    if (!frame.home.battery_label.empty()) {
        status += "  ";
        status += frame.home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true),
        kTag,
        "draw AI status");

    const bool has_user_text = !ai.user_text.empty() || !ai.user_partial.empty();
    const std::string user_text = has_user_text
        ? (ai.user_text.empty() ? ai.user_partial : ai.user_text)
        : "长按确认键开始语音提问";
    const size_t text_page_count = wqn::AiSessionTextPageCount(ai);
    const size_t page_count = wqn::AiSessionPageCount(ai);
    const size_t page = std::min(ai.page, page_count > 0 ? page_count - 1 : 0);
    const bool action_page = !ai.function_call_summaries.empty() && page >= text_page_count;
    // During v2 streaming the assistant_text grows lazily; for the first paint
    // we splice assistant_partial in so the user sees characters appear.
    std::string assistant_text = action_page ? FormatAiFunctionCallSummaries(ai.function_call_summaries)
                                             : (ai.assistant_text + ai.assistant_partial);
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
