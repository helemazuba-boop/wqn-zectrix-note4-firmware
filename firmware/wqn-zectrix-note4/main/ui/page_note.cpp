// Note page renderer: 笔记本 list -> 标题 list -> 笔记 body. The body uses a
// wrapped scroll viewport (like the AI page) with a proportional scrollbar so
// long plain_text_v1 notes are readable on the 400x300 panel.

#include "ui_internal.h"
#include "ui_widgets.h"

#include "markdown_layout.h"
#include "ai_native_text.h"
#include "note_reading_geometry.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace device_ui_internal {

namespace {

constexpr char kTag[] = "wqn_note_page";
constexpr int kNoteMarginX = kMarginDense;  // [v2] density-page margin (was 8)
constexpr int kContentW = wqn::kEpdWidth - 2 * kNoteMarginX;

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

void DrawNoteListHeading(const std::string& context, const std::string& title,
                         const std::string& position, const std::string& notice)
{
    const int count_width = wqn::MeasureTextWithFont(AiNativeFont(12), position.c_str());
    AiNativeText(kNoteListX, kNoteListKickerY, kNoteListWidth - count_width - 8, context, 12);
    AiNativeText(kNoteListX + kNoteListWidth - count_width, kNoteListKickerY, count_width, position, 12);
    AiNativeText(kNoteListX, kNoteListTitleY, kNoteListWidth,
                 notice.empty() ? title : notice, notice.empty() ? 24 : 16);
    DrawHorizontalLine(kNoteListX, kNoteListDividerY, kNoteListWidth);
}

void DrawNoteListRow(int y, size_t index, const std::string& primary,
                     const std::string& secondary, bool selected)
{
    if (selected) FillRect(kNoteListX, y, kNoteListWidth, kNoteListPitch - 1, true);
    DrawHorizontalLine(kNoteListX, y + kNoteListPitch - 1, kNoteListWidth);
    char number[24] = {};
    std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(index + 1));
    AiNativeText(kNoteListX + 7, y + 14, 27, number, 12, !selected);
    AiNativeText(kNoteListX + 38, y + 4, kNoteListWidth - 61, primary, 16, !selected);
    AiNativeText(kNoteListX + 38, y + 25, kNoteListWidth - 61, secondary, 12, !selected);
    const int arrow_x = kNoteListX + kNoteListWidth - 13;
    for (int step = 0; step < 5; ++step) {
        FillRect(arrow_x + step, y + 16 + step, 1, 1, !selected);
        FillRect(arrow_x + step, y + 24 - step, 1, 1, !selected);
    }
}

void DrawNoteEmpty(const std::string& title, const std::string& detail)
{
    AiNativeText(16, 147, 368, title, 20);
    AiNativeText(16, 179, 368, detail, 12);
}

esp_err_t RenderNotebookList(const wqn::NoteAppSnapshot& note)
{
    const size_t count = note.notebooks.size();
    const size_t selected = count ? std::min(note.notebook_selected, count - 1) : 0;
    DrawNoteListHeading("笔记本 / 错题本 / 词库", "笔记本",
        count ? std::to_string(selected + 1) + " / " + std::to_string(count) : "0", note.notice);
    if (!count) {
        DrawNoteEmpty("暂无笔记本", "请在网页添加笔记后同步");
        return ESP_OK;
    }
    const size_t start = wqn::ClampNoteListWindow(note.notebook_window_start, selected, count);
    for (size_t i = 0; i < wqn::kNoteListVisibleRows && start + i < count; ++i) {
        const size_t index = start + i;
        const auto& row = note.notebooks[index];
        const char* unit = row.kind == wqn::NoteLibraryRowKind::kProblemSet ? " 道错题"
            : row.kind == wqn::NoteLibraryRowKind::kWordDeck ? " 个单词" : " 条笔记";
        std::string detail = std::to_string(row.note_count) + unit;
        if (!row.has_pack) detail += " / 未同步";
        DrawNoteListRow(kNoteListTop + static_cast<int>(i) * kNoteListPitch, index,
            row.title.empty() ? "(未命名)" : row.title, detail, index == selected);
    }
    return ESP_OK;
}

esp_err_t RenderNoteList(const wqn::NoteAppSnapshot& note)
{
    const size_t count = note.titles.size();
    const size_t selected = count ? std::min(note.note_list_selected, count - 1) : 0;
    const std::string summary = (note.titles_have_more ? "已加载 " : "") + std::to_string(count) + " 条笔记";
    DrawNoteListHeading(summary, note.notebook_title.empty() ? "笔记" : note.notebook_title,
        count ? std::to_string(selected + 1) + " / " + std::to_string(count) + (note.titles_have_more ? "+" : "") : "0", note.notice);
    if (!count) {
        DrawNoteEmpty(note.titles_have_more ? "正在加载笔记..." : "该笔记本暂无笔记",
            note.titles_have_more ? "已加载的标题会显示在这里。" : "在网页添加笔记后同步到设备。");
        return ESP_OK;
    }
    const size_t start = wqn::ClampNoteListWindow(note.note_list_window_start, selected, count);
    for (size_t i = 0; i < wqn::kNoteListVisibleRows && start + i < count; ++i) {
        const size_t index = start + i;
        const auto& row = note.titles[index];
        DrawNoteListRow(kNoteListTop + static_cast<int>(i) * kNoteListPitch, index,
            row.title.empty() ? "(无标题)" : row.title,
            row.content_available ? LastViewedLabel(row.last_opened_at) : "内容未同步", index == selected);
    }
    return ESP_OK;
}

void DrawNoteStatusBar(const wqn::UiFrame& frame)
{
    DrawHorizontalLine(0, kStatusBarDividerY, kScreenWidth);
    const int icons_left = DrawStatusBarIcons(kScreenWidth - kStatusBarRightInset, kStatusBarIconY, frame.home);
    const std::string clock = CurrentClockLabel();
    const int clock_width = wqn::MeasureTextWithFont(AiNativeFont(12), clock.c_str());
    const int clock_x = icons_left - 7 - clock_width;
    AiNativeText(clock_x, kStatusBarTitleY, clock_width, clock, 12);
    const auto& note = frame.note_app;
    const bool reading = note.mode == wqn::NoteAppMode::kNoteView || note.mode == wqn::NoteAppMode::kNoteImageView;
    AiNativeText(kStatusBarTitleX, kStatusBarTitleY, clock_x - kStatusBarTitleX - 8,
        reading && !note.note_title.empty() ? note.note_title : "WQN / 笔记", reading ? 16 : 12);
}

// [transfer-progress] Segmented download bar (5 cells, quantized upstream in
// note_app). Style mirrors the settings volume bar: 1px outline + inner fill.
// bucket <0 hides the bar entirely (cache hits, request not on the wire yet).
void DrawTransferProgressBar(int y, int8_t bucket)
{
    if (bucket < 0) {
        return;
    }
    constexpr int kBarWidth = 240;
    constexpr int kBarHeight = 10;
    const int x = kNoteMarginX + (kContentW - kBarWidth) / 2;
    DrawRect(x, y, kBarWidth, kBarHeight);
    const int clamped = bucket > 5 ? 5 : bucket;
    const int filled = (kBarWidth - 2) * clamped / 5;
    if (filled > 0) {
        FillRect(x + 1, y + 1, filled, kBarHeight - 2, true);
    }
}

esp_err_t RenderNoteBody(const wqn::NoteAppSnapshot& note)
{
    if (!note.has_body || !note.body_layout) {
        DrawNoteEmpty(note.body_fetch_active ? "正在同步内容..."
            : note.body_fetch_failed ? "内容同步失败" : "内容未同步",
            note.body_fetch_active ? "请稍候，长按确认可返回。" : "确认重试，长按返回列表。");
        if (note.body_fetch_active) DrawTransferProgressBar(208, note.transfer_progress_bucket);
        return ESP_OK;
    }
    const auto& lines = note.body_layout->markdown;
    const auto geometry = ComputeNoteReadingGeometry(lines.size(), note.note_image_count > 0, !note.notice.empty());
    int entry_y = kNoteBodyTop;
    if (note.note_image_count > 0) {
        AiNativeText(kNoteBodyX, entry_y, kNoteBodyWidth,
            "↑ 图片 " + std::to_string(note.note_image_count) + " 张", 12);
        DrawHorizontalLine(kNoteBodyX, entry_y + kNoteBodyLineHeight - 1, kNoteBodyWidth);
        entry_y += kNoteBodyLineHeight;
    }
    if (!note.notice.empty()) AiNativeText(kNoteBodyX, entry_y, kNoteBodyWidth, note.notice, 12);
    if (lines.empty()) {
        DrawNoteEmpty("这篇笔记暂无正文", note.note_image_count ? "可从上方入口查看图片。" : "在网页补充内容后同步。");
        return ESP_OK;
    }
    const uint32_t top = std::min(note.note_scroll_offset_lines, geometry.max_scroll);
    for (uint32_t i = 0; i < geometry.visible && top + i < lines.size(); ++i) {
        ESP_RETURN_ON_ERROR(DrawMarkdownLine(lines[top + i], kNoteBodyX,
            geometry.top + static_cast<int>(i) * kNoteBodyLineHeight,
            kNoteBodyWidth, kNoteBodyLineHeight, AiNativeFont()), kTag, "draw note body line");
    }
    if (lines.size() > geometry.visible) {
        const int track_h = static_cast<int>(geometry.visible) * kNoteBodyLineHeight;
        const int thumb_h = std::max(10, track_h * static_cast<int>(geometry.visible) / static_cast<int>(lines.size()));
        const int travel = track_h - thumb_h;
        const int thumb_y = geometry.top + static_cast<int>(top * travel / geometry.max_scroll);
        FillRect(wqn::kEpdWidth - 5, thumb_y, 3, thumb_h, true);
    }
    return ESP_OK;
}

void RenderNoteImagePlaceholder(const wqn::NoteAppSnapshot& note)
{
    DrawNoteEmpty(note.note_image_error ? "图片加载失败" : "正在加载图片...",
        note.note_image_error ? "确认重试，长按返回正文" : "长按返回正文");
    if (!note.note_image_error) {
        DrawTransferProgressBar(208, note.transfer_progress_bucket);
    }
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
    // [status-bar-progress] A flip to a not-yet-downloaded image keeps the
    // PREVIOUS image on screen (payload carried stale) with a slim top
    // banner tracking the download, instead of a blocking loading page.
    if (note.mode == wqn::NoteAppMode::kNoteImageView && !note.note_image_error &&
        note.note_image_wqni != nullptr &&
        note.note_image_wqni->size() >= wqn::kNoteImageHeaderBytes &&
        note.note_image_wqni->data()[5] == 2 &&
        note.note_image_wqni->size() ==
            wqn::kNoteImageHeaderBytes + wqn::kNoteImageGray4PayloadBytes) {
        // A gray download may expose the previous payload while progress is
        // being reported. Re-running four waveform passes for every progress
        // bucket would flash the panel and waste minutes of battery; keep the
        // previous physical image until the variant is complete.
        if (!note.note_image_ready && !frame.navigation.visible &&
            !g_last_rendered_navigation_visible) {
            return ESP_OK;
        }
        return RefreshNavigationGrayImage(
            frame, note.note_image_wqni->data() + wqn::kNoteImageHeaderBytes,
            wqn::kNoteImageGray4PayloadBytes);
    }
    if (note.mode == wqn::NoteAppMode::kNoteImageView && !note.note_image_error &&
        note.note_image_wqni != nullptr &&
        note.note_image_wqni->size() ==
            wqn::kNoteImageHeaderBytes + static_cast<size_t>(wqn::kEpdFramebufferSize)) {
        wqn::BlitEpdFramebuffer(
            note.note_image_wqni->data() + wqn::kNoteImageHeaderBytes,
            static_cast<size_t>(wqn::kEpdFramebufferSize));
        if (!note.note_image_ready) {
            // White strip + segmented bar over the top edge of the stale
            // image; the completed download repaints the full frame anyway.
            FillRect(0, 0, wqn::kEpdWidth, 18, false);
            FillRect(0, 18, wqn::kEpdWidth, 1, true);
            DrawTransferProgressBar(4, note.transfer_progress_bucket >= 0
                                           ? note.transfer_progress_bucket
                                           : static_cast<int8_t>(0));
        }
        return RefreshFrame(frame, schedule);
    }

    wqn::ClearEpdFramebuffer(true);

    DrawNoteStatusBar(frame);
    // [status-bar-progress] Download progress lives IN the status bar (the
    // 4px slack under the text, above the separator at y=27): the user keeps
    // reading/browsing the content below while the strip fills up.
    if (note.transfer_progress_bucket >= 0 &&
        note.mode != wqn::NoteAppMode::kNoteImageView) {
        const int clamped = note.transfer_progress_bucket > 5
                                ? 5
                                : note.transfer_progress_bucket;
        // Floor of 12px: the empty-bar phase (request armed, no bytes yet)
        // must still be visible as the button acknowledgement.
        const int filled = std::max(wqn::kEpdWidth * clamped / 5, 12);
        FillRect(0, 23, filled, 3, true);
    }

    switch (note.mode) {
        case wqn::NoteAppMode::kNotebookList:
            ESP_RETURN_ON_ERROR(RenderNotebookList(note), kTag, "render notebook list");
            break;
        case wqn::NoteAppMode::kSessionStarting:
            DrawNoteEmpty("正在打开笔记本...", "请稍候，长按确认可取消。");
            break;
        case wqn::NoteAppMode::kNoteList:
            ESP_RETURN_ON_ERROR(RenderNoteList(note), kTag, "render note list");
            break;
        case wqn::NoteAppMode::kNoteView:
            ESP_RETURN_ON_ERROR(RenderNoteBody(note), kTag, "render note body");
            break;
        case wqn::NoteAppMode::kNoteImageView:
            // Ready images returned above; this is the loading / error page.
            RenderNoteImagePlaceholder(note);
            break;
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
