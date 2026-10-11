// Todo page rendering: status bar, timeline nodes, cards, due-time parsing, sync status text.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

#include "display_service.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

bool IsAsciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string TodoDueTimeLabel(const std::string& due_at)
{
    for (size_t i = 0; i + 4 < due_at.size(); ++i) {
        if (!IsAsciiDigit(due_at[i]) || !IsAsciiDigit(due_at[i + 1]) || due_at[i + 2] != ':' ||
            !IsAsciiDigit(due_at[i + 3]) || !IsAsciiDigit(due_at[i + 4])) {
            continue;
        }
        const int hour = (due_at[i] - '0') * 10 + (due_at[i + 1] - '0');
        const int minute = (due_at[i + 3] - '0') * 10 + (due_at[i + 4] - '0');
        if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
            return due_at.substr(i, 5);
        }
    }
    return "--:--";
}

std::string TodoDueDateLabel(const std::string& due_at)
{
    if (due_at.size() >= 10 && IsAsciiDigit(due_at[0]) && IsAsciiDigit(due_at[1]) &&
        IsAsciiDigit(due_at[2]) && IsAsciiDigit(due_at[3]) && due_at[4] == '-' &&
        IsAsciiDigit(due_at[5]) && IsAsciiDigit(due_at[6]) && due_at[7] == '-' &&
        IsAsciiDigit(due_at[8]) && IsAsciiDigit(due_at[9])) {
        return due_at.substr(0, 10);
    }
    return "无到期日期";
}

const char* TodoSyncStatusText(wqn::TodoSyncStatus status)
{
    switch (status) {
        case wqn::TodoSyncStatus::kLoading:
            return "同步中";
        case wqn::TodoSyncStatus::kReady:
            return "已同步";
        case wqn::TodoSyncStatus::kSyncFailed:
            return "同步失败";
        case wqn::TodoSyncStatus::kCompleting:
            return "完成中";
        case wqn::TodoSyncStatus::kCompleteFailed:
            return "完成失败";
        case wqn::TodoSyncStatus::kCompleted:
            return "已完成";
        case wqn::TodoSyncStatus::kAuthRequired:
            return "未配对";
        case wqn::TodoSyncStatus::kIdle:
        default:
            return "待同步";
    }
}

std::string TodoItemStatusText(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status)
{
    if (sync_status == wqn::TodoSyncStatus::kLoading) {
        return "同步中";
    }
    if (sync_status == wqn::TodoSyncStatus::kSyncFailed) {
        return "同步失败";
    }
    if (selected && sync_status == wqn::TodoSyncStatus::kCompleting) {
        return "完成中";
    }
    if (selected && sync_status == wqn::TodoSyncStatus::kCompleteFailed) {
        return "完成失败";
    }
    if (!item.completed_at.empty() || item.status == "completed" || item.status == "done") {
        return "已完成";
    }
    if (item.status == "cancelled" || item.status == "canceled") {
        return "已取消";
    }
    if (item.status == "overdue") {
        return "已逾期";
    }
    if (item.status == "pending" || item.status.empty()) {
        return "待完成";
    }
    return item.status;
}

std::string TodoCardMetaLabel(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status)
{
    const std::string subject = item.subject_name.empty() ? "未分类" : item.subject_name;
    return subject + " · " + TodoItemStatusText(item, selected, sync_status) + " · " + TodoDueDateLabel(item.due_at);
}

std::string TodoStatusNote(const wqn::TodoUiState& todo)
{
    switch (todo.sync_status) {
        case wqn::TodoSyncStatus::kLoading:
            return "正在同步 Todo";
        case wqn::TodoSyncStatus::kCompleting:
            return "正在完成选中待办";
        case wqn::TodoSyncStatus::kCompleteFailed:
            return "完成失败，请稍后重试";
        case wqn::TodoSyncStatus::kCompleted:
            return "已完成选中待办";
        case wqn::TodoSyncStatus::kSyncFailed:
            return "同步失败，显示当前缓存";
        case wqn::TodoSyncStatus::kAuthRequired:
            return "请重新配对后同步";
        case wqn::TodoSyncStatus::kReady:
        case wqn::TodoSyncStatus::kIdle:
        default:
            break;
    }
    return todo.status_message;
}

int TodoPendingCount(const wqn::TodoUiState& todo)
{
    if (todo.total_pending > 0) {
        return todo.total_pending;
    }
    return static_cast<int>(todo.todos.size());
}

bool ParseTodoDueTime(const std::string& due_at, std::tm* due_tm)
{
    if (due_tm == nullptr || due_at.size() < 10 || !IsAsciiDigit(due_at[0]) || !IsAsciiDigit(due_at[1]) ||
        !IsAsciiDigit(due_at[2]) || !IsAsciiDigit(due_at[3]) || due_at[4] != '-' || !IsAsciiDigit(due_at[5]) ||
        !IsAsciiDigit(due_at[6]) || due_at[7] != '-' || !IsAsciiDigit(due_at[8]) || !IsAsciiDigit(due_at[9])) {
        return false;
    }

    std::tm parsed = {};
    parsed.tm_year = (due_at[0] - '0') * 1000 + (due_at[1] - '0') * 100 + (due_at[2] - '0') * 10 +
                     (due_at[3] - '0') - 1900;
    parsed.tm_mon = (due_at[5] - '0') * 10 + (due_at[6] - '0') - 1;
    parsed.tm_mday = (due_at[8] - '0') * 10 + (due_at[9] - '0');
    parsed.tm_hour = 23;
    parsed.tm_min = 59;
    parsed.tm_sec = 59;

    if (due_at.size() >= 16 && due_at[10] == 'T' && IsAsciiDigit(due_at[11]) && IsAsciiDigit(due_at[12]) &&
        due_at[13] == ':' && IsAsciiDigit(due_at[14]) && IsAsciiDigit(due_at[15])) {
        parsed.tm_hour = (due_at[11] - '0') * 10 + (due_at[12] - '0');
        parsed.tm_min = (due_at[14] - '0') * 10 + (due_at[15] - '0');
        if (due_at.size() >= 19 && due_at[16] == ':' && IsAsciiDigit(due_at[17]) && IsAsciiDigit(due_at[18])) {
            parsed.tm_sec = (due_at[17] - '0') * 10 + (due_at[18] - '0');
        } else {
            parsed.tm_sec = 0;
        }
    }

    if (parsed.tm_mon < 0 || parsed.tm_mon > 11 || parsed.tm_mday < 1 || parsed.tm_mday > 31 ||
        parsed.tm_hour < 0 || parsed.tm_hour > 23 || parsed.tm_min < 0 || parsed.tm_min > 59 ||
        parsed.tm_sec < 0 || parsed.tm_sec > 59) {
        return false;
    }
    *due_tm = parsed;
    return true;
}

int TodoOverdueCount(const wqn::TodoUiState& todo)
{
    int count = 0;
    const std::time_t now = CurrentUnixTime();
    const bool has_valid_clock = now >= kMinReasonableUnixTime;
    for (const wqn::WqnTodoItem& item : todo.todos) {
        if (item.status == "overdue") {
            ++count;
            continue;
        }
        if (!has_valid_clock || item.due_at.empty() || item.status == "completed" || item.status == "done" ||
            item.status == "cancelled" || item.status == "canceled") {
            continue;
        }
        std::tm due_tm = {};
        if (!ParseTodoDueTime(item.due_at, &due_tm)) {
            continue;
        }
        const std::time_t due_time = mktime(&due_tm);
        if (due_time != static_cast<std::time_t>(-1) && due_time < now) {
            ++count;
        }
    }
    return count;
}

size_t TodoVisibleStart(const wqn::TodoUiState& todo, size_t selected, size_t visible_count)
{
    if (todo.todos.size() <= visible_count) {
        return 0;
    }
    if (selected + 1 >= visible_count) {
        return std::min(selected + 1 - visible_count, todo.todos.size() - visible_count);
    }
    return 0;
}

void DrawDashedVerticalLine(int x, int y, int height)
{
    constexpr int kDash = 6;
    constexpr int kGap = 5;
    for (int offset = 0; offset < height; offset += kDash + kGap) {
        DrawVerticalLine(x, y + offset, std::min(kDash, height - offset));
    }
}

void DrawTimelineNode(int cx, int cy, bool selected)
{
    constexpr int kRadius = 6;
    constexpr int kInnerRadius = 3;
    for (int dy = -kRadius; dy <= kRadius; ++dy) {
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
            const int distance = dx * dx + dy * dy;
            if (distance > kRadius * kRadius) {
                continue;
            }
            // [L3-cleanup] Removed a dead white-pixel write that was immediately
            // overwritten by the black/white write below. Selected = solid black
            // disc; unselected = black ring (distance >= inner^2) + white core.
            const bool black = selected || distance >= kInnerRadius * kInnerRadius;
            wqn::DrawEpdPixel(cx + dx, cy + dy, black);
        }
    }
}

esp_err_t DrawTodoStatusBar(const wqn::TodoUiState& todo, const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, kStatusBarDividerY, wqn::kEpdWidth);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(kMarginX, 6, "Todo", true), kTag, "draw todo title");

    // [v2] Status bar carries only the global four-piece set (title + clock +
    // wifi + battery). The 今日/逾期 counts that used to crowd here (and get
    // truncated) move into the content area as a summary line under the cards;
    // the sync status stays visible there too. Right inset aligned to kMarginX.
    const int icons_left = DrawStatusBarIcons(wqn::kEpdWidth - kMarginX, 6, home);
    std::string status = TodoSyncStatusText(todo.sync_status);
    status += "  " + CurrentClockLabel();
    const int max_width = std::max(0, icons_left - 6 - kMarginX);
    const std::string clipped = wqn::TruncateUtf8TextToWidth(status, max_width);
    const int status_width = wqn::MeasureUtf8TextWidth(clipped.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(kMarginX, icons_left - 6 - status_width), 6, clipped.c_str(), true),
        kTag,
        "draw todo status");
    return ESP_OK;
}

esp_err_t DrawTodoCard(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status, int x, int y, int width, int height)
{
    // Card outline: drawn by the focus decoration when selected (rounded outer
    // r6 + 2px-inset concentric inner), or as a plain rounded outline when not,
    // so the todo cards share the product's rounded-card visual language with
    // the word home cards. One path owns the outline.
    if (selected) {
        DrawSelectionDecoration(x, y, width, height, SelectionStyle::kRoundedInnerBorder);
    } else {
        DrawRoundedRect(x, y, width, height, kRoundedOuterRadius);
    }

    const std::string title = item.title.empty() ? "未命名 Todo" : item.title;
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 8, y + 7, 48, TodoDueTimeLabel(item.due_at)), kTag, "draw todo time");
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 62, y + 7, width - 70, title), kTag, "draw todo title text");
    ESP_RETURN_ON_ERROR(
        DrawClippedText(x + 8, y + 31, width - 16, TodoCardMetaLabel(item, selected, sync_status)),
        kTag,
        "draw todo meta");
    return ESP_OK;
}

esp_err_t DrawTodoEmptyState(const wqn::TodoUiState& todo)
{
    std::string title = "暂无 Todo";
    std::string body = "今天没有待办事项";
    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired) {
        title = "设备未配对";
        body = "请先在网页端扫码配对";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kSyncFailed) {
        title = "Todo 同步失败";
        body = "请检查 WiFi 后重试";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kLoading) {
        title = "正在同步 Todo";
        body = "正在从云端获取今天的待办";
    }
    // [v2] Unified empty-state surface (rounded container, title/body/hint).
    return DrawEmptyState(title, body, TodoSyncStatusText(todo.sync_status));
}

esp_err_t RenderTodoToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::TodoUiState& todo = frame.todo;
    wqn::ClearEpdFramebuffer(true);
    ESP_RETURN_ON_ERROR(DrawTodoStatusBar(todo, frame.home), kTag, "draw todo status bar");

    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired || todo.todos.empty()) {
        ESP_RETURN_ON_ERROR(DrawTodoEmptyState(todo), kTag, "draw todo empty state");
        return RefreshFrame(frame, schedule);
    }

    constexpr size_t kMaxVisibleTodos = 4;
    constexpr int kTimelineX = 34;
    constexpr int kCardX = 54;
    constexpr int kCardY = 38;
    constexpr int kCardWidth = 336;
    constexpr int kCardHeight = 54;
    constexpr int kCardGap = 8;

    const size_t selected = std::min(todo.selected, todo.todos.size() - 1);
    const size_t visible_count = std::min(kMaxVisibleTodos, todo.todos.size());
    const size_t start = TodoVisibleStart(todo, selected, visible_count);
    const int timeline_start_y = kCardY + 13;
    const int timeline_height = static_cast<int>((visible_count - 1) * (kCardHeight + kCardGap)) + 1;
    DrawDashedVerticalLine(kTimelineX, timeline_start_y, std::max(1, timeline_height));

    for (size_t visible_index = 0; visible_index < visible_count; ++visible_index) {
        const size_t item_index = start + visible_index;
        const bool is_selected = item_index == selected;
        const int y = kCardY + static_cast<int>(visible_index) * (kCardHeight + kCardGap);
        DrawTimelineNode(kTimelineX, y + 13, is_selected);
        ESP_RETURN_ON_ERROR(
            DrawTodoCard(todo.todos[item_index], is_selected, todo.sync_status, kCardX, y, kCardWidth, kCardHeight),
            kTag,
            "draw todo card");
    }

    // [v2] Footer summary line: the 今日/逾期 counts moved out of the status bar
    // live here, joined with any transient sync note.
    std::string footer = "今日 " + std::to_string(TodoPendingCount(todo));
    footer += " · 逾期 " + std::to_string(TodoOverdueCount(todo));
    const std::string note = TodoStatusNote(todo);
    if (!note.empty()) {
        footer += " · " + note;
    }
    ESP_RETURN_ON_ERROR(DrawClippedText(kCardX, 282, kCardWidth, footer), kTag, "draw todo footer");
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
