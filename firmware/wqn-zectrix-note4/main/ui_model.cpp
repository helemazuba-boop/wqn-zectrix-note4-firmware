#include "ui_model.h"

#include <algorithm>
#include <cstdint>

#include "ai_session.h"
#include "epd_display.h"
#include "flash_session.h"
#include "provision_manager.h"

namespace {

constexpr size_t kPreviewChars = 96;
constexpr int64_t kAiMockReplyDelayMs = 1300;

bool DecodeUtf8(const char*& cursor, uint32_t* codepoint)
{
    if (cursor == nullptr || codepoint == nullptr || *cursor == '\0') {
        return false;
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(cursor);
    if (p[0] < 0x80) {
        *codepoint = p[0];
        cursor += 1;
        return true;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *codepoint = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        cursor += 2;
        return true;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *codepoint = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        cursor += 3;
        return true;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *codepoint = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        cursor += 4;
        return true;
    }

    *codepoint = '?';
    cursor += 1;
    return true;
}

std::string Truncate(const std::string& text, size_t limit)
{
    const char* cursor = text.c_str();
    size_t count = 0;
    while (*cursor != '\0') {
        uint32_t codepoint = 0;
        const char* before = cursor;
        if (!DecodeUtf8(cursor, &codepoint) || cursor == before) {
            break;
        }
        if (++count > limit) {
            break;
        }
    }

    if (count <= limit && *cursor == '\0') {
        return text;
    }
    if (limit <= 3) {
        return "...";
    }

    std::string output;
    cursor = text.c_str();
    count = 0;
    while (*cursor != '\0' && count < limit - 3) {
        const char* before = cursor;
        uint32_t codepoint = 0;
        if (!DecodeUtf8(cursor, &codepoint) || cursor == before) {
            break;
        }
        output.append(before, static_cast<size_t>(cursor - before));
        ++count;
    }
    output.append("...");
    return output;
}


std::string AiPagedTextSource(const wqn::AiSessionState& ai)
{
    if (!ai.assistant_text.empty()) {
        return ai.assistant_text;
    }
    if (!ai.pending_text.empty()) {
        return ai.pending_text;
    }
    if (ai.status == wqn::AiSessionStatus::kListening) {
        return "正在录音，松手后上传识别。";
    }
    if (ai.status == wqn::AiSessionStatus::kWaitingReply) {
        return "正在上传并等待模型回复...";
    }
    if (ai.status == wqn::AiSessionStatus::kError) {
        return "请求失败，请长按确认重试。";
    }
    return "我会在这里显示转写和回答。";
}

const char* ScreenName(wqn::UiScreen screen)
{
    switch (screen) {
        case wqn::UiScreen::kAi:
            return "AI";
        case wqn::UiScreen::kTodo:
            return "Todo";
        case wqn::UiScreen::kSettings:
            return "设置";
        case wqn::UiScreen::kHome:
            return "首页";
        case wqn::UiScreen::kTime:
            return "时间";
        case wqn::UiScreen::kWord:
            return "单词";
        case wqn::UiScreen::kLibrary:
            return "题库";
        case wqn::UiScreen::kProblem:
            return "题目";
        case wqn::UiScreen::kSolution:
            return "解析";
        case wqn::UiScreen::kReviewQueue:
            return "上传队列";
        case wqn::UiScreen::kReviewScore:
            return "复习反馈";
        case wqn::UiScreen::kReviewQueued:
            return "已记录";
        case wqn::UiScreen::kProvisioning:
            return "配网";
    }
    return "WQN";
}

wqn::UiScreen PreviousTopScreen(wqn::UiScreen screen)
{
    switch (screen) {
        case wqn::UiScreen::kAi:
            return wqn::UiScreen::kTodo;
        case wqn::UiScreen::kSettings:
            return wqn::UiScreen::kAi;
        case wqn::UiScreen::kLibrary:
        case wqn::UiScreen::kProblem:
        case wqn::UiScreen::kSolution:
        case wqn::UiScreen::kReviewQueue:
        case wqn::UiScreen::kReviewScore:
        case wqn::UiScreen::kReviewQueued:
            return wqn::UiScreen::kAi;
        case wqn::UiScreen::kHome:
            return wqn::UiScreen::kSettings;
        case wqn::UiScreen::kTime:
            return wqn::UiScreen::kHome;
        case wqn::UiScreen::kWord:
            return wqn::UiScreen::kTime;
        case wqn::UiScreen::kTodo:
            return wqn::UiScreen::kWord;
        case wqn::UiScreen::kProvisioning:
            return wqn::UiScreen::kHome;
    }
    return wqn::UiScreen::kHome;
}

wqn::UiScreen NextTopScreen(wqn::UiScreen screen)
{
    switch (screen) {
        case wqn::UiScreen::kAi:
            return wqn::UiScreen::kSettings;
        case wqn::UiScreen::kSettings:
            return wqn::UiScreen::kHome;
        case wqn::UiScreen::kLibrary:
        case wqn::UiScreen::kProblem:
        case wqn::UiScreen::kSolution:
        case wqn::UiScreen::kReviewQueue:
        case wqn::UiScreen::kReviewScore:
        case wqn::UiScreen::kReviewQueued:
            return wqn::UiScreen::kHome;
        case wqn::UiScreen::kHome:
            return wqn::UiScreen::kTime;
        case wqn::UiScreen::kTime:
            return wqn::UiScreen::kWord;
        case wqn::UiScreen::kWord:
            return wqn::UiScreen::kTodo;
        case wqn::UiScreen::kTodo:
            return wqn::UiScreen::kAi;
        case wqn::UiScreen::kProvisioning:
            return wqn::UiScreen::kHome;
    }
    return wqn::UiScreen::kHome;
}

wqn::ReviewChoice PreviousReviewChoice(wqn::ReviewChoice choice)
{
    switch (choice) {
        case wqn::ReviewChoice::kWrong:
            return wqn::ReviewChoice::kMastered;
        case wqn::ReviewChoice::kNeedsReview:
            return wqn::ReviewChoice::kWrong;
        case wqn::ReviewChoice::kMastered:
            return wqn::ReviewChoice::kNeedsReview;
    }
    return wqn::ReviewChoice::kNeedsReview;
}

wqn::ReviewChoice NextReviewChoice(wqn::ReviewChoice choice)
{
    switch (choice) {
        case wqn::ReviewChoice::kWrong:
            return wqn::ReviewChoice::kNeedsReview;
        case wqn::ReviewChoice::kNeedsReview:
            return wqn::ReviewChoice::kMastered;
        case wqn::ReviewChoice::kMastered:
            return wqn::ReviewChoice::kWrong;
    }
    return wqn::ReviewChoice::kNeedsReview;
}

void AddLine(wqn::UiFrame* frame, wqn::UiTextStyle style, const std::string& text)
{
    if (frame == nullptr) {
        return;
    }
    frame->lines.push_back(wqn::UiLine{style, text});
}

const wqn::CachedProblem* SelectedProblem(const wqn::UiState& state)
{
    if (state.problems.empty() || state.selected_problem >= state.problems.size()) {
        return nullptr;
    }
    return &state.problems[state.selected_problem];
}

std::string StatusLabel(const std::string& status)
{
    if (status == "wrong") {
        return "错题";
    }
    if (status == "needs_review") {
        return "待复习";
    }
    if (status == "mastered") {
        return "已掌握";
    }
    return status;
}

void RenderAi(const wqn::UiState& state, wqn::UiFrame* frame)
{
    if (frame == nullptr) {
        return;
    }
    frame->ai = state.ai;
}

void RenderTodo(const wqn::UiState& state, wqn::UiFrame* frame)
{
    AddLine(frame, wqn::UiTextStyle::kTitle, "Todo");
    const wqn::TodoUiState& todo = state.todo;
    if (todo.sync_status == wqn::TodoSyncStatus::kLoading && !todo.loaded_once) {
        AddLine(frame, wqn::UiTextStyle::kBody, "同步 Todo...");
        return;
    }
    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired) {
        AddLine(frame, wqn::UiTextStyle::kWarning, "请重新配对");
        return;
    }
    if (todo.todos.empty()) {
        AddLine(
            frame,
            todo.sync_status == wqn::TodoSyncStatus::kSyncFailed ? wqn::UiTextStyle::kWarning : wqn::UiTextStyle::kBody,
            todo.sync_status == wqn::TodoSyncStatus::kSyncFailed ? "Todo 同步失败" : "暂无 Todo");
        return;
    }

    const size_t selected = std::min(todo.selected, todo.todos.size() - 1);
    const size_t window_start = selected > 3 ? selected - 3 : 0;
    const size_t window_end = std::min(todo.todos.size(), window_start + static_cast<size_t>(8));
    for (size_t i = window_start; i < window_end; ++i) {
        const wqn::WqnTodoItem& item = todo.todos[i];
        std::string line = item.title.empty() ? "Todo" : item.title;
        if (!item.subject_name.empty()) {
            line += " [" + item.subject_name + "]";
        }
        if (!item.due_at.empty()) {
            line += " " + item.due_at.substr(5, 11);
        }
        const bool is_selected = i == selected;
        AddLine(
            frame,
            is_selected ? wqn::UiTextStyle::kSelected : wqn::UiTextStyle::kBody,
            std::string(is_selected ? "> " : "  ") + Truncate(line, 34));
    }

    std::string footer = std::to_string(selected + 1) + "/" + std::to_string(todo.todos.size()) + "  确认：完成";
    if (todo.sync_status == wqn::TodoSyncStatus::kLoading) {
        footer += "  同步中";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kCompleting) {
        footer += "  完成中";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kCompleteFailed) {
        footer += "  完成失败";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kCompleted) {
        footer += "  已完成";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kSyncFailed) {
        footer += "  同步失败";
    }
    AddLine(frame, wqn::UiTextStyle::kMeta, footer);
}

void RenderHome(const wqn::UiState& state, wqn::UiFrame* frame)
{
    if (frame == nullptr) {
        return;
    }
    frame->home = state.home;
    frame->selected_home_task = state.selected_home_task;
}

void RenderTime(const wqn::UiState& state, wqn::UiFrame* frame)
{
    if (frame == nullptr) {
        return;
    }
    frame->time_app = state.time_app;
}

void RenderWord(const wqn::UiState& state, wqn::UiFrame* frame)
{
    if (frame == nullptr) {
        return;
    }
    frame->word_app = wqn::BuildWordAppSnapshot(state.word_app);
}

void RenderLibrary(const wqn::UiState& state, wqn::UiFrame* frame)
{
    AddLine(frame, wqn::UiTextStyle::kTitle, "错题列表");
    if (state.problems.empty()) {
        AddLine(frame, wqn::UiTextStyle::kWarning, "暂无本地题目");
        AddLine(frame, wqn::UiTextStyle::kMeta, "配对后同步题目");
        return;
    }

    const size_t window_start = state.selected_problem > 3 ? state.selected_problem - 3 : 0;
    const size_t window_end = std::min(state.problems.size(), window_start + 7);
    for (size_t i = window_start; i < window_end; ++i) {
        const wqn::CachedProblem& problem = state.problems[i];
        const std::string title = problem.title.empty() ? problem.id : problem.title;
        const std::string status = problem.status.empty() ? "" : " [" + StatusLabel(problem.status) + "]";
        const std::string prefix = i == state.selected_problem ? "> " : "  ";
        AddLine(frame, i == state.selected_problem ? wqn::UiTextStyle::kSelected : wqn::UiTextStyle::kBody,
                prefix + Truncate(title + status, 34));
    }
    AddLine(frame, wqn::UiTextStyle::kMeta,
            std::to_string(state.selected_problem + 1) + "/" + std::to_string(state.problems.size()) +
                "  确认：打开");
}

void RenderProblemDetail(const wqn::UiState& state, wqn::UiFrame* frame)
{
    const wqn::CachedProblem* problem = SelectedProblem(state);
    AddLine(frame, wqn::UiTextStyle::kTitle, "题目");
    if (problem == nullptr) {
        AddLine(frame, wqn::UiTextStyle::kWarning, "未选择题目");
        return;
    }

    AddLine(frame, wqn::UiTextStyle::kMeta, Truncate(problem->title.empty() ? problem->id : problem->title, 42));
    if (!problem->type.empty()) {
        AddLine(frame, wqn::UiTextStyle::kMeta, "类型：" + problem->type);
    }
    if (!problem->status.empty()) {
        AddLine(frame, wqn::UiTextStyle::kMeta, "状态：" + StatusLabel(problem->status));
    }
    if (problem->asset_count > 0) {
        AddLine(frame, wqn::UiTextStyle::kMeta, "图片：" + std::to_string(problem->asset_count) + " 张，待下载渲染");
    }
    AddLine(frame, wqn::UiTextStyle::kWrappedBody, Truncate(problem->content_text, kPreviewChars));
    AddLine(frame, wqn::UiTextStyle::kMeta, "确认：解析  长按：题库");
}

void RenderSolution(const wqn::UiState& state, wqn::UiFrame* frame)
{
    const wqn::CachedProblem* problem = SelectedProblem(state);
    AddLine(frame, wqn::UiTextStyle::kTitle, "解析");
    if (problem == nullptr) {
        AddLine(frame, wqn::UiTextStyle::kWarning, "未选择题目");
        return;
    }

    AddLine(frame, wqn::UiTextStyle::kWrappedBody,
            problem->solution_text.empty() ? "暂无解析文本" : Truncate(problem->solution_text, kPreviewChars));
    if (problem->solution_asset_count > 0) {
        AddLine(frame, wqn::UiTextStyle::kMeta, "解析图片：" + std::to_string(problem->solution_asset_count) + " 张，待下载渲染");
    }
    AddLine(frame, wqn::UiTextStyle::kMeta, "确认：复习反馈  长按：题库");
}

void RenderReviewQueue(const wqn::UiState& state, wqn::UiFrame* frame)
{
    AddLine(frame, wqn::UiTextStyle::kTitle, "上传队列");
    AddLine(frame, wqn::UiTextStyle::kBody,
            "待上传复习结果：" + std::to_string(state.status.pending_reviews));
    AddLine(frame, wqn::UiTextStyle::kMeta,
            state.status.pending_reviews > 0 ? "联网后自动上传" : "没有本地复习结果");
    AddLine(frame, wqn::UiTextStyle::kMeta, "确认：题库  长按：状态");
}

void RenderReviewScore(const wqn::UiState& state, wqn::UiFrame* frame)
{
    const wqn::CachedProblem* problem = SelectedProblem(state);
    AddLine(frame, wqn::UiTextStyle::kTitle, "复习反馈");
    if (problem == nullptr) {
        AddLine(frame, wqn::UiTextStyle::kWarning, "未选择题目");
        return;
    }

    AddLine(frame, wqn::UiTextStyle::kMeta, Truncate(problem->title.empty() ? problem->id : problem->title, 42));
    const wqn::ReviewChoice choices[] = {
        wqn::ReviewChoice::kWrong,
        wqn::ReviewChoice::kNeedsReview,
        wqn::ReviewChoice::kMastered,
    };
    for (const wqn::ReviewChoice choice : choices) {
        const bool selected = choice == state.selected_review;
        AddLine(frame, selected ? wqn::UiTextStyle::kSelected : wqn::UiTextStyle::kBody,
                std::string(selected ? "> " : "  ") + wqn::ReviewChoiceLabel(choice));
    }
    AddLine(frame, wqn::UiTextStyle::kMeta, "上下：选择  确认：保存");
}

void RenderReviewQueued(const wqn::UiState& state, wqn::UiFrame* frame)
{
    AddLine(frame, wqn::UiTextStyle::kTitle, "已记录");
    if (!state.last_review_message.empty()) {
        AddLine(frame, wqn::UiTextStyle::kBody, state.last_review_message);
    } else {
        AddLine(frame, wqn::UiTextStyle::kBody, "已保存，等待上传");
    }
    AddLine(frame, wqn::UiTextStyle::kMeta,
            "待上传：" + std::to_string(state.status.pending_reviews));
    AddLine(frame, wqn::UiTextStyle::kMeta, "确认：题库  上：题目");
}

}  // namespace

namespace wqn {

const char* ReviewChoiceLabel(ReviewChoice choice)
{
    switch (choice) {
        case ReviewChoice::kWrong:
            return "还是错了";
        case ReviewChoice::kNeedsReview:
            return "需要再复习";
        case ReviewChoice::kMastered:
            return "已经掌握";
    }
    return "需要再复习";
}

const char* ReviewChoiceStatus(ReviewChoice choice)
{
    switch (choice) {
        case ReviewChoice::kWrong:
            return "wrong";
        case ReviewChoice::kNeedsReview:
            return "needs_review";
        case ReviewChoice::kMastered:
            return "mastered";
    }
    return "needs_review";
}

size_t AiSessionTextPageCount(const AiSessionState& ai)
{
    std::string text = AiPagedTextSource(ai);
    if (text.empty()) {
        return 1;
    }

    const bool has_user_text = !ai.user_text.empty() || !ai.user_partial.empty();
    const std::string user_text = has_user_text
        ? (ai.user_text.empty() ? ai.user_partial : ai.user_text)
        : "";
    std::vector<std::string> user_lines;
    if (!user_text.empty()) {
        user_lines = wqn::WrapUtf8TextToWidth(user_text, 388, 2);
    }

    const int assistant_y = user_lines.empty() ? 34 : (34 + user_lines.size() * 18 + 6);
    const int assistant_height = 294 - assistant_y;
    const int assistant_max_lines = (assistant_height - 14) / 18;

    const std::vector<std::string> lines = wqn::WrapUtf8TextToWidth(text, 388 - 16, 9999);
    return std::max<size_t>(1, (lines.size() + assistant_max_lines - 1) / assistant_max_lines);
}


size_t AiSessionPageCount(const AiSessionState& ai)
{
    const size_t action_pages = ai.function_call_summaries.empty() ? 0 : 1;
    return AiSessionTextPageCount(ai) + action_pages;
}

void ClampUiSelection(UiState* state)
{
    if (state == nullptr) {
        return;
    }
    if (state->home.tasks.empty()) {
        state->selected_home_task = 0;
    } else if (state->selected_home_task >= state->home.tasks.size()) {
        state->selected_home_task = state->home.tasks.size() - 1;
    }
    if (state->problems.empty()) {
        state->selected_problem = 0;
    } else if (state->selected_problem >= state->problems.size()) {
        state->selected_problem = state->problems.size() - 1;
    }
    if (state->todo.todos.empty()) {
        state->todo.selected = 0;
    } else if (state->todo.selected >= state->todo.todos.size()) {
        state->todo.selected = state->todo.todos.size() - 1;
    }
    if (state->settings.selected >= 6) {
        state->settings.selected = 5;
    }
    const size_t ai_pages = AiSessionPageCount(state->ai);
    if (state->ai.page >= ai_pages) {
        state->ai.page = ai_pages > 0 ? ai_pages - 1 : 0;
    }
}

void HandleUiInput(UiState* state, UiInput input)
{
    if (state == nullptr) {
        return;
    }
    ClampUiSelection(state);
    const wqn::UiScreen screen_before = state->screen;

    switch (input) {
        case UiInput::kTopPrevious:
            state->screen = PreviousTopScreen(state->screen);
            break;

        case UiInput::kTopNext:
            state->screen = NextTopScreen(state->screen);
            break;

        case UiInput::kUp:
            if (state->screen == UiScreen::kTime) {
                HandleTimeAppInput(&state->time_app, TimeInput::kUp);
                break;
            } else if (state->screen == UiScreen::kWord) {
                HandleWordAppInput(&state->word_app, WordInput::kUp);
                break;
            } else if (state->screen == UiScreen::kAi) {
                if (state->ai.page > 0) {
                    --state->ai.page;
                }
            } else if (state->screen == UiScreen::kHome && state->selected_home_task > 0) {
                --state->selected_home_task;
            } else if (state->screen == UiScreen::kTodo && state->todo.selected > 0) {
                --state->todo.selected;
            } else if (state->screen == UiScreen::kSettings) {
                if (state->settings.selected > 0) {
                    --state->settings.selected;
                }
            } else if (state->screen == UiScreen::kReviewScore) {
                state->selected_review = PreviousReviewChoice(state->selected_review);
            } else if (state->screen == UiScreen::kLibrary && state->selected_problem > 0) {
                --state->selected_problem;
            } else if (state->screen == UiScreen::kSolution) {
                state->screen = UiScreen::kProblem;
            } else if (state->screen == UiScreen::kReviewQueued) {
                state->screen = UiScreen::kProblem;
            }
            break;

        case UiInput::kDown:
            if (state->screen == UiScreen::kTime) {
                HandleTimeAppInput(&state->time_app, TimeInput::kDown);
                break;
            } else if (state->screen == UiScreen::kWord) {
                HandleWordAppInput(&state->word_app, WordInput::kDown);
                break;
            } else if (state->screen == UiScreen::kAi) {
                if (state->ai.page + 1 < AiSessionPageCount(state->ai)) {
                    ++state->ai.page;
                }
            } else if (state->screen == UiScreen::kHome && state->selected_home_task + 1 < state->home.tasks.size()) {
                ++state->selected_home_task;
            } else if (state->screen == UiScreen::kTodo && state->todo.selected + 1 < state->todo.todos.size()) {
                ++state->todo.selected;
            } else if (state->screen == UiScreen::kSettings) {
                if (state->settings.selected + 1 < 6) {
                    ++state->settings.selected;
                }
            } else if (state->screen == UiScreen::kReviewScore) {
                state->selected_review = NextReviewChoice(state->selected_review);
            } else if (state->screen == UiScreen::kLibrary && state->selected_problem + 1 < state->problems.size()) {
                ++state->selected_problem;
            } else if (state->screen == UiScreen::kProblem) {
                state->screen = UiScreen::kSolution;
            }
            break;

        case UiInput::kConfirm:
            if (state->screen == UiScreen::kTime) {
                HandleTimeAppInput(&state->time_app, TimeInput::kConfirm);
                break;
            } else if (state->screen == UiScreen::kWord) {
                HandleWordAppInput(&state->word_app, WordInput::kConfirm);
                break;
            } else if (state->screen == UiScreen::kHome) {
                if (state->selected_home_task == 1) {
                    state->screen = UiScreen::kWord;
                } else {
                    state->screen = (state->selected_home_task == 0 && !state->problems.empty()) ? UiScreen::kProblem :
                                                                                                     UiScreen::kLibrary;
                }
            } else if (state->screen == UiScreen::kLibrary && !state->problems.empty()) {
                state->screen = UiScreen::kProblem;
            } else if (state->screen == UiScreen::kProblem) {
                state->screen = UiScreen::kSolution;
            } else if (state->screen == UiScreen::kSolution) {
                state->screen = UiScreen::kReviewScore;
            } else if (state->screen == UiScreen::kReviewQueue || state->screen == UiScreen::kReviewQueued) {
                state->screen = UiScreen::kLibrary;
            } else if (state->screen == UiScreen::kSettings) {
                // Settings actions are handled in device_ui.cpp because they need storage and diagnostics.
            }
            break;

        case UiInput::kLongConfirm:
            if (state->screen == UiScreen::kTime) {
                HandleTimeAppInput(&state->time_app, TimeInput::kLongConfirm);
                break;
            } else if (state->screen == UiScreen::kWord) {
                // Long-press confirm = universal back. At the word home, exit to
                // the device home screen; in any deeper word mode, let the word
                // app pop one level.
                if (state->word_app.mode == WordAppMode::kHome) {
                    state->screen = UiScreen::kHome;
                } else {
                    HandleWordAppInput(&state->word_app, WordInput::kLongConfirm);
                }
                break;
            } else if (state->screen == UiScreen::kAi) {
                if (state->ai.status != AiSessionStatus::kListening &&
                    state->ai.status != AiSessionStatus::kWaitingReply) {
#if CONFIG_WQN_AI_ENABLE
                    // [ptt-fix] Flash tier uses instant kPress/kRelease events
                    // routed through ui_input.cpp; skip the long-confirm path
                    // entirely so PTT doesn't have to wait 1 s for the long-
                    // press threshold.
                    if (state->ai.tier != AiTier::kFlash &&
                        StartAiRecordingSession() == ESP_OK) {
                        AiSessionState ai_state;
                        if (CopyAiSessionToUi(&ai_state)) {
                            state->ai = ai_state;
                        }
                    } else {
                        AiSessionState ai_error;
                        if (CopyAiSessionToUi(&ai_error)) {
                            state->ai = ai_error;
                        } else {
                            state->ai.status = AiSessionStatus::kError;
                            state->ai.pending_text.clear();
                            state->ai.assistant_text = "AI 启动失败";
                        }
                    }
#else
                    state->ai.status = AiSessionStatus::kListening;
                    state->ai.pending_text = "AI 功能未启用";
                    state->ai.status_since_ms = 0;
#endif
                }
                break;
            } else if (state->screen == UiScreen::kHome) {
                state->screen = UiScreen::kSettings;
            } else if (state->screen == UiScreen::kSettings) {
                state->screen = UiScreen::kHome;
            } else {
                state->screen = UiScreen::kLibrary;
            }
            break;
    }
    ClampUiSelection(state);
#if CONFIG_WQN_AI_ENABLE
    // Leaving the AI screen while in Flash tier must tear down the WebSocket
    // and the audio streaming task — otherwise they keep running in the
    // background and drain the battery.
    if (screen_before == wqn::UiScreen::kAi && state->screen != wqn::UiScreen::kAi &&
        state->ai.tier == wqn::AiTier::kFlash) {
        wqn::StopFlashSession();
    }
#endif
}

bool TickAiSession(UiState* state, int64_t now_ms)
{
#if CONFIG_WQN_AI_ENABLE
    (void)state;
    (void)now_ms;
    return false;
#else
    if (state == nullptr) {
        return false;
    }
    if (state->ai.status == AiSessionStatus::kWaitingReply && state->ai.status_since_ms > 0 &&
        now_ms - state->ai.status_since_ms >= kAiMockReplyDelayMs) {
        state->ai.status = AiSessionStatus::kError;
        state->ai.assistant_text = "AI 功能未启用";
        state->ai.pending_text.clear();
        state->ai.status_since_ms = now_ms;
        return true;
    }
    return false;
#endif
}

// [tier-flash] One-shot flag set by RequestForceFullRefresh() and consumed by
// RenderUiFrame. Lets callers like the AI tier-switch handler force a full
// EPD refresh without changing the RenderUiFrame API.
bool g_force_full_refresh_next = false;
void RequestForceFullRefresh() { g_force_full_refresh_next = true; }
bool ConsumeForceFullRefresh() {
    bool val = g_force_full_refresh_next;
    g_force_full_refresh_next = false;
    return val;
}

UiFrame RenderUiFrame(const UiState& state)
{
    UiFrame frame;

    // [fix] While WiFi provisioning is active, override the screen with a
    // provisioning prompt (which SoftAP to join + captive-portal URL). The
    // generic fallback in RenderFrameToEpd draws frame.lines for screens it
    // doesn't specialise, so no new renderer/dispatch is needed.
    if (wqn::GetProvisioningState() != wqn::ProvisionState::kIdle &&
        wqn::GetProvisioningApSsid()[0] != '\0') {
        frame.screen = UiScreen::kProvisioning;
        frame.prefer_full_refresh = true;
        AddLine(&frame, UiTextStyle::kTitle, "设备配网");
        AddLine(&frame, UiTextStyle::kBody, "请用手机连接 WiFi 热点:");
        AddLine(&frame, UiTextStyle::kSelected, wqn::GetProvisioningApSsid());
        AddLine(&frame, UiTextStyle::kBody, "然后浏览器访问:");
        AddLine(&frame, UiTextStyle::kSelected, "192.168.4.1");
        AddLine(&frame, UiTextStyle::kMeta, "输入家庭 WiFi 完成配网");
        return frame;
    }

    frame.screen = state.screen;
    frame.home = state.home;
    frame.ai = state.ai;
    frame.todo = state.todo;
    frame.word_app = BuildWordAppSnapshot(state.word_app);
    frame.settings = state.settings;
    frame.selected_home_task = state.selected_home_task;
    // [force-full-fix] Check the one-shot flag but do NOT consume it here.
    // RenderUiFrame may be called multiple times per tick (once for signature
    // comparison, once for actual render). If we clear it here, the signature
    // call eats the flag and the actual render frame gets prefer_full=false.
    // Consumption is deferred to ConsumeForceFullRefresh() called by the
    // render path in device_ui.cpp after it has the final frame.
    frame.prefer_full_refresh = (state.screen == UiScreen::kHome) || g_force_full_refresh_next;
    if (state.screen != UiScreen::kHome) {
        AddLine(&frame, UiTextStyle::kMeta, ScreenName(state.screen));
    }

    switch (state.screen) {
        case UiScreen::kAi:
            RenderAi(state, &frame);
            break;
        case UiScreen::kTodo:
            RenderTodo(state, &frame);
            break;
        case UiScreen::kSettings:
            break;
        case UiScreen::kHome:
            RenderHome(state, &frame);
            break;
        case UiScreen::kTime:
            RenderTime(state, &frame);
            break;
        case UiScreen::kWord:
            RenderWord(state, &frame);
            break;
        case UiScreen::kLibrary:
            RenderLibrary(state, &frame);
            break;
        case UiScreen::kProblem:
            RenderProblemDetail(state, &frame);
            break;
        case UiScreen::kSolution:
            RenderSolution(state, &frame);
            break;
        case UiScreen::kReviewQueue:
            RenderReviewQueue(state, &frame);
            break;
        case UiScreen::kReviewScore:
            RenderReviewScore(state, &frame);
            break;
        case UiScreen::kReviewQueued:
            RenderReviewQueued(state, &frame);
            break;
        case UiScreen::kProvisioning:
            // Handled by the provisioning early-return above; unreachable here.
            break;
    }

    return frame;
}

}  // namespace

namespace wqn {

AiTier NextAiTier(AiTier current)
{
    return static_cast<AiTier>((static_cast<uint8_t>(current) + 1) % static_cast<uint8_t>(AiTier::kCount));
}

AiTier PrevAiTier(AiTier current)
{
    const uint8_t val = static_cast<uint8_t>(current);
    return static_cast<AiTier>((val + static_cast<uint8_t>(AiTier::kCount) - 1) % static_cast<uint8_t>(AiTier::kCount));
}

const char* AiTierLabel(AiTier tier)
{
    switch (tier) {
        case AiTier::kFlash:
            return "Flash";
        case AiTier::kStd:
            return "STD";
        case AiTier::kPro:
            return "Pro";
        default:
            return "???";
    }
}

}  // namespace wqn
