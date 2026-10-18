// Native problem review: title list, independently cached text faces and
// self-assessment. Existing attachment transport and ring semantics are kept.
#include "ui_internal.h"
#include "ui_widgets.h"
#include "ai_native_text.h"
#include "note_reading_geometry.h"
#include "note_list_model.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace device_ui_internal {
namespace {
constexpr char kTag[] = "wqn_problem_page";

void DrawProblemEmpty(const std::string& title, const std::string& detail)
{
    AiNativeText(16, 147, 368, title, 20);
    AiNativeText(16, 179, 368, detail, 12);
}

void DrawProblemStatusBar(const wqn::UiFrame& frame)
{
    DrawHorizontalLine(0, kStatusBarDividerY, kScreenWidth);
    const int icons_left = DrawStatusBarIcons(kScreenWidth - kStatusBarRightInset,
        kStatusBarIconY, frame.home);
    const std::string clock = CurrentClockLabel();
    const int clock_width = wqn::MeasureTextWithFont(AiNativeFont(12), clock.c_str());
    const int clock_x = icons_left - 7 - clock_width;
    AiNativeText(clock_x, kStatusBarTitleY, clock_width, clock, 12);
    const auto& problem = frame.problem_app;
    const bool reading = problem.mode == wqn::ProblemAppMode::kProblemView;
    std::string title = "WQN / 错题";
    if (reading) {
        const char* face = problem.face == wqn::ProblemFace::kBody ? "题面"
            : problem.face == wqn::ProblemFace::kAnswer ? "答案"
            : problem.face == wqn::ProblemFace::kProblemImage ? "题图" : "答案图";
        title = face;
        if (problem.total) title += " " + std::to_string(problem.position) + "/" + std::to_string(problem.total);
        if (!problem.problem_title.empty()) title += " / " + problem.problem_title;
    }
    AiNativeText(kStatusBarTitleX, kStatusBarTitleY,
        clock_x - kStatusBarTitleX - 8, title, reading ? 16 : 12);
}

void DrawProblemHeading(const std::string& context, const std::string& title,
                        const std::string& position, const std::string& notice)
{
    const int count_width = wqn::MeasureTextWithFont(AiNativeFont(12), position.c_str());
    AiNativeText(kNoteListX, kNoteListKickerY, kNoteListWidth - count_width - 8, context, 12);
    AiNativeText(kNoteListX + kNoteListWidth - count_width, kNoteListKickerY, count_width, position, 12);
    AiNativeText(kNoteListX, kNoteListTitleY, kNoteListWidth,
        notice.empty() ? title : notice, notice.empty() ? 24 : 16);
    DrawHorizontalLine(kNoteListX, kNoteListDividerY, kNoteListWidth);
}

void DrawProblemTitleRow(int y, size_t index, const wqn::ProblemListRow& row, bool selected)
{
    if (selected) FillRect(kNoteListX, y, kNoteListWidth, kNoteListPitch - 1, true);
    DrawHorizontalLine(kNoteListX, y + kNoteListPitch - 1, kNoteListWidth);
    char number[24] = {};
    std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(index + 1));
    AiNativeText(kNoteListX + 7, y + 14, 27, number, 12, !selected);
    AiNativeText(kNoteListX + 38, y + 4, kNoteListWidth - 61,
        row.title.empty() ? "(无标题)" : row.title, 16, !selected);
    AiNativeText(kNoteListX + 38, y + 25, kNoteListWidth - 61, row.status_label, 12, !selected);
    const int arrow_x = kNoteListX + kNoteListWidth - 13;
    for (int step = 0; step < 5; ++step) {
        FillRect(arrow_x + step, y + 16 + step, 1, 1, !selected);
        FillRect(arrow_x + step, y + 24 - step, 1, 1, !selected);
    }
}

esp_err_t RenderProblemList(const wqn::ProblemAppSnapshot& problem)
{
    const size_t count = problem.rows.size();
    const size_t selected = count ? std::min(problem.list_selected, count - 1) : 0;
    DrawProblemHeading(std::to_string(count) + " 道错题",
        problem.set_name.empty() ? "错题本" : problem.set_name,
        count ? std::to_string(selected + 1) + " / " + std::to_string(count) : "0", problem.notice);
    if (!count) {
        DrawProblemEmpty("该错题本暂无题目", "同步错题本后可在设备复习");
        return ESP_OK;
    }
    const size_t start = wqn::ClampNoteListWindow(problem.list_window_start, selected, count);
    for (size_t i = 0; i < wqn::kNoteListVisibleRows && start + i < count; ++i) {
        const size_t index = start + i;
        DrawProblemTitleRow(kNoteListTop + static_cast<int>(i) * kNoteListPitch,
            index, problem.rows[index], index == selected);
    }
    return ESP_OK;
}

esp_err_t RenderProblemTextFace(const wqn::ProblemAppSnapshot& problem, bool answer_face)
{
    if (!problem.has_body) {
        DrawProblemEmpty(problem.notice.empty() ? "内容未同步" : problem.notice,
            "请同步错题本后重试");
        return ESP_OK;
    }
    const auto& layout = answer_face ? problem.answer_layout : problem.body_layout;
    const auto geometry = ComputeNoteReadingGeometry(layout ? layout->markdown.size() : 0,
        false, !problem.notice.empty());
    if (!problem.notice.empty()) AiNativeText(kNoteBodyX, kNoteBodyTop,
        kNoteBodyWidth, problem.notice, 12);
    if (!layout || layout->markdown.empty() ||
        (layout->markdown.size() == 1 && layout->markdown[0].text.empty())) {
        DrawProblemEmpty(answer_face ? "本题暂无文字答案" : "本题暂无文字题面",
            problem.answer_unlocked ? "确认可打开自评" : "确认可查看答案");
        return ESP_OK;
    }
    const auto& lines = layout->markdown;
    const uint32_t offset = std::min(answer_face ? problem.answer_scroll_lines : problem.body_scroll_lines,
        geometry.max_scroll);
    for (uint32_t i = 0; i < geometry.visible && offset + i < lines.size(); ++i) {
        ESP_RETURN_ON_ERROR(DrawMarkdownLine(lines[offset + i], kNoteBodyX,
            geometry.top + static_cast<int>(i) * kNoteBodyLineHeight,
            kNoteBodyWidth, kNoteBodyLineHeight, AiNativeFont()), kTag, "draw problem text");
    }
    if (geometry.max_scroll) {
        const int track_h = static_cast<int>(geometry.visible) * kNoteBodyLineHeight;
        const int thumb_h = std::max(10, track_h * static_cast<int>(geometry.visible) / static_cast<int>(lines.size()));
        const int thumb_y = geometry.top + static_cast<int>(
            (static_cast<uint64_t>(track_h - thumb_h) * offset) / geometry.max_scroll);
        FillRect(kScreenWidth - 5, thumb_y, 3, thumb_h, true);
    }
    return ESP_OK;
}

esp_err_t RenderProblemImageStatus(const wqn::ProblemAppSnapshot& problem)
{
    char label[80] = {};
    std::snprintf(label, sizeof(label), "正在加载%s (%u/%u)...",
        problem.image_is_solution ? "答案图" : "题图",
        static_cast<unsigned>(problem.image_ordinal), static_cast<unsigned>(problem.image_count));
    DrawProblemEmpty(problem.image_error ? "图片加载失败" : label,
        problem.image_error ? "上下可继续浏览" : "长按返回题目列表");
    return ESP_OK;
}

esp_err_t RenderVerdictDialog(const wqn::ProblemAppSnapshot& problem)
{
    const std::string position = problem.total
        ? std::to_string(problem.position) + " / " + std::to_string(problem.total) : "";
    const int count_width = wqn::MeasureTextWithFont(AiNativeFont(12), position.c_str());
    AiNativeText(kNoteListX, kNoteListKickerY, kNoteListWidth - count_width - 8,
        problem.set_name.empty() ? "错题复习" : problem.set_name, 12);
    AiNativeText(kNoteListX + kNoteListWidth - count_width, kNoteListKickerY, count_width, position, 12);
    AiNativeText(kNoteListX, kNoteListTitleY, kNoteListWidth, "本题自评", 24);
    AiNativeText(kNoteListX, kProblemVerdictSubtitleY, kNoteListWidth,
        problem.notice.empty() ? problem.problem_title : problem.notice,
        problem.notice.empty() ? 16 : 12);
    DrawHorizontalLine(kNoteListX, kProblemVerdictDividerY, kNoteListWidth);
    static const char* options[] = {"A  对了", "B  还要想想", "C  错了", "D  跳过"};
    for (size_t i = 0; i < 4; ++i) {
        const int y = kProblemVerdictTop + static_cast<int>(i) * kProblemVerdictPitch;
        const bool selected = i == problem.verdict_selected;
        if (selected) FillRect(kNoteListX, y, kNoteListWidth, kProblemVerdictPitch - 1, true);
        DrawHorizontalLine(kNoteListX, y + kProblemVerdictPitch - 1, kNoteListWidth);
        AiNativeText(kNoteListX + 12, y + 11, kNoteListWidth - 24, options[i], 16, !selected);
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
        problem.image_wqni->size() >= wqn::kNoteImageHeaderBytes &&
        problem.image_wqni->data()[5] == 2 &&
        problem.image_wqni->size() ==
            wqn::kNoteImageHeaderBytes + wqn::kNoteImageGray4PayloadBytes) {
        return RefreshNavigationGrayImage(
            frame, problem.image_wqni->data() + wqn::kNoteImageHeaderBytes,
            wqn::kNoteImageGray4PayloadBytes);
    }
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
    DrawProblemStatusBar(frame);
    switch (problem.mode) {
        case wqn::ProblemAppMode::kProblemList:
            ESP_RETURN_ON_ERROR(RenderProblemList(problem), kTag, "render problem list");
            break;
        case wqn::ProblemAppMode::kProblemView:
            if (problem.face == wqn::ProblemFace::kBody || problem.face == wqn::ProblemFace::kAnswer) {
                ESP_RETURN_ON_ERROR(RenderProblemTextFace(problem,
                    problem.face == wqn::ProblemFace::kAnswer), kTag, "render problem text face");
            } else {
                ESP_RETURN_ON_ERROR(RenderProblemImageStatus(problem), kTag, "render problem image status");
            }
            break;
        case wqn::ProblemAppMode::kVerdict:
            ESP_RETURN_ON_ERROR(RenderVerdictDialog(problem), kTag, "render problem verdict");
            break;
    }
    return RefreshFrame(frame, schedule);
}
}  // namespace device_ui_internal
