// Problem browse renderer, hosted by the note page: 题目列表 -> 题目页
// (竖直环: 题图/题面/答案面/答案图) -> 自评弹窗. Text faces reuse the note
// body's wrapped scroll viewport with a proportional scrollbar; image
// segments blit the WQNI payload full-screen exactly like the note viewer.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace device_ui_internal {

namespace {

constexpr char kTag[] = "wqn_problem_page";
constexpr int kProblemMarginX = 8;
constexpr int kContentW = wqn::kEpdWidth - 2 * kProblemMarginX;
constexpr int kContentTop = 32;
// No footer line (device pages keep no bottom status row); the 13-line face
// budget must stay in sync with kProblemFaceVisibleLines in problem_app.cpp.
constexpr int kContentBottom = wqn::kEpdHeight - 4;
constexpr int kListRowH = 30;
constexpr int kBodyLineH = 20;

int VisibleListRows()
{
    return std::max(1, (kContentBottom - kContentTop) / kListRowH);
}

size_t ClampListWindowStart(size_t start, size_t selected, size_t count, size_t visible)
{
    if (visible == 0 || count <= visible) return 0;
    const size_t max_start = count - visible;
    if (start > max_start) start = max_start;
    if (selected < start) start = selected;
    else if (selected >= start + visible) start = selected + 1 - visible;
    return start;
}

esp_err_t DrawListRow(int y, const std::string& primary, const std::string& trailing, bool selected)
{
    if (selected) {
        DrawSelectedFill(kProblemMarginX, y, kContentW, kListRowH - 4);
    }
    const bool black = !selected;
    const int trailing_w = 88;
    ESP_RETURN_ON_ERROR(
        DrawClippedText(kProblemMarginX + 10, y + 7, kContentW - trailing_w - 20, primary, black),
        kTag, "draw problem row primary");
    if (!trailing.empty()) {
        ESP_RETURN_ON_ERROR(
            DrawClippedText(
                kProblemMarginX + kContentW - trailing_w, y + 7, trailing_w - 6, trailing, black),
            kTag, "draw problem row trailing");
    }
    return ESP_OK;
}

esp_err_t RenderProblemList(const wqn::ProblemAppSnapshot& problem)
{
    if (problem.rows.empty()) {
        return DrawCenteredText(kProblemMarginX, 160, kContentW, "该错题本暂无题目");
    }
    const size_t visible = static_cast<size_t>(VisibleListRows());
    const size_t start = ClampListWindowStart(
        problem.list_window_start, problem.list_selected, problem.rows.size(), visible);
    for (size_t i = 0; i < visible && start + i < problem.rows.size(); ++i) {
        const size_t index = start + i;
        const wqn::ProblemListRow& row = problem.rows[index];
        ESP_RETURN_ON_ERROR(
            DrawListRow(
                kContentTop + static_cast<int>(i) * kListRowH,
                row.title.empty() ? "(无标题)" : row.title,
                row.status_label,
                index == problem.list_selected),
            kTag, "draw problem title row");
    }
    return ESP_OK;
}

// Wrapped scroll viewport shared by the 题面 and 答案面. The wrapped lines are
// cached per problem+face so scrolling never re-wraps a multi-KB body.
esp_err_t RenderProblemTextFace(
    const wqn::ProblemAppSnapshot& problem,
    bool answer_face)
{
    const std::string& text = answer_face ? problem.answer_text : problem.body_text;
    const uint32_t scroll = answer_face
        ? problem.answer_scroll_lines
        : problem.body_scroll_lines;
    if (!problem.has_body) {
        return DrawCenteredText(kProblemMarginX, 160, kContentW, "内容未同步，请稍后再试");
    }
    if (text.empty()) {
        return DrawCenteredText(
            kProblemMarginX, 160, kContentW,
            answer_face ? "本题暂无文字答案" : "本题为图片题，向上查看题图");
    }
    const int body_top = kContentTop;
    const int visible = std::max(1, (kContentBottom - body_top) / kBodyLineH);
    static std::string s_wrapped_key;
    static std::vector<std::string> s_wrapped_lines;
    const std::string key = problem.problem_id + (answer_face ? "#a" : "#b");
    if (key != s_wrapped_key) {
        s_wrapped_lines = wqn::WrapUtf8TextToWidth(text, kContentW - 14, 4096);
        s_wrapped_key = key;
    }
    const std::vector<std::string>& lines = s_wrapped_lines;
    const int total = static_cast<int>(lines.size());
    const int max_top = total > visible ? total - visible : 0;
    int top = static_cast<int>(scroll);
    if (top > max_top) top = max_top;
    if (top < 0) top = 0;
    for (int i = 0; i < visible && top + i < total; ++i) {
        ESP_RETURN_ON_ERROR(
            DrawClippedText(
                kProblemMarginX + 2, body_top + i * kBodyLineH, kContentW - 14, lines[top + i]),
            kTag, "draw problem face line");
    }
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

esp_err_t RenderProblemImageStatus(const wqn::ProblemAppSnapshot& problem)
{
    char label[64];
    if (problem.image_error) {
        std::snprintf(label, sizeof(label), "图片加载失败");
    } else {
        std::snprintf(
            label, sizeof(label), "正在加载%s(%u/%u)…",
            problem.image_is_solution ? "答案图" : "题图",
            static_cast<unsigned>(problem.image_ordinal),
            static_cast<unsigned>(problem.image_count));
    }
    ESP_RETURN_ON_ERROR(
        DrawCenteredText(kProblemMarginX, 148, kContentW, label),
        kTag, "render problem image status");
    return DrawCenteredText(
        kProblemMarginX, 176, kContentW,
        problem.image_error ? "上下可继续浏览" : "长按可返回");
}

// 自评弹窗: full-screen dialog (mode switches always commit with a full
// refresh, so no underlay is kept -- large partial overlays ghost).
esp_err_t RenderVerdictDialog(const wqn::ProblemAppSnapshot& problem)
{
    const int box_x = 44;
    const int box_w = wqn::kEpdWidth - 2 * box_x;
    const int box_y = 56;
    const int box_h = 204;
    DrawRoundedRect(box_x, box_y, box_w, box_h, 6);
    ESP_RETURN_ON_ERROR(
        DrawCenteredText(box_x, box_y + 12, box_w, "这道题做得怎么样？"),
        kTag, "draw verdict title");
    static const char* kOptions[] = {
        "A 对了",
        "B 还要想想",
        "C 错了",
        "D 跳过",
    };
    const int row_h = 36;
    const int first_y = box_y + 48;
    for (size_t index = 0; index < 4; ++index) {
        const int y = first_y + static_cast<int>(index) * row_h;
        const bool selected = index == problem.verdict_selected;
        if (selected) {
            DrawSelectedFill(box_x + 10, y, box_w - 20, row_h - 6);
        }
        ESP_RETURN_ON_ERROR(
            DrawClippedText(box_x + 24, y + 6, box_w - 48, kOptions[index], !selected),
            kTag, "draw verdict option");
    }
    return ESP_OK;
}

}  // namespace

esp_err_t RenderProblemBrowseToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::ProblemAppSnapshot& problem = frame.problem_app;

    // Full-screen image: the WQNI payload IS the frame (no status bar, no
    // margins), so blit it straight into the framebuffer and refresh.
    if (problem.mode == wqn::ProblemAppMode::kProblemView &&
        (problem.face == wqn::ProblemFace::kProblemImage ||
         problem.face == wqn::ProblemFace::kSolutionImage) &&
        problem.image_ready && problem.image_wqni != nullptr &&
        problem.image_wqni->size() ==
            wqn::kNoteImageHeaderBytes + static_cast<size_t>(wqn::kEpdFramebufferSize)) {
        wqn::BlitEpdFramebuffer(
            problem.image_wqni->data() + wqn::kNoteImageHeaderBytes,
            static_cast<size_t>(wqn::kEpdFramebufferSize));
        return RefreshFrame(frame, schedule);
    }

    wqn::ClearEpdFramebuffer(true);

    // Status-bar title: problem title > set name > 错题.
    std::string bar_title = "错题";
    if (!problem.set_name.empty()) {
        bar_title = problem.set_name;
    }
    if ((problem.mode == wqn::ProblemAppMode::kProblemView ||
         problem.mode == wqn::ProblemAppMode::kVerdict) &&
        !problem.problem_title.empty()) {
        bar_title = problem.problem_title;
        if (problem.total > 0) {
            bar_title += " " + std::to_string(problem.position) + "/" +
                std::to_string(problem.total);
        }
    }
    const auto bar_lines = wqn::WrapUtf8TextToWidth(bar_title, wqn::kEpdWidth / 2, 1);
    DrawStatusBar(
        bar_lines.empty() ? bar_title.c_str() : bar_lines.front().c_str(), frame.home);

    switch (problem.mode) {
        case wqn::ProblemAppMode::kProblemList:
            ESP_RETURN_ON_ERROR(RenderProblemList(problem), kTag, "render problem list");
            break;
        case wqn::ProblemAppMode::kProblemView:
            if (problem.face == wqn::ProblemFace::kBody) {
                ESP_RETURN_ON_ERROR(
                    RenderProblemTextFace(problem, false), kTag, "render problem body");
            } else if (problem.face == wqn::ProblemFace::kAnswer) {
                ESP_RETURN_ON_ERROR(
                    RenderProblemTextFace(problem, true), kTag, "render problem answer");
            } else {
                // Ready images returned above; this is the loading/error page.
                ESP_RETURN_ON_ERROR(
                    RenderProblemImageStatus(problem), kTag, "render problem image page");
            }
            break;
        case wqn::ProblemAppMode::kVerdict:
            ESP_RETURN_ON_ERROR(RenderVerdictDialog(problem), kTag, "render verdict dialog");
            break;
    }

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
