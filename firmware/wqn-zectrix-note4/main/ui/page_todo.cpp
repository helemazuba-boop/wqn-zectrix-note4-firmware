// Todo Editorial reference: native four-row task sheet and async feedback.
#include "ui_internal.h"
#include "ui_widgets.h"
#include "ai_native_text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

namespace device_ui_internal {
namespace {
constexpr int64_t kBeijingOffset = 8 * 3600;

// EPD-owner-only cache of the successfully drawn body. Clock-only frames may
// retain it, but a failed presentation must never establish a new baseline.
std::string g_todo_body_signature;
bool g_todo_body_valid = false;

struct TodoDue {
    std::time_t instant = 0;
    std::tm local = {};
    bool timed = false;
};

bool ReadDigits(const std::string& text, size_t offset, size_t count, int* value)
{
    if (offset + count > text.size()) return false;
    *value = 0;
    for (size_t i = offset; i < offset + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        *value = *value * 10 + text[i] - '0';
    }
    return true;
}

// Wire instants retain their zone; date-only values retain their calendar day.
// Do not feed UTC wall-clock fields to mktime() under the device's CST-8 TZ.
bool ParseTodoDue(const std::string& text, TodoDue* due)
{
    int year = 0, month = 0, day = 0, hour = 23, minute = 59, second = 59;
    if (!due || text.size() < 10 || text[4] != '-' || text[7] != '-' ||
        !ReadDigits(text, 0, 4, &year) || !ReadDigits(text, 5, 2, &month) ||
        !ReadDigits(text, 8, 2, &day) || year < 1970 || month < 1 || month > 12) return false;
    constexpr int days_per_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day < 1 || day > days_per_month[month - 1] + (month == 2 && leap)) return false;
    const bool timed = text.size() != 10;
    int offset = static_cast<int>(kBeijingOffset);
    if (timed) {
        if (text.size() < 16 || (text[10] != 'T' && text[10] != ' ') || text[13] != ':' ||
            !ReadDigits(text, 11, 2, &hour) || !ReadDigits(text, 14, 2, &minute)) return false;
        size_t cursor = 16;
        second = 0;
        if (cursor < text.size() && text[cursor] == ':') {
            if (!ReadDigits(text, cursor + 1, 2, &second)) return false;
            cursor += 3;
        }
        if (cursor < text.size() && text[cursor] == '.') {
            const size_t first = ++cursor;
            while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) ++cursor;
            if (cursor == first) return false;
        }
        if (cursor < text.size() && (text[cursor] == 'Z' || text[cursor] == 'z')) {
            offset = 0;
            ++cursor;
        } else if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) {
            const int sign = text[cursor] == '+' ? 1 : -1;
            int zone_hour = 0, zone_minute = 0;
            if (text.size() - cursor != 6 || text[cursor + 3] != ':' ||
                !ReadDigits(text, cursor + 1, 2, &zone_hour) ||
                !ReadDigits(text, cursor + 4, 2, &zone_minute) || zone_hour > 23 || zone_minute > 59) return false;
            offset = sign * (zone_hour * 3600 + zone_minute * 60);
            cursor += 6;
        }
        if (cursor != text.size() || hour > 23 || minute > 59 || second > 59) return false;
    }
    const int adjusted_year = year - (month <= 2);
    const int era = adjusted_year / 400;
    const unsigned yoe = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    due->instant = static_cast<std::time_t>(days * 86400 + hour * 3600 + minute * 60 + second - offset);
    const std::time_t local = due->instant + kBeijingOffset;
    if (!gmtime_r(&local, &due->local)) return false;
    due->timed = timed;
    return true;
}

bool TodoIsOverdue(const wqn::WqnTodoItem& item)
{
    if (!item.completed_at.empty() || item.status == "completed" || item.status == "done" ||
        item.status == "cancelled" || item.status == "canceled") return false;
    if (item.status == "overdue") return true;
    TodoDue due;
    const std::time_t now = CurrentUnixTime();
    return now >= kMinReasonableUnixTime && ParseTodoDue(item.due_at, &due) && due.instant < now;
}

std::string TodoBodySignature(const wqn::UiFrame& frame)
{
    const auto& todo = frame.todo;
    std::string signature;
    const auto text = [&signature](const std::string& value) {
        signature.append(std::to_string(value.size()));
        signature.push_back(':'); signature.append(value); signature.push_back('|');
    };
    const auto number = [&text](auto value) { text(std::to_string(value)); };
    text(CurrentDateLabel());
    number(static_cast<int>(todo.sync_status));
    number(todo.selected); number(todo.window_start); number(todo.total_pending);
    number(todo.loaded_once); number(todo.paging_stale);
    number(todo.has_earlier); number(todo.has_later);
    text(todo.status_message); text(todo.completion_task_id); text(todo.completion_title);
    number(frame.navigation.visible);
    if (frame.navigation.visible) number(frame.navigation.selected);
    for (const auto& item : todo.todos) {
        text(item.id); text(item.title); text(item.status); text(item.due_at);
        text(item.subject_name); text(item.completed_at);
        // The list's overdue labels/count can change without a cloud update.
        number(TodoIsOverdue(item));
    }
    return signature;
}

std::string TodoDueTimeLabel(const std::string& text)
{
    TodoDue due;
    if (!ParseTodoDue(text, &due) || !due.timed) return "--:--";
    char label[8] = {};
    std::strftime(label, sizeof(label), "%H:%M", &due.local);
    return label;
}

std::string TodoDueDateLabel(const std::string& text)
{
    TodoDue due;
    if (!ParseTodoDue(text, &due)) return "无到期日期";
    char label[32] = {};
    std::snprintf(label, sizeof(label), "%d月%d日", due.local.tm_mon + 1, due.local.tm_mday);
    return label;
}

std::string TodoStatusNote(const wqn::TodoUiState& todo)
{
    switch (todo.sync_status) {
        case wqn::TodoSyncStatus::kLoading: return "正在同步";
        case wqn::TodoSyncStatus::kReady: return todo.paging_stale ? "列表待刷新" : "已同步";
        case wqn::TodoSyncStatus::kSyncFailed: return "同步失败 · 当前缓存";
        case wqn::TodoSyncStatus::kCompleting: return "正在完成";
        case wqn::TodoSyncStatus::kCompleteFailed: return "完成失败 · 确认重试";
        case wqn::TodoSyncStatus::kCompleted: return todo.paging_stale
            ? "已完成 · 列表待刷新" : "已完成 · " + todo.completion_title;
        case wqn::TodoSyncStatus::kAuthRequired: return "未配对";
        default: return "待同步";
    }
}

void DrawTodoStatusBar(const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, kStatusBarDividerY, kScreenWidth);
    const int icons_left = DrawStatusBarIcons(kScreenWidth - kStatusBarRightInset, kStatusBarIconY, home);
    const auto* font = AiNativeFont(12);
    const std::string clock = CurrentClockLabel();
    const int width = wqn::MeasureTextWithFont(font, clock.c_str());
    AiNativeText(icons_left - 7 - width, kStatusBarTitleY, width, clock, 12);
    AiNativeText(kStatusBarTitleX, kStatusBarTitleY, icons_left - width - 25, "Todo", 12);
}

void DrawTodoSummary(const wqn::TodoUiState& todo)
{
    AiNativeText(kTodoLeft, kTodoMetaY, 144, CurrentDateLabel(), 12);
    const auto* font = AiNativeFont(12);
    const std::string note = AiNativeClip(TodoStatusNote(todo), kTodoWidth - 152, font);
    const int note_width = wqn::MeasureTextWithFont(font, note.c_str());
    AiNativeText(kTodoLeft + kTodoWidth - note_width, kTodoMetaY, note_width, note, 12);
    const bool unpaired = todo.sync_status == wqn::TodoSyncStatus::kAuthRequired;
    const bool no_pending = todo.loaded_once && todo.total_pending == 0 && todo.todos.empty() &&
        !todo.has_earlier && !todo.has_later;
    const std::string title = unpaired ? "设备未配对" : no_pending ? "暂无待办" : "待办事项";
    int title_width = kTodoWidth;
    if (!unpaired) {
        const int pending = std::max(todo.total_pending, static_cast<int>(todo.todos.size()));
        std::string number = std::to_string(pending);
        if (number.size() < 2) number.insert(0, "0");
        std::string caption = "待完成";
        // No global overdue aggregate exists on the timeline API. Show it only
        // when the loaded page contains the whole result, never as a page count.
        if (!todo.has_earlier && !todo.has_later) {
            const int overdue = static_cast<int>(std::count_if(todo.todos.begin(), todo.todos.end(), TodoIsOverdue));
            if (overdue) caption += " · 逾期 " + std::to_string(overdue);
        }
        const int number_width = wqn::MeasureTextWithFont(&wqn_ui_latin_32, number.c_str());
        const int caption_width = wqn::MeasureTextWithFont(font, caption.c_str());
        const int number_x = kTodoLeft + kTodoWidth - caption_width - 7 - number_width;
        title_width = std::max(0, number_x - kTodoLeft - 10);
        wqn::DrawTextWithFont(number_x, kTodoTitleY, &wqn_ui_latin_32, number.c_str(), true);
        AiNativeText(number_x + number_width + 7, kTodoTitleY + 15, caption_width, caption, 12);
    }
    AiNativeText(kTodoLeft, kTodoTitleY, title_width, title, 24);
    DrawHorizontalLine(kTodoLeft, kTodoDividerY, kTodoWidth);
}

void DrawTodoRow(const wqn::WqnTodoItem& item, size_t index, bool selected,
                 const wqn::TodoUiState& todo, int y)
{
    if (selected) FillRect(kTodoLeft, y, kTodoWidth, kTodoRowPitch - 1, true);
    DrawHorizontalLine(kTodoLeft, y + kTodoRowPitch - 1, kTodoWidth);
    const bool ink = !selected;
    char number[24] = {};
    std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(index + 1));
    AiNativeText(kTodoLeft + 8, y + 16, 27, number, 12, ink);
    constexpr int text_x = kTodoLeft + 37;
    constexpr int due_x = kTodoLeft + kTodoWidth - 79;
    constexpr int text_width = due_x - text_x - 8;
    AiNativeText(text_x, y + 3, text_width, item.title.empty() ? "未命名待办" : item.title, 16, ink);
    const std::string meta = (item.subject_name.empty() ? "未分类" : item.subject_name) +
        " · " + TodoDueDateLabel(item.due_at);
    AiNativeText(text_x, y + 25, text_width, meta, 12, ink);
    const bool target = !todo.completion_task_id.empty() && item.id == todo.completion_task_id;
    const bool pending = target && todo.sync_status == wqn::TodoSyncStatus::kCompleting;
    const bool failed = target && todo.sync_status == wqn::TodoSyncStatus::kCompleteFailed;
    const bool overdue = TodoIsOverdue(item);
    const std::string status = pending ? "完成中" : failed ? "重试" : overdue ? "逾期" : TodoDueTimeLabel(item.due_at);
    const auto* font = AiNativeFont(12);
    const int right = kTodoLeft + kTodoWidth - 8;
    const auto right_text = [&](int top, const std::string& text) {
        const auto clipped = AiNativeClip(text, 71, font);
        const int width = wqn::MeasureTextWithFont(font, clipped.c_str());
        AiNativeText(right - width, top, width, clipped, 12, ink);
        return width;
    };
    const bool two_lines = overdue && !pending && !failed;
    const int width = right_text(y + (two_lines ? 7 : 16), status);
    if (two_lines) {
        right_text(y + 26, TodoDueTimeLabel(item.due_at));
        if (!selected) FillRect(right - width, y + 21, width, 1, true);
    }
}

void DrawTodoScroll(const wqn::TodoUiState& todo, size_t start)
{
    constexpr int height = static_cast<int>(wqn::kTodoListVisibleRows) * kTodoRowPitch;
    const size_t count = todo.todos.size();
    if (count > wqn::kTodoListVisibleRows) {
        // One-bit dashed track, solid thumb. No grayscale asset or waveform.
        for (int y = 0; y < height; y += 4) FillRect(kTodoScrollX, kTodoListTop + y, 1, 1, true);
        const int thumb = std::max(18, height * static_cast<int>(wqn::kTodoListVisibleRows) / static_cast<int>(count));
        const int top = (height - thumb) * static_cast<int>(start) / static_cast<int>(count - wqn::kTodoListVisibleRows);
        FillRect(kTodoScrollX - 1, kTodoListTop + top, 3, thumb, true);
    }
    for (int step = 0; step < 3; ++step) {
        if (todo.has_earlier) FillRect(kTodoScrollX - step, kTodoListTop - 7 + step, 2 * step + 1, 1, true);
        if (todo.has_later) FillRect(kTodoScrollX - step, kTodoListTop + height + 6 - step, 2 * step + 1, 1, true);
    }
}

void DrawTodoEmptyState(const wqn::TodoUiState& todo)
{
    std::string title = "事情都处理好了。";
    std::string detail = "当前没有待完成的任务。";
    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired) {
        title = "请先连接账户";
        detail = "在网页端完成设备配对后同步待办。";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kLoading) {
        title = "正在同步待办";
        detail = "任务加载后会显示在这里。";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kSyncFailed) {
        title = "待办同步失败";
        detail = "请检查网络后重新进入待办。";
    } else if (todo.has_earlier || todo.has_later) {
        title = "本页已完成";
        detail = todo.has_later ? "按下键加载后面的待办。" : "按上键加载前面的待办。";
    } else if (!todo.loaded_once) {
        title = "待办尚未加载";
        detail = "连接网络后重新进入待办。";
    }
    AiNativeText(16, 151, 368, title, 24);
    DrawHorizontalLine(16, 191, 40);
    AiNativeText(16, 207, 368, detail, 12);
}
}  // namespace

void InvalidateTodoRenderCache()
{
    g_todo_body_valid = false;
}

esp_err_t RenderTodoToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const auto& todo = frame.todo;
    const std::string body_signature = TodoBodySignature(frame);
    const bool header_only =
        schedule == RefreshSchedule::kClock && g_todo_body_valid &&
        wqn::IsEpdFramebufferSynchronized() && g_last_rendered_screen == wqn::UiScreen::kTodo &&
        body_signature == g_todo_body_signature;
    if (header_only) {
        constexpr UiRect header = {0, 0, kScreenWidth, kStatusBarHeight, "todo-status-bar"};
        ClearRect(header);
        DrawTodoStatusBar(frame.home);
        const esp_err_t result = RefreshRegion(header, schedule);
        if (result != ESP_OK) g_todo_body_valid = false;
        return result;
    }
    wqn::ClearEpdFramebuffer(true);
    DrawTodoStatusBar(frame.home);
    DrawTodoSummary(todo);
    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired || todo.todos.empty()) {
        DrawTodoEmptyState(todo);
    } else {
        const size_t selected = std::min(todo.selected, todo.todos.size() - 1);
        const size_t start = wqn::ClampTodoListWindow(todo.window_start, selected, todo.todos.size());
        for (size_t row = 0; row < wqn::kTodoListVisibleRows && start + row < todo.todos.size(); ++row) {
            const size_t index = start + row;
            DrawTodoRow(todo.todos[index], index, index == selected, todo, kTodoListTop + static_cast<int>(row) * kTodoRowPitch);
        }
        DrawTodoScroll(todo, start);
    }
    const esp_err_t result = RefreshFrame(frame, schedule);
    g_todo_body_valid = result == ESP_OK;
    if (g_todo_body_valid) g_todo_body_signature = body_signature;
    return result;
}
}  // namespace device_ui_internal
