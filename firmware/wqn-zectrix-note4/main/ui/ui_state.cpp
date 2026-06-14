// UI state management: load from NVS/storage, build home summary, queue review result,
// detect time-app structural changes.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <string>
#include <vector>

#include "esp_log.h"
#include "online_sync.h"
#include "storage.h"
#include "wifi_manager.h"
#include "word_app.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

bool SameTimeAppState(const wqn::TimeAppState& a, const wqn::TimeAppState& b)
{
    return a.tile == b.tile && a.active_mode == b.active_mode && a.status == b.status &&
           a.pomodoro_phase == b.pomodoro_phase && a.config_mode == b.config_mode &&
           a.is_editing == b.is_editing && a.active_field == b.active_field &&
           a.remaining_seconds == b.remaining_seconds && a.countdown_hours == b.countdown_hours &&
           a.countdown_minutes == b.countdown_minutes && a.countdown_seconds == b.countdown_seconds &&
           a.countdown_total_seconds == b.countdown_total_seconds && a.pomodoro_rounds == b.pomodoro_rounds &&
           a.pomodoro_focus_minutes == b.pomodoro_focus_minutes &&
           a.pomodoro_break_minutes == b.pomodoro_break_minutes &&
           a.pomodoro_long_break_minutes == b.pomodoro_long_break_minutes &&
           a.pomodoro_current_round == b.pomodoro_current_round;
}

bool TimeAppStructureChanged(const wqn::TimeAppState& before, const wqn::TimeAppState& after)
{
    return before.tile != after.tile || before.active_mode != after.active_mode || before.status != after.status ||
           before.config_mode != after.config_mode || before.pomodoro_phase != after.pomodoro_phase;
}

void BuildHomeSummary(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }

    wqn::HomeSummary home;
    home.wifi_label = state->status.wifi_connected ? "WiFi" : "离线";
    BatteryReading battery = {};
    home.battery_label = ReadBatteryStatus(&battery) ? BatteryLabel(battery) : "--%";

    // UI contract: one line only. Source priority is pomodoro > countdown > clock.
    home.primary_time_line = ChooseHomePrimaryTimeLine(state->time_app);

    const int review_count = CountReviewDueLikeProblems(state->problems);
    home.review_metric.value = std::to_string(review_count);
    home.review_metric.label = "今日复习";
    home.todo_metric.value = std::to_string(std::max(0, state->todo.total_pending));
    home.todo_metric.label = "今日 Todo";
    home.word_metric.value = wqn::WordAppProgressLabel(state->word_app);
    home.word_metric.label = "单词进度";
    home.current_status =
        "本地 " + std::to_string(state->problems.size()) + " 题 · 待上传 " +
        std::to_string(state->status.pending_reviews);

    home.tasks.clear();
    if (!state->problems.empty()) {
        const wqn::CachedProblem& problem = state->problems[std::min(state->selected_problem, state->problems.size() - 1)];
        wqn::HomeTask task;
        task.title = problem.title.empty() ? problem.id : problem.title;
        task.subtitle = "错题复习" + std::string(problem.status == "mastered" ? "，已掌握" : "，待复习");
        task.tag = "错题";
        home.tasks.push_back(std::move(task));
    } else {
        home.tasks.push_back(wqn::HomeTask{"同步错题后开始复习", "当前没有本地题目缓存", "错题"});
    }
    home.tasks.push_back(wqn::HomeTask{"单词复习", wqn::WordAppStatusLine(state->word_app), "单词"});

    state->home = std::move(home);
    wqn::ClampUiSelection(state);
}

bool LoadUiState(wqn::UiState* state)
{
    if (state == nullptr) {
        return false;
    }

    std::vector<wqn::CachedProblem> problems;
    esp_err_t result = wqn::LoadProblems(&problems);
    if (result == ESP_OK) {
        state->problems = std::move(problems);
    } else {
        ESP_LOGW(kTag, "load UI problem cache failed: %s", esp_err_to_name(result));
    }

    result = wqn::InitWordApp(&state->word_app);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "init word app failed: %s", esp_err_to_name(result));
    }

    std::vector<wqn::PendingReviewResult> pending;
    result = wqn::LoadPendingReviewResults(&pending);
    if (result == ESP_OK) {
        state->status.pending_reviews = static_cast<int>(pending.size());
    } else {
        ESP_LOGW(kTag, "load UI pending queue failed: %s", esp_err_to_name(result));
    }

    std::string token;
    result = wqn::LoadAccessToken(&token);
    if (result == ESP_OK && !token.empty() && wqn::IsValidAccessToken(token)) {
        state->status.paired = true;
        state->status.token_mask = wqn::MaskTokenForLog(token);
    } else {
        state->status.paired = false;
        state->status.token_mask.clear();
    }

#if CONFIG_WQN_WIFI_STA_ENABLE
    state->status.wifi_enabled = true;
    state->status.wifi_connected = wqn::IsWifiStationConnected();
#else
    state->status.wifi_enabled = false;
    state->status.wifi_connected = false;
#endif

    UpdateSettingsDiagnostics(state);
    wqn::ClampUiSelection(state);
    BuildHomeSummary(state);
    return true;
}

RefreshSchedule QueueSelectedReview(wqn::UiState* state)
{
    if (state == nullptr || state->problems.empty() || state->selected_problem >= state->problems.size()) {
        return RefreshSchedule::kNone;
    }

    const wqn::CachedProblem& problem = state->problems[state->selected_problem];
    wqn::PendingReviewResult review;
    review.problem_id = problem.id;
    review.selected_status = wqn::ReviewChoiceStatus(state->selected_review);
    review.is_correct = state->selected_review == wqn::ReviewChoice::kMastered;
    review.created_at = CurrentIsoTimestamp();

    const esp_err_t result = wqn::EnqueueReviewResult(review);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "enqueue review failed: %s", esp_err_to_name(result));
        state->last_review_message = "保存失败";
        state->screen = wqn::UiScreen::kReviewQueued;
        return RefreshSchedule::kCommit;
    }

    std::vector<wqn::PendingReviewResult> pending;
    if (wqn::LoadPendingReviewResults(&pending) == ESP_OK) {
        state->status.pending_reviews = static_cast<int>(pending.size());
    } else {
        ++state->status.pending_reviews;
    }

    state->last_review_message = std::string("已保存：") + wqn::ReviewChoiceLabel(state->selected_review);
    state->status.last_sync_status = "复习结果待上传";
    state->problems[state->selected_problem].status = review.selected_status;
    const esp_err_t cache_result = wqn::SaveProblems(state->problems);
    if (cache_result != ESP_OK) {
        ESP_LOGW(kTag, "save reviewed problem cache failed: %s", esp_err_to_name(cache_result));
    }
    state->screen = wqn::UiScreen::kReviewQueued;
    ESP_LOGI(
        kTag,
        "queued review result: problem_id=%s status=%s pending=%d",
        problem.id.c_str(),
        review.selected_status.c_str(),
        state->status.pending_reviews);
    wqn::NotifyOnlineSyncRequested();
    return RefreshSchedule::kCommit;
}

}  // namespace device_ui_internal
