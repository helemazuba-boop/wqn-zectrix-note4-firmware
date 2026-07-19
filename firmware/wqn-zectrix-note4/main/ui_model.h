#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ai_history.h"
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
    kProvisioning,
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
    kStreaming,         // v2 SSE: connected, server is producing events
    kReplyReady,
    kError,
};

enum class AiTier : uint8_t {
    kFlash = 0,
    kStd = 1,
    kPro = 2,
    kCount = 3,
};

AiTier NextAiTier(AiTier current);
AiTier PrevAiTier(AiTier current);
const char* AiTierLabel(AiTier tier);

// [shell] Thinking effort level shown as the status-bar lightbulb icon
// (off/low/med/high = 0..3 rays). Local-only for now (server controls the real
// thinking level); the status-bar toggle cycles this for display.
enum class ThinkingLevel : uint8_t {
    kOff = 0,
    kLow,
    kMed,
    kHigh,
    kCount = 4,
};

struct AiSessionState {
    AiSessionStatus status = AiSessionStatus::kIdle;
    AiTier tier = AiTier::kStd;
    std::string user_text;
    std::string assistant_text;
    std::string pending_text;
    std::string status_detail;
    std::vector<std::string> function_call_summaries;
    std::string conversation_id;
    int64_t status_since_ms = 0;
    size_t page = 0;

    // Flash realtime session fields (used when tier == kFlash)
    std::string flash_transcript;
    std::string flash_pending;
    std::string flash_error;
    std::string flash_status_label;  // [phase-fix] 连接/就绪/录音/识别/生成/播放/错误
    bool flash_is_streaming = false;

    // v2 SSE incremental render fields (Std/Pro tiers).
    // assistant_partial accumulates streamed text.delta events between two
    // commits and is flushed to assistant_text on text.end / final.
    std::string assistant_partial;
    std::string user_partial;          // transient asr.delta buffer
    int64_t last_render_ms = 0;        // last chunked partial-refresh timestamp

    // v2 multi-turn layout fields: scroll offset for the chat viewport
    // (in 18-px line units, 0 == top of newest message) and toast state for
    // status broadcasts above the viewport. The toast never carries an
    // elapsed-seconds counter by design; the user reaches "still working"
    // signal by the toast being present at all, with a blink dot.
    int32_t scroll_offset_lines = 0;
    bool toast_visible = false;
    std::string toast_label;       // e.g. "\xe2\x97\x8f 录音中 00:04" / "\xe2\x97\x8f 上传…"
    int64_t toast_since_ms = 0;
    int32_t toast_recording_ms = 0;  // running counter for the recording label (only)
    int64_t scroll_no_op_hint_ms = 0; // when Down is pressed at the bottom, briefly stamp a "已最新" hint

    // [shell] Status-bar quick toggles (AI page). Local, cyclable via the
    // status-bar edit mode; NOT wired to backend (thinking level is server-
    // controlled, TTS/Flash sound WIP). See docs/13-ui-design-language.md.
    ThinkingLevel thinking_level = ThinkingLevel::kMed;
    bool tts_on = false;
    bool expand_content = true;   // [expand] default full thinking/tool display (Flash can't toggle)
};

// Returns the current scroll offset in 18-px line units (0 == bottom / newest).
// UI tasks read this under the AI mutex; safe to call from any task.
int32_t GetAiScrollOffsetLines();

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
    // Raw state for status-bar icons (font_zectrix). Populated by BuildHomeSummary.
    int battery_percent = 0;
    bool charging = false;
    bool full = false;
    bool wifi_connected = false;
    int wifi_rssi = 0;  // dBm (e.g. -65); 0 if not connected.
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
    kVolume,
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
    int volume_percent = 100;
    size_t volume_selected = 0;
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
    std::string claim_code;
    std::string last_sync_status;
};

// [shell] Status-bar edit mode (global, currently AI page only): which toggle
// icon is selected and when the last edit happened (for the 3 s exit timeout).
// The toggle VALUES themselves live in the per-page app state (e.g. AI's
// thinking_level / tts_on / expand_content in AiSessionState).
struct StatusBarEditState {
    bool active = false;
    uint8_t selected = 0;       // index into the page's toggle list
    int64_t last_action_ms = 0; // for 3 s inactivity auto-exit
    int64_t last_cycle_ms = 0;  // [shell] ts of last forward cycle; a 2nd short
                                // confirm within kStatusBarEditDblMs = save & exit
};

// Reducer-owned gesture memory. Keeping these values inside AppState removes
// hidden file-static history from button reduction, so replaying the same
// timestamped input sequence starts from and produces the same state.
struct UiGestureState {
    bool flash_ptt_started = false;
    int64_t last_ai_confirm_tap_ms = 0;
};

// M4: the application state has exactly one owner (UiRuntime on DeviceUiTask).
// `revision` is advanced by the reducer for every accepted state mutation and
// is also used as the revision of the resulting DisplayIntent.  Keep the
// UiState alias during page-by-page migration so render code does not need a
// flag-day rename.
struct AppState {
    uint64_t revision = 0;
    UiScreen screen = UiScreen::kHome;
    StatusBarEditState status_edit;
    UiGestureState gestures;
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

using UiState = AppState;

struct UiLine {
    UiTextStyle style = UiTextStyle::kBody;
    std::string text;
};

struct UiFrame {
    UiScreen screen = UiScreen::kHome;
    bool prefer_full_refresh = false;
    StatusBarEditState status_edit;  // [shell] status-bar edit mode for render
    HomeSummary home;
    AiSessionState ai;
    std::shared_ptr<const AiHistorySnapshot> ai_history;
    uint64_t ai_history_revision = 0;
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
void RequestForceFullRefresh();  // one-shot: next RenderUiFrame forces full refresh
bool ConsumeForceFullRefresh();  // returns true if flag was set, then clears it
const char* ReviewChoiceLabel(ReviewChoice choice);
const char* ReviewChoiceStatus(ReviewChoice choice);

}  // namespace wqn
