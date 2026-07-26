// Note page renderer: 笔记本 list -> 标题 list -> 笔记 body. The body uses a
// wrapped scroll viewport (like the AI page) with a proportional scrollbar so
// long plain_text_v1 notes are readable on the 400x300 panel.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace device_ui_internal {

namespace {

constexpr char kTag[] = "wqn_note_page";
constexpr int kNoteMarginX = 8;
constexpr int kContentW = wqn::kEpdWidth - 2 * kNoteMarginX;
constexpr int kContentTop = 32;
// No footer line: lists and the body run down to the panel edge. The body
// line budget ((kContentBottom - kContentTop) / kBodyLineH = 13) must stay in
// sync with kNoteBodyVisibleLines in note_app.cpp.
constexpr int kContentBottom = wqn::kEpdHeight - 4;
constexpr int kListRowH = 30;
constexpr int kBodyLineH = 20;
constexpr int kBodyBottom = kContentBottom;

int VisibleListRows()
{
    return std::max(1, (kContentBottom - kContentTop) / kListRowH);
}

// Viewport start comes from app state (edge-triggered; see note_app.cpp
// UpdateListViewport). Clamp defensively so the selected row is always
// on-screen even if counts changed after the viewport was last updated.
size_t ClampListWindowStart(size_t start, size_t selected, size_t count, size_t visible)
{
    if (visible == 0 || count <= visible) return 0;
    const size_t max_start = count - visible;
    if (start > max_start) start = max_start;
    if (selected < start) start = selected;
    else if (selected >= start + visible) start = selected + 1 - visible;
    return start;
}

// Converts a UTC ISO-8601 instant ("...Z" or "...+00:00") to the Beijing
// (UTC+8) calendar date. The pin stays UTC in storage and on the wire (the
// server orders by it), so the calendar-day shift belongs to the display
// layer -- otherwise a note read after midnight Beijing shows yesterday's
// date. Uses the days-from-civil algorithm instead of relying on newlib
// shipping timegm().
std::string BeijingDateFromUtcIso(const std::string& iso)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(
            iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6 ||
        month < 1 || month > 12 || day < 1 || day > 31) {
        return std::string();
    }
    const int adjusted_year = year - (month <= 2 ? 1 : 0);
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2u) / 5u +
        static_cast<unsigned>(day) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int64_t days =
        static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    const std::time_t epoch = static_cast<std::time_t>(
        days * 86400 + hour * 3600 + minute * 60 + second + 8 * 3600);
    std::tm beijing = {};
    gmtime_r(&epoch, &beijing);
    char buffer[12] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &beijing) == 0) {
        return std::string();
    }
    return buffer;
}

// A note's last-viewed cell: the pinned date in Beijing time, or 未读 when
// never opened.
std::string LastViewedLabel(const std::string& last_opened_at)
{
    if (last_opened_at.empty()) return "未读";
    const std::string beijing_date = BeijingDateFromUtcIso(last_opened_at);
    if (!beijing_date.empty()) return "看过 " + beijing_date;
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
    const size_t start = ClampListWindowStart(
        note.notebook_window_start, note.notebook_selected, note.notebooks.size(), visible);
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
    const size_t start = ClampListWindowStart(
        note.note_list_window_start, note.note_list_selected, note.titles.size(), visible);
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
    // The note title lives in the status bar, so the body uses full height.
    // Notes with images dedicate the first row to the image entry (按上进入
    // 全屏图片)；the body clamp in note_app.cpp mirrors the reduced line budget.
    int body_top = kContentTop;
    if (note.note_image_count > 0) {
        char entry[64];
        std::snprintf(
            entry, sizeof(entry), "↑ 查看图片(%u)",
            static_cast<unsigned>(note.note_image_count));
        ESP_RETURN_ON_ERROR(
            DrawClippedText(kNoteMarginX + 2, body_top, kContentW - 14, entry),
            kTag, "draw note image entry");
        body_top += kBodyLineH;
    }
    const int visible = std::max(1, (kBodyBottom - body_top) / kBodyLineH);
    // Wrap the body once per opened note. Re-running WrapUtf8TextToWidth over a
    // body of up to 16 KB on every frame pegged the CPU while scrolling; cache the
    // wrapped lines keyed by note_id so scrolling only re-reads them.
    static std::string s_wrapped_note_id;
    static std::vector<std::string> s_wrapped_lines;
    if (note.note_id != s_wrapped_note_id) {
        s_wrapped_lines = wqn::WrapUtf8TextToWidth(note.note_body, kContentW - 14, 4096);
        s_wrapped_note_id = note.note_id;
    }
    const std::vector<std::string>& lines = s_wrapped_lines;
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
    // The problem layer ([题] rows) owns the whole screen while active.
    if (frame.problem_app.active) {
        return RenderProblemBrowseToEpd(frame, schedule);
    }

    const wqn::NoteAppSnapshot& note = frame.note_app;

    // Full-screen image: the WQNI payload IS the frame (no status bar, no
    // margins), so blit it straight into the framebuffer and refresh.
    if (note.mode == wqn::NoteAppMode::kNoteImageView && note.note_image_ready &&
        note.note_image_wqni != nullptr &&
        note.note_image_wqni->size() ==
            wqn::kNoteImageHeaderBytes + static_cast<size_t>(wqn::kEpdFramebufferSize)) {
        wqn::BlitEpdFramebuffer(
            note.note_image_wqni->data() + wqn::kNoteImageHeaderBytes,
            static_cast<size_t>(wqn::kEpdFramebufferSize));
        return RefreshFrame(frame, schedule);
    }

    wqn::ClearEpdFramebuffer(true);

    // The status-bar title carries the page context (priority: note title >
    // notebook name > "笔记"), so there is no separate header row. Truncate to the
    // left half of the bar -- DrawStatusBar draws the clock/battery on the right
    // without clipping the title, so an untrimmed title would overrun them.
    std::string bar_title = "笔记";
    if ((note.mode == wqn::NoteAppMode::kNoteList ||
         note.mode == wqn::NoteAppMode::kNoteView ||
         note.mode == wqn::NoteAppMode::kNoteImageView) &&
        !note.notebook_title.empty()) {
        bar_title = note.notebook_title;
    }
    if ((note.mode == wqn::NoteAppMode::kNoteView ||
         note.mode == wqn::NoteAppMode::kNoteImageView) &&
        !note.note_title.empty()) {
        bar_title = note.note_title;
    }
    const auto bar_lines = wqn::WrapUtf8TextToWidth(bar_title, wqn::kEpdWidth / 2, 1);
    DrawStatusBar(
        bar_lines.empty() ? bar_title.c_str() : bar_lines.front().c_str(), frame.home);

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
        case wqn::NoteAppMode::kNoteImageView:
            // Ready images returned above; this is the loading / error page.
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(
                    kNoteMarginX, 148, kContentW,
                    note.note_image_error ? "图片加载失败" : "正在加载图片…"),
                kTag, "render note image status");
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(
                    kNoteMarginX, 176, kContentW,
                    note.note_image_error ? "长按返回正文" : "长按可取消"),
                kTag, "render note image status hint");
            break;
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
