// Note page renderer: 笔记本 list -> 标题 list -> 笔记 body. The body uses a
// wrapped scroll viewport (like the AI page) with a proportional scrollbar so
// long plain_text_v1 notes are readable on the 400x300 panel.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <string>
#include <vector>

namespace device_ui_internal {

namespace {

constexpr char kTag[] = "wqn_note_page";
constexpr int kNoteMarginX = 8;
constexpr int kContentW = wqn::kEpdWidth - 2 * kNoteMarginX;
constexpr int kHeaderY = 38;
constexpr int kDividerY = 58;
constexpr int kContentTop = 66;
constexpr int kHintY = wqn::kEpdHeight - 16;
constexpr int kListRowH = 30;
constexpr int kBodyLineH = 20;
constexpr int kBodyBottom = kHintY - 6;

int VisibleListRows()
{
    return std::max(1, (kHintY - 4 - kContentTop) / kListRowH);
}

// Window start so the selected row stays visible and roughly centered.
size_t ListWindowStart(size_t selected, size_t count, size_t visible)
{
    if (count <= visible) return 0;
    size_t start = selected > visible / 2 ? selected - visible / 2 : 0;
    if (start + visible > count) start = count - visible;
    return start;
}

// A note's last-viewed cell: the stored ISO date, or 未读 when never opened.
std::string LastViewedLabel(const std::string& last_opened_at)
{
    if (last_opened_at.empty()) return "未读";
    return "看过 " + last_opened_at.substr(0, std::min<size_t>(10, last_opened_at.size()));
}

esp_err_t DrawListRow(int y, const std::string& primary, const std::string& trailing, bool selected)
{
    if (selected) {
        DrawSelectedFill(kNoteMarginX, y, kContentW, kListRowH - 4);
    }
    const bool black = !selected;
    const int trailing_w = 108;
    ESP_RETURN_ON_ERROR(
        DrawClippedText(kNoteMarginX + 10, y + 7, kContentW - trailing_w - 20, primary, black),
        kTag, "draw note row primary");
    if (!trailing.empty()) {
        ESP_RETURN_ON_ERROR(
            DrawClippedText(
                kNoteMarginX + kContentW - trailing_w, y + 7, trailing_w - 6, trailing, black),
            kTag, "draw note row trailing");
    }
    return ESP_OK;
}

esp_err_t RenderNotebookList(const wqn::NoteAppSnapshot& note)
{
    if (note.notebooks.empty()) {
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(kNoteMarginX, 150, kContentW, "暂无笔记本"), kTag, "note empty books");
        return DrawCenteredText(kNoteMarginX, 178, kContentW, "请在网页添加笔记后同步");
    }
    const size_t visible = static_cast<size_t>(VisibleListRows());
    const size_t start = ListWindowStart(note.notebook_selected, note.notebooks.size(), visible);
    for (size_t i = 0; i < visible && start + i < note.notebooks.size(); ++i) {
        const size_t index = start + i;
        const wqn::NoteNotebookRow& row = note.notebooks[index];
        const std::string count = std::to_string(row.note_count) + " 条";
        ESP_RETURN_ON_ERROR(
            DrawListRow(
                kContentTop + static_cast<int>(i) * kListRowH,
                row.title.empty() ? "(未命名笔记本)" : row.title,
                count,
                index == note.notebook_selected),
            kTag, "draw notebook row");
    }
    return ESP_OK;
}

esp_err_t RenderNoteList(const wqn::NoteAppSnapshot& note)
{
    if (note.titles.empty()) {
        return DrawCenteredText(kNoteMarginX, 160, kContentW, "该笔记本暂无笔记");
    }
    const size_t visible = static_cast<size_t>(VisibleListRows());
    const size_t start = ListWindowStart(note.note_list_selected, note.titles.size(), visible);
    for (size_t i = 0; i < visible && start + i < note.titles.size(); ++i) {
        const size_t index = start + i;
        const wqn::NoteTitleRow& row = note.titles[index];
        ESP_RETURN_ON_ERROR(
            DrawListRow(
                kContentTop + static_cast<int>(i) * kListRowH,
                row.title.empty() ? "(无标题)" : row.title,
                LastViewedLabel(row.last_opened_at),
                index == note.note_list_selected),
            kTag, "draw note title row");
    }
    return ESP_OK;
}

esp_err_t RenderNoteBody(const wqn::NoteAppSnapshot& note)
{
    if (!note.has_body) {
        return DrawCenteredText(kNoteMarginX, 160, kContentW, "内容未同步，请稍后再试");
    }
    if (!note.note_title.empty()) {
        ESP_RETURN_ON_ERROR(
            DrawClippedText(kNoteMarginX, kContentTop, kContentW, note.note_title), kTag, "note body title");
    }
    const int body_top = kContentTop + 24;
    const int visible = std::max(1, (kBodyBottom - body_top) / kBodyLineH);
    const std::vector<std::string> lines =
        wqn::WrapUtf8TextToWidth(note.note_body, kContentW - 14, 4096);
    const int total = static_cast<int>(lines.size());
    const int max_top = total > visible ? total - visible : 0;
    int top = static_cast<int>(note.note_scroll_offset_lines);
    if (top > max_top) top = max_top;
    if (top < 0) top = 0;
    for (int i = 0; i < visible && top + i < total; ++i) {
        ESP_RETURN_ON_ERROR(
            DrawClippedText(kNoteMarginX + 2, body_top + i * kBodyLineH, kContentW - 14, lines[top + i]),
            kTag, "draw note body line");
    }
    // Proportional scrollbar (font-independent) when the body overflows.
    if (total > visible) {
        const int track_x = wqn::kEpdWidth - 5;
        const int track_h = visible * kBodyLineH;
        int thumb_h = std::max(10, track_h * visible / total);
        int thumb_y = body_top + track_h * top / total;
        if (thumb_y + thumb_h > body_top + track_h) thumb_y = body_top + track_h - thumb_h;
        FillRect(track_x, thumb_y, 3, thumb_h, true);
    }
    return ESP_OK;
}

}  // namespace

esp_err_t RenderNoteToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::NoteAppSnapshot& note = frame.note_app;
    wqn::ClearEpdFramebuffer(true);
    DrawStatusBar("笔记", frame.home);

    std::string header = note.status_line;
    if ((note.mode == wqn::NoteAppMode::kNoteList ||
         note.mode == wqn::NoteAppMode::kNoteView) &&
        !note.notebook_title.empty()) {
        header = note.notebook_title;
    }
    ESP_RETURN_ON_ERROR(
        DrawClippedText(kNoteMarginX, kHeaderY, kContentW, header), kTag, "draw note header");
    DrawHorizontalLine(kNoteMarginX, kDividerY, kContentW);

    switch (note.mode) {
        case wqn::NoteAppMode::kNotebookList:
            ESP_RETURN_ON_ERROR(RenderNotebookList(note), kTag, "render notebook list");
            break;
        case wqn::NoteAppMode::kSessionStarting:
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(kNoteMarginX, 160, kContentW, "正在打开笔记本…"),
                kTag, "render session starting");
            break;
        case wqn::NoteAppMode::kNoteList:
            ESP_RETURN_ON_ERROR(RenderNoteList(note), kTag, "render note list");
            break;
        case wqn::NoteAppMode::kNoteView:
            ESP_RETURN_ON_ERROR(RenderNoteBody(note), kTag, "render note body");
            break;
    }

    if (!note.hint.empty()) {
        ESP_RETURN_ON_ERROR(
            DrawClippedText(kNoteMarginX, kHintY, kContentW, note.hint), kTag, "draw note hint");
    }
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
