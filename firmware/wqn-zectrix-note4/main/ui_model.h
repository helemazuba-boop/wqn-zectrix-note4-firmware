#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "storage.h"
#include "time_app.h"
#include "word_app.h"
#include "wqn_api.h"

namespace wqn {

enum class UiScreen {
    kAi,
    kTodo,
    kSettings,
    kHome,
    kTime,
    kWord,
    kLibrary,
    kProblem,
    kSolution,
    kReviewQueue,
    kReviewScore,
    kReviewQueued,
};

enum class UiInput {
    kUp,
    kDown,
    kConfirm,
    kLongConfirm,
    kTopPrevious,
    kTopNext,
};

enum class UiTextStyle {
    kTitle,
    kBody,
    kMeta,
    kSelected,
    kWarning,
    kWrappedBody,
};

enum class ReviewChoice {
    kWrong,
    kNeedsReview,
    kMastered,
};

enum class AiSessionStatus {
    kIdle,
    kListening,
    kWaitingReply,
    kReplyReady,
    kError,
};

struct AiSessionState {
    AiSessionStatus status = AiSessionStatus::kIdle;
    std::string user_text;
    std::string assistant_text;
    std::string pending_text;
    std::string status_detail;
    std::vector<std::string> function_call_summaries;
    std::string conversation_id;
    int64_t status_since_ms = 0;
    size_t page = 0;
};

enum class TodoSyncStatus {
    kIdle,
    kLoading,
    kReady,
    kSyncFailed,
    kCompleting,
    kCompleteFailed,
    kCompleted,
    kAuthRequired,
};

struct TodoUiState {
    std::vector<WqnTodoItem> todos;
    size_t selected = 0;
    TodoSyncStatus sync_status = TodoSyncStatus::kIdle;
    std::string status_message;
    bool loaded_once = false;
    int total_pending = 0;
    std::string previous_cursor;
    std::string next_cursor;
    bool has_earlier = false;
    bool has_later = false;
};

struct HomeMetric {
    std::string value;
    std::string label;
};

struct HomeTask {
    std::string title;
    std::string subtitle;
    std::string tag;
};

struct HomeSummary {
    std::string wifi_label = "WiFi";
    std::string battery_label = "--%";
    std::string primary_time_line = "--:--";
    HomeMetric review_metric = {"0", "今日复习"};
    HomeMetric todo_metric = {"--", "今日 Todo"};
    HomeMetric word_metric = {"--%", "单词进度"};
    std::string current_status = "当前进行";
    std::vector<HomeTask> tasks;
};

enum class SettingsDialog {
    kNone,
    kAutoSync,
    kBattery,
    kStorage,
    kFactoryReset,
};

struct SettingsDiagnosticsSnapshot {
    int adc_raw = 0;
    int adc_mv = 0;
    int battery_mv = 0;
    int battery_percent = 0;
    bool charging = false;
    bool full = false;
    uint32_t flash_size = 0;
    size_t nvs_used_entries = 0;
    size_t nvs_free_entries = 0;
    size_t nvs_total_entries = 0;
    size_t psram_total = 0;
    size_t psram_free = 0;
    size_t psram_used = 0;
    std::string mac_label;
    std::string firmware_version;
    std::string board_id;
    std::string idf_target;
};

struct SettingsAppState {
    size_t selected = 0;
    SettingsDialog dialog = SettingsDialog::kNone;
    size_t auto_sync_selected = 0;
    uint32_t auto_sync_interval_min = 0;
    std::string sync_status;
    std::string notice;
    SettingsDiagnosticsSnapshot diagnostics;
};

struct UiRuntimeStatus {
    bool wifi_enabled = false;
    bool wifi_connected = false;
    bool paired = false;
    bool syncing = false;
    int pending_reviews = 0;
    std::string token_mask;
    std::string last_sync_status;
};

struct UiState {
    UiScreen screen = UiScreen::kHome;
    size_t selected_home_task = 0;
    size_t selected_problem = 0;
    ReviewChoice selected_review = ReviewChoice::kNeedsReview;
    std::string last_review_message;
    UiRuntimeStatus status;
    AiSessionState ai;
    TimeAppState time_app;
    WordAppState word_app;
    TodoUiState todo;
    SettingsAppState settings;
    HomeSummary home;
    std::vector<CachedProblem> problems;
};

struct UiLine {
    UiTextStyle style = UiTextStyle::kBody;
    std::string text;
};

struct UiFrame {
    UiScreen screen = UiScreen::kHome;
    bool prefer_full_refresh = false;
    HomeSummary home;
    AiSessionState ai;
    TodoUiState todo;
    TimeAppState time_app;
    WordAppSnapshot word_app;
    SettingsAppState settings;
    size_t selected_home_task = 0;
    std::vector<UiLine> lines;
};

void ClampUiSelection(UiState* state);
void HandleUiInput(UiState* state, UiInput input);
size_t AiSessionTextPageCount(const AiSessionState& ai);
size_t AiSessionPageCount(const AiSessionState& ai);
bool TickAiSession(UiState* state, int64_t now_ms);
UiFrame RenderUiFrame(const UiState& state);
const char* ReviewChoiceLabel(ReviewChoice choice);
const char* ReviewChoiceStatus(ReviewChoice choice);

}  // namespace wqn
