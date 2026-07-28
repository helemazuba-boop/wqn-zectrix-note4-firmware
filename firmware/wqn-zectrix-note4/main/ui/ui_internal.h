// Internal header for device_ui subsystem (split from device_ui.cpp).
// Exposes cross-file symbols: types, enums, constants, free function declarations,
// and external globals shared between translation units in main/ui/.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ui_model.h"
#include "wqn_api.h"
#include "config.h"
#include "display_service.h"
#include "button_input.h"
#include "time_app.h"
#include "services/sync_service.h"
#include "esp_timer.h"
#include "power_manager.h"
#include "ui_layout.h"
#include "typography.h"
#include "ui/ui_widgets.h"
#include "ui/assets/wqn_bitmap_asset.h"
#include "display/display_types.h"

namespace device_ui_internal {

class UiRuntime;

constexpr std::time_t kMinReasonableUnixTime = 1704067200;  // 2024-01-01 UTC

// ---- Schedule enum and helpers ---------------------------------------------

enum class RefreshSchedule {
    kNone,
    kConfig,
    kAi,
    kClock,
    kSelection,
    kTimer,
    kCoalesced,
    kCommit,
    kImmediate,
};

int RefreshRank(RefreshSchedule schedule);
const char* RefreshScheduleName(RefreshSchedule schedule);
TickType_t RefreshDelay(RefreshSchedule schedule);
bool TickReached(TickType_t now, TickType_t due);
TickType_t TicksUntil(TickType_t now, TickType_t due);
RefreshSchedule StrongerSchedule(RefreshSchedule a, RefreshSchedule b);

// ---- Shared types -----------------------------------------------------------

// UiRect is now defined in ui_layout.h (geometry owned alongside layout
// tokens so ui_widgets.h can reference it without including ui_internal.h).

constexpr TickType_t kCommitRefreshDelay = pdMS_TO_TICKS(120);
constexpr TickType_t kClockRefreshDelay = pdMS_TO_TICKS(80);
constexpr TickType_t kTimerRefreshDelay = pdMS_TO_TICKS(80);
// [feel] Coalescing window for selection moves. 180 ms rarely merged anything
// (real key intervals sit above the ~400-700 ms refresh time) but added its
// full length to EVERY single press; 100 ms keeps burst coalescing while
// cutting ~80 ms off each visible cursor step.
constexpr TickType_t kSelectionRefreshDelay = pdMS_TO_TICKS(100);
constexpr TickType_t kConfigRefreshDelay = pdMS_TO_TICKS(120);
constexpr TickType_t kAiRefreshDelay = pdMS_TO_TICKS(120);

constexpr size_t kSettingsItemCount = 8;
// Settings rows overflow the panel at 38px pitch; the list renders a
// selection-following window of this many rows.
constexpr size_t kSettingsVisibleRows = 6;
constexpr uint32_t kAutoSyncOptions[] = {0, 15, 30, 60, 240};
constexpr std::size_t kAutoSyncOptionsCount = sizeof(kAutoSyncOptions) / sizeof(kAutoSyncOptions[0]);
constexpr int kVolumeOptions[] = {0, 25, 50, 75, 100};
constexpr std::size_t kVolumeOptionsCount = sizeof(kVolumeOptions) / sizeof(kVolumeOptions[0]);

// kStatusBarRect removed: dead constant (height 30 was never referenced).
// Use kStatusBarHeight / kStatusBarDividerY in ui_layout.h instead.
constexpr UiRect kHomePrimaryRect = {8, 33, 384, 31, "home-primary-time"};
constexpr UiRect kTimeStandbyRect = {0, 42, wqn::kEpdWidth, 172, "time-standby"};
constexpr UiRect kTimerRunRect = {0, 58, wqn::kEpdWidth, 186, "timer-run"};
constexpr UiRect kCountdownConfigRect = {0, 70, wqn::kEpdWidth, 205, "countdown-config"};
constexpr UiRect kPomodoroConfigRect = {0, 54, wqn::kEpdWidth, 218, "pomodoro-config"};
constexpr UiRect kSettingsContentRect = {0, 32, wqn::kEpdWidth, 252, "settings-content"};

UiRect ConfigRefreshRect(const wqn::TimeAppState& time_app);

struct BatteryReading {
    int raw = 0;
    int adc_mv = 0;
    int battery_mv = 0;
    int percent = 0;
    int chrg_l = 1;
    int stdby_h = 0;
    bool charging = false;
    bool full = false;
    bool power_present_or_status_known = false;
    const char* pmu_status = "unknown";
    bool pmu_implemented = false;
};

bool ReadBatteryStatus(BatteryReading* reading);
std::string BatteryLabel(const BatteryReading& reading);
void CheckLowBatteryProtection(const BatteryReading* reading);
void CheckBatteryProtection();

size_t AutoSyncOptionIndex(uint32_t minutes);
size_t VolumeOptionIndex(int percent);
std::string OnlineSyncStatusLabel(const char* status);
std::string BytesLabel(size_t bytes);
void UpdateSettingsDiagnostics(wqn::UiState* state);

// ---- Time-page internal helpers (defined in page_time.cpp) ----------------

void DrawStatusBar(const char* title, const wqn::HomeSummary& home);
void DrawClockStatusBar(const wqn::HomeSummary& home);
void DrawProgressBar(int x, int y, int width, int height, int current, int total);
void DrawConfigBox(int x, int y, int width, int height, const std::string& value, const std::string& label, bool selected);
void DrawActionBox(int x, int y, int width, const std::string& label, bool selected);

// ---- Todo/Word cloud request/result types ----------------------------------

enum class TodoCloudOp {
    kRefresh,
    kComplete,
};

struct TodoCloudRequest {
    TodoCloudOp op = TodoCloudOp::kRefresh;
    char todo_id[64] = {};
    char cursor[160] = {};
};

struct TodoCloudResult {
    TodoCloudOp op = TodoCloudOp::kRefresh;
    esp_err_t result = ESP_FAIL;
    bool auth_required = false;
    char todo_id[64] = {};
    wqn::WqnTodoListPage page;
    wqn::WqnTodoItem todo;
};

enum class WordCloudOp {
    kPackSync,
    kStartSession,
    kFetchSessionPage,
    kSearch,
    kAiLookup,
};

struct WordCloudRequest {
    WordCloudOp op = WordCloudOp::kPackSync;
    char request_id[65] = {};
    char session_id[37] = {};
    char cursor[65] = {};
    uint16_t limit = 0;
    uint8_t study_mode = 0;
    char query[96] = {};
};

struct WordCloudResult {
    WordCloudOp op = WordCloudOp::kPackSync;
    esp_err_t result = ESP_FAIL;
    bool auth_required = false;
    bool pack_index_ready = false;
    wqn::WordPackIndex pack_index;
    wqn::protocol::word_study_v1::SessionData session;
    wqn::protocol::word_study_v1::CandidatePageData candidate_page;
    wqn::protocol::v3::Error protocol_error;
    wqn::WqnWordSearchResult search;
    wqn::WqnWordAiLookupResult lookup;
    std::string query;
    std::string message;
    // kStartSession: the compacted session snapshot, already persisted on the
    // runner thread so the 36 KB fsync never runs on the UI task. The apply
    // step only installs it in memory.
    wqn::PersistedWordSession persisted_session;
    esp_err_t session_compact_result = ESP_OK;
    esp_err_t session_persist_result = ESP_OK;
};

struct TodoCloudResultReady {
    uint32_t generation = 0;
};

struct WordCloudResultReady {
    uint32_t generation = 0;
};

enum class NoteCloudOp {
    kPackSync,
    kStartSession,
    kFetchSessionPage,
    kFetchImage,
    // Targeted single-notebook pack fetch: the user opened a note whose pack
    // is not on disk and is actively waiting, so it rides the interactive
    // lane (kPackSync stays the bulk full-catalog walk).
    kFetchNotebookPack,
    // Local storage write (observation outbox + session snapshot); needs no
    // network/token. Runs on the interactive lane so the note-open bookkeeping
    // leaves the UI task (the ~0.9s foreground commit stall).
    kCommitObservation,
};

struct NoteCloudRequest {
    NoteCloudOp op = NoteCloudOp::kPackSync;
    char request_id[65] = {};
    char session_id[37] = {};
    char cursor[65] = {};
    char notebook_id[37] = {};
    uint16_t limit = 0;
    // kFetchImage: which note/attachment and the content hash to verify.
    char note_id[37] = {};
    char image_id[65] = {};
    uint8_t image_index = 0;
    // kFetchImage / kFetchNotebookPack: identity of this dispatch in the
    // transfer-progress mailbox; the UI only consumes progress whose
    // generation matches its own record (stale-download defense).
    uint32_t progress_generation = 0;
};

struct NoteCloudResult {
    NoteCloudOp op = NoteCloudOp::kPackSync;
    esp_err_t result = ESP_FAIL;
    bool auth_required = false;
    bool pack_index_ready = false;
    wqn::NotePackIndex pack_index;
    wqn::protocol::note_study_v1::SessionData session;
    wqn::protocol::note_study_v1::CandidatePageData candidate_page;
    wqn::protocol::v3::Error protocol_error;
    std::string message;
    // kFetchImage: validated WQNI bytes (header + payload) and their id.
    std::string image_id;
    std::shared_ptr<const std::vector<uint8_t>> image_wqni;
    // kStartSession: compacted + already-persisted session snapshot (see
    // WordCloudResult); apply installs it without touching storage.
    wqn::PersistedNoteSession persisted_session;
    esp_err_t session_compact_result = ESP_OK;
    esp_err_t session_persist_result = ESP_OK;
};

struct NoteCloudResultReady {
    uint32_t generation = 0;
};

enum class ProblemCloudOp {
    kPackSync,
    kFetchImage,
    // Local storage write (verdict outbox append); needs no network/token.
    // Runs on the interactive lane so the durable append leaves the UI task.
    kCommitObservation,
};

struct ProblemCloudRequest {
    ProblemCloudOp op = ProblemCloudOp::kPackSync;
    // kFetchImage: which problem/attachment and the content hash to verify.
    char problem_id[37] = {};
    char image_id[65] = {};
    uint8_t image_index = 0;
    // 0 = assets, 1 = solution (the /v3 image route path segment).
    uint8_t image_kind = 0;
};

struct ProblemCloudResult {
    ProblemCloudOp op = ProblemCloudOp::kPackSync;
    esp_err_t result = ESP_FAIL;
    bool auth_required = false;
    bool pack_index_ready = false;
    wqn::ProblemPackIndex pack_index;
    std::string message;
    // kFetchImage: validated WQNI bytes (header + payload) and their id.
    std::string image_id;
    std::shared_ptr<const std::vector<uint8_t>> image_wqni;
};

struct ProblemCloudResultReady {
    uint32_t generation = 0;
};

// --- Unified cloud runner ---------------------------------------------------
// One executor replaces the three per-domain cloud tasks. Two lanes so bulk
// transfers (pack sync: multi-MB downloads + index rebuilds) can never sit in
// front of interactive work the user is actively waiting on (session start,
// candidate pages, images, todo refresh) -- the "late candidate result blocks
// the word page" class of stalls. Per-domain busy flags, sleep leases, result
// slots and generations keep their existing semantics; only the task/queue
// plumbing is shared.
enum class CloudDomain : uint8_t {
    kTodo,
    kWord,
    kNote,
    kProblem,
};

enum class CloudLane : uint8_t {
    kInteractive,
    kBulk,
};

struct CloudJob {
    CloudDomain domain = CloudDomain::kTodo;
    // Requests are PODs, so one job slot carries any domain's request.
    union {
        TodoCloudRequest todo;
        WordCloudRequest word;
        NoteCloudRequest note;
        ProblemCloudRequest problem;
    };
    CloudJob() : todo() {}
};

struct CloudResultReady {
    CloudDomain domain = CloudDomain::kTodo;
    uint32_t generation = 0;
};

esp_err_t StartCloudRunner();
bool EnqueueCloudJob(const CloudJob& job);
// [hang-fix] Busy-watch (see cloud_runner.cpp): armed on enqueue, cleared by
// Finish*CloudRequest; the UI loop polls WarnStuckCloudDomains so a domain
// stuck busy past its lane budget is loudly logged instead of dying silent.
void ClearCloudDomainBusyWatch(CloudDomain domain);
void WarnStuckCloudDomains();

// [transfer-progress] Single-writer atomic mailbox for interactive-lane
// downloads (note image / targeted notebook pack). The runner writes freely
// from the HTTP read loop; the UI task polls, quantizes and throttles before
// any e-ink repaint. Latest-value-wins semantics: no queue, no lock, torn
// reads are harmless after quantization. INTERACTIVE LANE ONLY -- the bulk
// lane runs concurrently and must never write here.
enum class CloudTransferKind : uint8_t {
    kNone = 0,
    kNoteImage = 1,
    kNotebookPack = 2,
};
struct CloudTransferSnapshot {
    CloudTransferKind kind = CloudTransferKind::kNone;
    uint32_t generation = 0;
    uint32_t done_bytes = 0;
    uint32_t total_bytes = 0;
};
void BeginCloudTransferProgress(CloudTransferKind kind, uint32_t generation);
void ReportCloudTransferBytes(uint32_t done_bytes, uint32_t total_bytes);
void EndCloudTransferProgress();
CloudTransferSnapshot ReadCloudTransferProgress();
extern QueueHandle_t g_cloud_result_queue;

static_assert(std::is_trivially_copyable_v<TodoCloudResultReady>);
static_assert(std::is_trivially_copyable_v<WordCloudResultReady>);
static_assert(std::is_trivially_copyable_v<NoteCloudResultReady>);
static_assert(std::is_trivially_copyable_v<CloudJob>);
static_assert(std::is_trivially_copyable_v<CloudResultReady>);

void SendTodoCloudResult();
void SendWordCloudResult();
void SendNoteCloudResult();
void SendProblemCloudResult();
const TodoCloudResult* PeekTodoCloudResult(uint32_t generation);
WordCloudResult* PeekWordCloudResult(uint32_t generation);
NoteCloudResult* PeekNoteCloudResult(uint32_t generation);
ProblemCloudResult* PeekProblemCloudResult(uint32_t generation);

bool IsTodoCloudBusy();
bool IsWordCloudBusy();
bool IsNoteCloudBusy();
bool IsProblemCloudBusy();
void FinishTodoCloudRequest();
void FinishWordCloudRequest();
void FinishNoteCloudRequest();
void FinishProblemCloudRequest();

bool QueueTodoCloudRequest(const TodoCloudRequest& request);
bool QueueWordCloudRequest(const WordCloudRequest& request);
bool QueueNoteCloudRequest(const NoteCloudRequest& request);
bool QueueProblemCloudRequest(const ProblemCloudRequest& request);

bool QueueTodoRefresh();
bool QueueTodoRefreshCursor(const std::string& cursor);
bool QueueTodoComplete(const std::string& todo_id);

bool QueueWordReviewRefresh();
bool QueueWordSessionStart(
    const wqn::protocol::word_study_v1::CreateSessionRequest& request);
bool QueueWordCandidatePage(
    const std::string& session_id,
    const wqn::protocol::word_study_v1::CandidatePageRequest& request);
void PumpWordCandidatePrefetch(UiRuntime* runtime);
bool QueueWordSearch(const wqn::WqnWordSearchRequest& search);
bool QueueWordAiLookup(const wqn::WqnWordAiLookupRequest& lookup);
// Rebuilds the note screen's [词] rows from word_app.deck_catalog, excluding
// the current default deck (it lives on the word page itself).
void RebuildNoteWordDeckRows(wqn::UiState* state);

bool QueueNotePackSync();
bool QueueNoteSessionStart(
    const wqn::protocol::note_study_v1::CreateSessionRequest& request);
bool QueueNoteCandidatePage(
    const std::string& session_id,
    const wqn::protocol::note_study_v1::CandidatePageRequest& request);
void PumpNoteCandidatePrefetch(UiRuntime* runtime);
bool QueueNoteImageFetch(
    const std::string& note_id, uint8_t image_index, const std::string& image_id,
    uint32_t progress_generation);
void PumpNoteImageFetch(UiRuntime* runtime);
bool QueueNoteBodyPackFetch(
    const std::string& notebook_id, uint32_t progress_generation);
void PumpNoteBodyPackFetch(UiRuntime* runtime);
void PumpNoteObservationCommit(UiRuntime* runtime);

bool QueueProblemPackSync();
bool QueueProblemImageFetch(
    const std::string& problem_id,
    bool is_solution,
    uint8_t image_index,
    const std::string& image_id);
void PumpProblemImageFetch(UiRuntime* runtime);
void PumpProblemVerdictCommit(UiRuntime* runtime);

bool ApplyTodoCloudResult(
    wqn::UiState* state,
    const TodoCloudResult& result,
    bool* content_changed = nullptr);
bool ApplyWordCloudResult(wqn::UiState* state, WordCloudResult& result);
bool ApplyNoteCloudResult(wqn::UiState* state, NoteCloudResult& result);
bool ApplyProblemCloudResult(wqn::UiState* state, ProblemCloudResult& result);

bool RefreshTodosFromCloud(wqn::UiState* state);
RefreshSchedule CompleteSelectedTodo(wqn::UiState* state);

void ExecuteTodoCloudRequest(const TodoCloudRequest& request);
void ExecuteWordCloudRequest(const WordCloudRequest& request);
void ExecuteNoteCloudRequest(const NoteCloudRequest& request);
void ExecuteProblemCloudRequest(const ProblemCloudRequest& request);

bool LoadValidTokenForTodo(std::string* token);

// ---- State / input ----------------------------------------------------------

bool LoadUiState(wqn::UiState* state);
void BuildHomeSummary(wqn::UiState* state);
RefreshSchedule QueueSelectedReview(wqn::UiState* state);

bool SameTimeAppState(const wqn::TimeAppState& a, const wqn::TimeAppState& b);
bool TimeAppStructureChanged(const wqn::TimeAppState& before, const wqn::TimeAppState& after);
bool ShouldRefreshTimeTick(const wqn::UiState& state);
bool ScreenUsesClockMinute(const wqn::UiState& state);

RefreshSchedule ApplyButtonEvent(
    const wqn::ButtonEvent& event,
    int64_t event_time_ms,
    wqn::UiState* state);

void OpenSettingsDialog(wqn::UiState* state, wqn::SettingsDialog dialog);

// ---- Render -----------------------------------------------------------------

esp_err_t RenderFrameToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);
wqn::display::DisplaySubmission RequestEpdUiRefresh(
    const wqn::UiFrame& frame,
    const std::string& signature,
    wqn::display::DisplayRevision revision,
    RefreshSchedule schedule,
    wqn::display::WaveformRequirement waveform);
void AcknowledgeDisplayResult(wqn::display::DisplayRevision revision);
void EpdRefreshTask(void*);
void DeviceUiTask(void*);

std::string FrameSignature(const wqn::UiFrame& frame);

// ---- Clock ------------------------------------------------------------------

std::time_t CurrentUnixTime();
bool SystemClockIsReasonable();
std::string CurrentClockLabel();
std::string CurrentDateLabel();
std::string CurrentIsoTimestamp();
void SeedClockFromBuildTimeIfNeeded();
const char* TimeTileTitle(wqn::TimeTile tile);
std::string ChooseHomePrimaryTimeLine(const wqn::TimeAppState& time_app);
void UpdateHomePrimaryTimeLine(wqn::UiState* state);
int CountReviewDueLikeProblems(const std::vector<wqn::CachedProblem>& problems);
int CountdownStartField();
int PomodoroStartField();
std::string TwoDigit(int value);

// ---- Graphics primitives ----------------------------------------------------

void DrawHorizontalLine(int x, int y, int width);
void DrawVerticalLine(int x, int y, int height);
void DrawRect(int x, int y, int width, int height);
void DrawRoundedRect(int x, int y, int width, int height, int radius);
void FillRect(int x, int y, int width, int height, bool black);
void ClearRect(const UiRect& rect);
// [L3-semantics] Named black-fill wrappers (defined in graphics.cpp). L2 migrates call sites.
void DrawSelectedFill(int x, int y, int width, int height);
void DrawProgressFill(int x, int y, int width, int height);
void DrawRoleBar(int x, int y, int width, int height);
void DrawActivityDot(int x, int y, int size);
void DrawWqnBitmapAsset(int x, int y, const WqnBitmapAsset& asset, bool black = true);
esp_err_t RefreshRegion(const UiRect& rect, RefreshSchedule schedule);
esp_err_t RefreshStableRegion(const UiRect& rect, RefreshSchedule schedule);
esp_err_t RefreshFrame(const wqn::UiFrame& frame, RefreshSchedule schedule);

esp_err_t DrawClippedText(int x, int y, int max_width, const std::string& text, bool black = true);
esp_err_t DrawCenteredText(int x, int y, int width, const std::string& text, bool black = true);
esp_err_t DrawWrappedText(int x, int y, int width, const std::string& text, int max_lines, bool black = true);
std::string LimitForEpd(const std::string& text);
std::string Utf8PageSlice(const std::string& text, size_t page, size_t chars_per_page);
std::string JoinAiFunctionCallSummaries(const std::vector<std::string>& summaries);
std::string FormatAiFunctionCallSummaries(const std::vector<std::string>& summaries);

// ---- Bitmap digits and status icons ----------------------------------------

void DrawStandbyClockDigits(int y, const std::string& text);
void DrawConfigDigitsCentered(int x, int y, int width, const std::string& ascii, bool black = true);
// Draw the running-timer duration with the 48px artistic digit font, centered.
void DrawTimerDigitsArt(int y, const std::string& ascii_duration);
// Draw the existing WiFi glyph only, right-aligned at right_edge.
int DrawWifiStatusIcon(int right_edge, int y, const wqn::HomeSummary& home);
// Draw [wifi][battery] status icons right-aligned at right_edge.
// Returns the x just left of the cluster (for drawing time/other text before it).
int DrawStatusBarIcons(int right_edge, int y, const wqn::HomeSummary& home);

// ---- Home page --------------------------------------------------------------

esp_err_t DrawMetricCard(int x, int y, int width, const wqn::HomeMetric& metric);
esp_err_t DrawHomeTaskRow(int x, int y, int width, int index, const wqn::HomeTask& task, bool selected);
esp_err_t RenderHomeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);
esp_err_t RenderHomePrimaryRegion(const wqn::HomeSummary& home, RefreshSchedule schedule);

// ---- Time page --------------------------------------------------------------

esp_err_t RenderClockStandbyContent();
esp_err_t RenderTimeClockRegion(RefreshSchedule schedule, bool include_date);
esp_err_t RenderCountdownConfigToEpd(const wqn::TimeAppState& time_app);
esp_err_t RenderPomodoroConfigToEpd(const wqn::TimeAppState& time_app);
int TimerInitialSeconds(const wqn::TimeAppState& time_app);
esp_err_t RenderTimerRunToEpd(const wqn::TimeAppState& time_app);
esp_err_t RenderTimerRunRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule);
esp_err_t RenderTimeConfigRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule);
esp_err_t RenderTimeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);

// ---- AI page ----------------------------------------------------------------

esp_err_t RenderAiToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);
const char* AiStatusLabel(wqn::AiSessionStatus status);

// ---- Word page --------------------------------------------------------------

esp_err_t RenderWordToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);

// ---- Note page --------------------------------------------------------------

esp_err_t RenderNoteToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);

// ---- Problem browse (hosted by the note page) -------------------------------

esp_err_t RenderProblemBrowseToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);

// ---- Settings page ----------------------------------------------------------

esp_err_t DrawSettingsRow(size_t row_index, int y, const std::string& title, const std::string& value, bool selected);
esp_err_t DrawSettingsDialogBox(const std::string& title);
esp_err_t DrawSettingsOptionCard(int x, int y, int width, const std::string& label, bool selected);
void DrawSettingsProgressBar(int x, int y, int width, int current, int total);
esp_err_t RenderSettingsDialog(const wqn::SettingsAppState& settings);
esp_err_t RenderSettingsToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);

// ---- Todo page --------------------------------------------------------------

void DrawDashedVerticalLine(int x, int y, int height);
void DrawTimelineNode(int cx, int cy, bool selected);
esp_err_t DrawTodoStatusBar(const wqn::TodoUiState& todo, const wqn::HomeSummary& home);
esp_err_t DrawTodoCard(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status, int x, int y, int width, int height);
esp_err_t DrawTodoEmptyState(const wqn::TodoUiState& todo);
esp_err_t RenderTodoToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule);
std::string TodoDueTimeLabel(const std::string& due_at);
std::string TodoDueDateLabel(const std::string& due_at);
const char* TodoSyncStatusText(wqn::TodoSyncStatus status);
std::string TodoItemStatusText(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status);
std::string TodoCardMetaLabel(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status);
std::string TodoStatusNote(const wqn::TodoUiState& todo);
int TodoPendingCount(const wqn::TodoUiState& todo);
int TodoOverdueCount(const wqn::TodoUiState& todo);
bool ParseTodoDueTime(const std::string& due_at, std::tm* due_tm);
size_t TodoVisibleStart(const wqn::TodoUiState& todo, size_t selected, size_t visible_count);

// ---- Extern globals (defined in ui_refresh.cpp) ----------------------------

extern SemaphoreHandle_t g_refresh_mutex;
extern TaskHandle_t g_refresh_task;
extern QueueHandle_t g_display_result_queue;
extern wqn::UiFrame g_pending_frames[2];
extern std::string g_pending_signatures[2];
extern wqn::display::DisplayIntent g_pending_intents[2];
extern std::atomic<int> g_consumer_index;
extern bool g_refresh_pending;
extern int g_rtc_screen_val;
extern TickType_t g_refresh_due_tick;
extern RefreshSchedule g_refresh_schedule;

struct SecondarySlot {
    wqn::UiFrame frame;
    std::string signature;
    wqn::display::DisplayIntent intent;
    RefreshSchedule schedule = RefreshSchedule::kNone;
    TickType_t due_tick = 0;
    bool pending = false;
};
extern SecondarySlot g_secondary;

extern volatile wqn::UiScreen g_last_rendered_screen;

}  // namespace device_ui_internal
