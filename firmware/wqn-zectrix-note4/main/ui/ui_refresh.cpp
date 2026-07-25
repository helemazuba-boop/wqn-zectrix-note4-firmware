// EPD refresh task: double-buffered frame slot, secondary slot for producer-during-render,
// promotion path, dedup. This is the file where the [fix epd-hang] lives.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <string>

#include "display_service.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "runtime/sleep_coordinator.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr int64_t kUiPollDelay = pdMS_TO_TICKS(50);
constexpr int64_t kUiIdlePollDelay = pdMS_TO_TICKS(500);
constexpr int64_t kStatusRefreshDelay = pdMS_TO_TICKS(60000);

// ---- Global refresh state (definitions live here) --------------------------

SemaphoreHandle_t g_refresh_mutex = nullptr;
TaskHandle_t g_refresh_task = nullptr;
QueueHandle_t g_display_result_queue = nullptr;
wqn::UiFrame g_pending_frames[2];
std::string g_pending_signatures[2];
wqn::display::DisplayIntent g_pending_intents[2];
std::atomic<int> g_consumer_index{0};
bool g_refresh_pending = false;
static bool g_refresh_busy = false;
TickType_t g_refresh_due_tick = 0;
RefreshSchedule g_refresh_schedule = RefreshSchedule::kNone;
SecondarySlot g_secondary;
volatile wqn::UiScreen g_last_rendered_screen = wqn::UiScreen::kHome;
wqn::runtime::SleepLease g_display_sleep_lease;
uint32_t g_outstanding_display_intents = 0;
std::array<wqn::display::DisplayRevision, wqn::display::kDisplayResultQueueDepth>
    g_outstanding_display_revisions{};
wqn::display::DisplayRevision g_last_presented_revision =
    wqn::display::kInvalidDisplayRevision;

// ---- Schedule helpers -------------------------------------------------------

int RefreshRank(RefreshSchedule schedule)
{
    switch (schedule) {
        case RefreshSchedule::kConfig:
            return 1;
        case RefreshSchedule::kAi:
            return 2;
        case RefreshSchedule::kClock:
            return 1;
        case RefreshSchedule::kSelection:
            return 2;
        case RefreshSchedule::kTimer:
            return 2;
        case RefreshSchedule::kCoalesced:
            return 2;
        case RefreshSchedule::kCommit:
            return 3;
        case RefreshSchedule::kImmediate:
            return 4;
        case RefreshSchedule::kNone:
        default:
            return 0;
    }
}

const char* RefreshScheduleName(RefreshSchedule schedule)
{
    switch (schedule) {
        case RefreshSchedule::kConfig:
            return "config";
        case RefreshSchedule::kAi:
            return "ai";
        case RefreshSchedule::kSelection:
            return "selection";
        case RefreshSchedule::kClock:
            return "clock";
        case RefreshSchedule::kTimer:
            return "timer";
        case RefreshSchedule::kCoalesced:
            return "coalesced";
        case RefreshSchedule::kCommit:
            return "commit";
        case RefreshSchedule::kImmediate:
            return "immediate";
        case RefreshSchedule::kNone:
        default:
            return "none";
    }
}

TickType_t RefreshDelay(RefreshSchedule schedule)
{
    switch (schedule) {
        case RefreshSchedule::kImmediate:
            return 0;
        case RefreshSchedule::kConfig:
            return kConfigRefreshDelay;
        case RefreshSchedule::kAi:
            return kAiRefreshDelay;
        case RefreshSchedule::kSelection:
            return kSelectionRefreshDelay;
        case RefreshSchedule::kClock:
            return kClockRefreshDelay;
        case RefreshSchedule::kTimer:
            return kTimerRefreshDelay;
        case RefreshSchedule::kCoalesced:
            return 0;
        case RefreshSchedule::kCommit:
            return kCommitRefreshDelay;
        case RefreshSchedule::kNone:
        default:
            return 0;
    }
}

bool TickReached(TickType_t now, TickType_t due)
{
    return static_cast<int32_t>(now - due) >= 0;
}

TickType_t TicksUntil(TickType_t now, TickType_t due)
{
    if (TickReached(now, due)) {
        return 0;
    }
    const TickType_t ticks = due - now;
    return ticks > 0 ? ticks : 1;
}

RefreshSchedule StrongerSchedule(RefreshSchedule a, RefreshSchedule b)
{
    return RefreshRank(a) >= RefreshRank(b) ? a : b;
}

namespace {

using wqn::display::DisplayIntent;
using wqn::display::DisplayResult;
using wqn::display::DisplayRevision;
using wqn::display::DisplayStatus;
using wqn::display::WaveformRequirement;

void MaybeReleaseDisplayLeaseLocked()
{
    if (!g_refresh_busy && !g_refresh_pending && !g_secondary.pending &&
        g_outstanding_display_intents == 0) {
        g_display_sleep_lease.Reset();
    }
}

bool HasOutstandingRevisionLocked(wqn::display::DisplayRevision revision)
{
    for (const wqn::display::DisplayRevision active : g_outstanding_display_revisions) {
        if (active == revision) {
            return true;
        }
    }
    return false;
}

bool TrackOutstandingRevisionLocked(wqn::display::DisplayRevision revision)
{
    if (revision == wqn::display::kInvalidDisplayRevision ||
        HasOutstandingRevisionLocked(revision)) {
        return false;
    }
    for (wqn::display::DisplayRevision& active : g_outstanding_display_revisions) {
        if (active == wqn::display::kInvalidDisplayRevision) {
            active = revision;
            ++g_outstanding_display_intents;
            return true;
        }
    }
    return false;
}

bool UntrackOutstandingRevisionLocked(wqn::display::DisplayRevision revision)
{
    for (wqn::display::DisplayRevision& active : g_outstanding_display_revisions) {
        if (active == revision) {
            active = wqn::display::kInvalidDisplayRevision;
            if (g_outstanding_display_intents > 0) {
                --g_outstanding_display_intents;
            }
            return true;
        }
    }
    return false;
}

uint32_t RefreshReasonMask(RefreshSchedule schedule)
{
    if (schedule == RefreshSchedule::kNone) {
        return 0;
    }
    return 1U << static_cast<uint8_t>(schedule);
}

constexpr WaveformRequirement StrongerWaveform(WaveformRequirement a, WaveformRequirement b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

constexpr TickType_t EarlierDeadline(TickType_t a, TickType_t b)
{
    return static_cast<int32_t>(a - b) <= 0 ? a : b;
}

constexpr bool HasMultipleReasons(uint32_t reasons)
{
    return reasons != 0 && (reasons & (reasons - 1U)) != 0;
}

static_assert(sizeof(TickType_t) == sizeof(uint32_t),
              "display deadlines require 32-bit FreeRTOS ticks");
static_assert(StrongerWaveform(WaveformRequirement::kPartial,
                              WaveformRequirement::kFull) ==
                  WaveformRequirement::kFull,
              "coalescing must never weaken the waveform");
static_assert(EarlierDeadline(static_cast<TickType_t>(100),
                              static_cast<TickType_t>(120)) == 100,
              "coalescing must retain the earlier deadline");
static_assert(EarlierDeadline(static_cast<TickType_t>(0xfffffff0U),
                              static_cast<TickType_t>(0x10U)) == 0xfffffff0U,
              "deadline ordering must remain correct across tick wrap");

bool PublishDisplayResult(const DisplayResult& result, TickType_t timeout)
{
    if (g_display_result_queue == nullptr ||
        xQueueSend(g_display_result_queue, &result, timeout) != pdTRUE) {
        ESP_LOGE(kTag,
                 "display result delivery failed: revision=%llu status=%d",
                 static_cast<unsigned long long>(result.revision),
                 static_cast<int>(result.status));
        return false;
    }
    return true;
}

DisplayResult SupersededResult(DisplayRevision revision, DisplayRevision replacement)
{
    DisplayResult result;
    result.revision = revision;
    result.status = DisplayStatus::kSuperseded;
    result.presented_revision = g_last_presented_revision;
    result.replacement_revision = replacement;
    result.error = ESP_OK;
    return result;
}

DisplayIntent NewDisplayIntent(
    DisplayRevision revision,
    RefreshSchedule schedule,
    TickType_t deadline,
    WaveformRequirement waveform)
{
    DisplayIntent intent;
    intent.revision = revision;
    intent.waveform = waveform;
    intent.deadline_tick = static_cast<uint32_t>(deadline);
    intent.reason_mask = RefreshReasonMask(schedule);
    return intent;
}

void MergeDisplayPolicy(
    const DisplayIntent& old_intent,
    wqn::UiFrame* latest_frame,
    DisplayIntent* latest_intent,
    TickType_t* latest_deadline)
{
    latest_intent->waveform = StrongerWaveform(old_intent.waveform, latest_intent->waveform);
    latest_intent->reason_mask |= old_intent.reason_mask;
    *latest_deadline = EarlierDeadline(static_cast<TickType_t>(old_intent.deadline_tick),
                                       *latest_deadline);
    latest_intent->deadline_tick = static_cast<uint32_t>(*latest_deadline);
    if (latest_intent->waveform == WaveformRequirement::kFull) {
        latest_frame->prefer_full_refresh = true;
    }
}

RefreshSchedule EffectiveSchedule(RefreshSchedule latest_schedule, const DisplayIntent& intent)
{
    // Local render paths return before RefreshFrame() sees frame.prefer_full_refresh.
    // Normalize a retained full-waveform requirement to kCommit so a newer
    // clock/config/selection intent cannot weaken an older safety requirement.
    if (intent.waveform == WaveformRequirement::kFull) {
        return RefreshSchedule::kCommit;
    }
    const uint32_t reasons = intent.reason_mask;
    if (HasMultipleReasons(reasons)) {
        return RefreshSchedule::kCoalesced;
    }
    return latest_schedule;
}

}  // namespace

// ---- Frame signature (used to dedup) ---------------------------------------

std::string FrameSignature(const wqn::UiFrame& frame)
{
    std::string signature = std::to_string(static_cast<int>(frame.screen));
    const bool time_config_mode = frame.screen == wqn::UiScreen::kTime && frame.time_app.config_mode;
    if (frame.screen == wqn::UiScreen::kHome) {
        signature.append("|home:");
        signature.append(frame.home.wifi_label);
        signature.push_back('/');
        signature.append(frame.home.battery_label);
        signature.push_back('/');
        signature.append(frame.home.primary_time_line);
        signature.push_back('/');
        signature.append(frame.home.review_metric.value);
        signature.push_back('/');
        signature.append(frame.home.todo_metric.value);
        signature.push_back('/');
        signature.append(frame.home.word_metric.value);
        signature.push_back('/');
        signature.append(frame.home.current_status);
        signature.push_back('/');
        signature.append(std::to_string(frame.selected_home_task));
        for (const wqn::HomeTask& task : frame.home.tasks) {
            signature.push_back('/');
            signature.append(task.title);
            signature.push_back(':');
            signature.append(task.subtitle);
            signature.push_back(':');
            signature.append(task.tag);
        }
    }
    if (frame.screen == wqn::UiScreen::kTime) {
        const wqn::TimeAppState& time_app = frame.time_app;
        signature.append("|time:");
        signature.append(std::to_string(static_cast<int>(time_app.tile)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(time_app.active_mode)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(time_app.status)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(time_app.pomodoro_phase)));
        signature.push_back('/');
        signature.append(time_app.config_mode ? "1" : "0");
        signature.push_back('/');
        signature.append(time_app.is_editing ? "1" : "0");
        signature.push_back('/');
        signature.append(std::to_string(time_app.active_field));
        signature.push_back('/');
        signature.append(std::to_string(time_app.remaining_seconds));
        signature.push_back('/');
        signature.append(std::to_string(time_app.countdown_hours));
        signature.push_back(':');
        signature.append(std::to_string(time_app.countdown_minutes));
        signature.push_back(':');
        signature.append(std::to_string(time_app.countdown_seconds));
        signature.push_back('/');
        signature.append(std::to_string(time_app.countdown_total_seconds));
        signature.push_back('/');
        signature.append(std::to_string(time_app.pomodoro_rounds));
        signature.push_back(':');
        signature.append(std::to_string(time_app.pomodoro_focus_minutes));
        signature.push_back(':');
        signature.append(std::to_string(time_app.pomodoro_break_minutes));
        signature.push_back(':');
        signature.append(std::to_string(time_app.pomodoro_long_break_minutes));
        signature.push_back('/');
        signature.append(std::to_string(time_app.pomodoro_current_round));
        signature.push_back('/');
        if (!time_config_mode) {
            signature.append(CurrentClockLabel());
        }
    }
    if (frame.screen == wqn::UiScreen::kAi) {
        signature.append("|ai:");
        // [sig-fix] Do NOT include frame.home.battery_label here: it changes
        // every minute when BuildHomeSummary refreshes the ADC reading, which
        // would trigger a signature change → refresh → full refresh (dirty
        // rect >85% on AI page) every minute. The AI page status bar is drawn
        // by DrawStatusBar which reads home.battery_label directly; it doesn't
        // need to be in the dedup signature.
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.ai.status)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.ai.tier)));
        signature.push_back('/');
        signature.append(frame.ai.user_text);
        signature.push_back('/');
        signature.append(frame.ai.assistant_text);
        signature.push_back('/');
        signature.append(frame.ai.pending_text);
        signature.push_back('/');
        signature.append(frame.ai.status_detail);
        signature.push_back('/');
        signature.append(frame.ai.conversation_id);
        signature.push_back('/');
        signature.append(std::to_string(frame.ai.page));
        // v2 chat scroll + toast: keep the signature unique across scroll
        // movements and toast label updates so the dedup pipeline does not
        // drop the redraw.
        signature.push_back('/');
        signature.append(std::to_string(frame.ai.scroll_offset_lines));
        signature.push_back('/');
        signature.append(frame.ai.toast_visible ? "1" : "0");
        signature.push_back('/');
        signature.append(frame.ai.toast_label);
        // [scroll-hint] Track the latest no-op Down press so the renderer
        // can flash a "已最新" hint at the bottom for a brief moment.
        signature.push_back('/');
        signature.append(std::to_string(frame.ai.scroll_no_op_hint_ms));
        for (const std::string& summary : frame.ai.function_call_summaries) {
            signature.push_back('/');
            signature.append(summary);
        }
        // [shell] status-bar edit mode + toggle values: MUST be in the signature
        // so entering/exiting edit mode and cycling toggles changes the sig and
        // triggers a refresh (otherwise the dedup pipeline skips the redraw).
        signature.push_back('/');
        signature.append(frame.status_edit.active ? "1" : "0");
        signature.push_back('/');
        signature.append(std::to_string(frame.status_edit.selected));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.ai.thinking_level)));
        signature.push_back('/');
        signature.append(frame.ai.tts_on ? "1" : "0");
        signature.push_back('/');
        signature.append(frame.ai.expand_content ? "1" : "0");
        signature.push_back('/');
        signature.append(std::to_string(frame.ai_history_revision));
    }
    if (frame.screen == wqn::UiScreen::kTodo) {
        signature.append("|todo:");
        signature.append(std::to_string(static_cast<int>(frame.todo.sync_status)));
        signature.push_back('/');
        signature.append(std::to_string(frame.todo.selected));
        signature.push_back('/');
        signature.append(std::to_string(frame.todo.total_pending));
        signature.push_back('/');
        signature.append(frame.todo.status_message);
        signature.push_back('/');
        signature.append(CurrentClockLabel());
        signature.push_back('/');
        signature.append(frame.home.wifi_label);
        signature.push_back('/');
        signature.append(frame.home.battery_label);
        for (const wqn::WqnTodoItem& item : frame.todo.todos) {
            signature.push_back('/');
            signature.append(item.id);
            signature.push_back(':');
            signature.append(item.title);
            signature.push_back(':');
            signature.append(item.status);
            signature.push_back(':');
            signature.append(item.due_at);
            signature.push_back(':');
            signature.append(item.subject_name);
        }
    }
    if (frame.screen == wqn::UiScreen::kWord) {
        signature.append("|word:");
        signature.append(std::to_string(static_cast<int>(frame.word_app.mode)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.word_app.card_phase)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.word_app.card_source)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.word_app.dictionary_stage)));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.word_app.home_selection)));
        signature.push_back('/');
        signature.append(
            frame.word_app.sequential_session_resumable ? "1" : "0");
        signature.append(
            frame.word_app.random_session_resumable ? "1" : "0");
        signature.push_back('/');
        signature.append(std::to_string(frame.word_app.card_position));
        signature.push_back('/');
        signature.append(std::to_string(frame.word_app.card_count));
        signature.push_back('/');
        signature.append(std::to_string(frame.word_app.reviewed_today));
        signature.push_back('/');
        signature.append(frame.word_app.word);
        signature.push_back('/');
        signature.append(frame.word_app.meaning);
        signature.push_back('/');
        signature.append(frame.word_app.dictionary_prefix);
        signature.push_back('/');
        signature.append(std::to_string(frame.word_app.dictionary_letter_selected));
        signature.push_back('/');
        signature.append(std::to_string(frame.word_app.dictionary_match_selected));
        signature.push_back('/');
        signature.append(frame.word_app.hint);
    }
    if (frame.screen == wqn::UiScreen::kNote) {
        // The note page renders entirely from frame.note_app (RenderNote adds no
        // frame.lines), so every field that changes the drawn view must be in the
        // signature or the dedup pipeline skips the repaint and the page looks
        // frozen. Body text is identified by note_id (+ scroll offset) rather than
        // hashing the full 16 KB body each frame.
        const wqn::NoteAppSnapshot& note = frame.note_app;
        signature.append("|note:");
        signature.append(std::to_string(static_cast<int>(note.mode)));
        signature.push_back('/');
        signature.append(std::to_string(note.notebook_selected));
        signature.push_back('/');
        signature.append(std::to_string(note.note_list_selected));
        signature.push_back('/');
        signature.append(std::to_string(note.notebook_window_start));
        signature.push_back('/');
        signature.append(std::to_string(note.note_list_window_start));
        signature.push_back('/');
        signature.append(std::to_string(note.note_scroll_offset_lines));
        signature.push_back('/');
        signature.append(note.has_body ? "1" : "0");
        signature.push_back('/');
        signature.append(note.cloud_sync_failed ? "1" : "0");
        signature.push_back('/');
        signature.append(std::to_string(note.notebook_count));
        signature.push_back('/');
        signature.append(note.notebook_title);
        signature.push_back('/');
        signature.append(note.note_title);
        signature.push_back('/');
        signature.append(note.note_id);
        signature.push_back('/');
        signature.append(note.status_line);
        signature.push_back('/');
        signature.append(note.hint);
        for (const wqn::NoteNotebookRow& row : note.notebooks) {
            signature.push_back('/');
            signature.append(row.title);
            signature.push_back(':');
            signature.append(std::to_string(row.note_count));
            signature.push_back(':');
            signature.append(row.has_pack ? "1" : "0");
        }
        for (const wqn::NoteTitleRow& row : note.titles) {
            signature.push_back('|');
            signature.append(row.title);
            signature.push_back(':');
            signature.append(row.last_opened_at);
        }
    }
    if (frame.screen == wqn::UiScreen::kSettings) {
        const wqn::SettingsDiagnosticsSnapshot& diag = frame.settings.diagnostics;
        signature.append("|settings:");
        signature.append(std::to_string(frame.settings.selected));
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.settings.dialog)));
        signature.push_back('/');
        signature.append(std::to_string(frame.settings.auto_sync_selected));
        signature.push_back('/');
        signature.append(std::to_string(frame.settings.auto_sync_interval_min));
        signature.push_back('/');
        signature.append(std::to_string(frame.settings.volume_percent));
        signature.push_back('/');
        signature.append(std::to_string(frame.settings.volume_selected));
        signature.push_back('/');
        signature.append(frame.settings.sync_status);
        signature.push_back('/');
        signature.append(frame.settings.notice);
        signature.push_back('/');
        signature.append(frame.paired ? "paired" : "unpaired");
        signature.push_back('/');
        signature.append(frame.claim_code);
        signature.push_back('/');
        signature.append(std::to_string(diag.flash_size));
        signature.push_back('/');
        signature.append(std::to_string(diag.nvs_total_entries));
        signature.push_back('/');
        signature.append(std::to_string(diag.psram_total));
        signature.push_back('/');
        signature.append(diag.firmware_version);
        signature.push_back('/');
        signature.append(frame.home.wifi_label);
        signature.push_back('/');
        signature.append(WQN_FIRMWARE_VERSION);
    }
    for (const wqn::UiLine& line : frame.lines) {
        signature.push_back('|');
        signature.append(std::to_string(static_cast<int>(line.style)));
        signature.push_back(':');
        signature.append(line.text);
    }
    return signature;
}

// ---- Producer-side: enqueue a new frame -------------------------------------

wqn::display::DisplaySubmission RequestEpdUiRefresh(
    const wqn::UiFrame& frame,
    const std::string& signature,
    wqn::display::DisplayRevision revision,
    RefreshSchedule schedule,
    wqn::display::WaveformRequirement waveform)
{
    wqn::display::DisplaySubmission submission;
    if (g_refresh_mutex == nullptr || g_refresh_task == nullptr ||
        g_display_result_queue == nullptr || schedule == RefreshSchedule::kNone ||
        revision == wqn::display::kInvalidDisplayRevision) {
        ESP_LOGW(kTag, "RequestEpdUiRefresh rejected: prereqs (mutex=%p task=%p schedule=%s revision=%llu)",
                 g_refresh_mutex, g_refresh_task, RefreshScheduleName(schedule),
                 static_cast<unsigned long long>(revision));
        return submission;
    }

    const TickType_t now = xTaskGetTickCount();
    TickType_t due_tick = now + RefreshDelay(schedule);

    xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
    if (HasOutstandingRevisionLocked(revision)) {
        xSemaphoreGive(g_refresh_mutex);
        ESP_LOGE(kTag, "RequestEpdUiRefresh rejected: duplicate revision=%llu",
                 static_cast<unsigned long long>(revision));
        return submission;
    }
    if (g_outstanding_display_intents >= wqn::display::kDisplayResultQueueDepth) {
        xSemaphoreGive(g_refresh_mutex);
        ESP_LOGW(kTag, "RequestEpdUiRefresh deferred: result ledger full (%u)",
                 static_cast<unsigned>(g_outstanding_display_intents));
        return submission;
    }
    if (!g_display_sleep_lease) {
        wqn::runtime::SleepLease lease =
            wqn::runtime::SleepLease::TryAcquire(
                wqn::runtime::SleepBlocker::kDisplay, "display-refresh", __FILE__, __LINE__);
        if (!lease) {
            xSemaphoreGive(g_refresh_mutex);
            ESP_LOGI(kTag, "RequestEpdUiRefresh deferred: sleep quiesce in progress");
            return submission;
        }
        g_display_sleep_lease = std::move(lease);
    }

    wqn::UiFrame merged_frame = frame;
    wqn::display::DisplayIntent merged_intent =
        NewDisplayIntent(revision, schedule, due_tick, waveform);

    if (g_refresh_busy) {
        // Consumer is rendering: the main path buffer cannot be written without breaking
        // the in-flight frame. The newest pixels replace the secondary intent, while safety
        // policy is monotonic: never weaken its waveform and never postpone its deadline.
        if (g_secondary.pending) {
            MergeDisplayPolicy(g_secondary.intent,
                               &merged_frame, &merged_intent, &due_tick);
            if (!PublishDisplayResult(
                    SupersededResult(g_secondary.intent.revision, revision), 0)) {
                xSemaphoreGive(g_refresh_mutex);
                return submission;
            }
        }
        const RefreshSchedule effective_schedule =
            EffectiveSchedule(schedule, merged_intent);
        ESP_LOGI(kTag,
                 "display intent accepted: revision=%llu path=secondary requested=%s effective=%s deadline=%u reasons=0x%lx waveform=%d sig_len=%zu",
                 static_cast<unsigned long long>(revision), RefreshScheduleName(schedule),
                 RefreshScheduleName(effective_schedule),
                 static_cast<unsigned>(due_tick),
                 static_cast<unsigned long>(merged_intent.reason_mask),
                 static_cast<int>(merged_intent.waveform), signature.size());
        g_secondary.frame = std::move(merged_frame);
        g_secondary.signature = signature;
        g_secondary.intent = merged_intent;
        g_secondary.schedule = effective_schedule;
        g_secondary.due_tick = due_tick;
        g_secondary.pending = true;
        if (!TrackOutstandingRevisionLocked(revision)) {
            g_secondary.pending = false;
            xSemaphoreGive(g_refresh_mutex);
            ESP_LOGE(kTag, "display outstanding ledger full: revision=%llu",
                     static_cast<unsigned long long>(revision));
            return submission;
        }
        xSemaphoreGive(g_refresh_mutex);
        submission.accepted = true;
        submission.revision = revision;
        submission.waveform = merged_intent.waveform;
        submission.deadline_tick = merged_intent.deadline_tick;
        return submission;
    }

    // Main path: if a not-yet-started primary intent exists, it is superseded. Keep the
    // newest pixels and drawing schedule, but merge the old safety/latency constraints.
    const int consumer_holds = g_consumer_index.load(std::memory_order_acquire);
    const int producer_slot = 1 - consumer_holds;
    if (g_refresh_pending) {
        MergeDisplayPolicy(g_pending_intents[consumer_holds],
                           &merged_frame, &merged_intent, &due_tick);
        if (!PublishDisplayResult(
                SupersededResult(g_pending_intents[consumer_holds].revision, revision), 0)) {
            xSemaphoreGive(g_refresh_mutex);
            return submission;
        }
    }
    const RefreshSchedule effective_schedule =
        EffectiveSchedule(schedule, merged_intent);
    ESP_LOGI(kTag,
             "display intent accepted: revision=%llu path=primary requested=%s effective=%s deadline=%u reasons=0x%lx waveform=%d sig_len=%zu slot=%d",
             static_cast<unsigned long long>(revision), RefreshScheduleName(schedule),
             RefreshScheduleName(effective_schedule),
             static_cast<unsigned>(due_tick),
             static_cast<unsigned long>(merged_intent.reason_mask),
             static_cast<int>(merged_intent.waveform), signature.size(), producer_slot);
    g_pending_frames[producer_slot] = std::move(merged_frame);
    g_pending_signatures[producer_slot] = signature;
    g_pending_intents[producer_slot] = merged_intent;
    g_refresh_pending = true;
    g_refresh_schedule = effective_schedule;
    g_refresh_due_tick = due_tick;
    if (!TrackOutstandingRevisionLocked(revision)) {
        g_refresh_pending = false;
        xSemaphoreGive(g_refresh_mutex);
        ESP_LOGE(kTag, "display outstanding ledger full: revision=%llu",
                 static_cast<unsigned long long>(revision));
        return submission;
    }
    // Index flip must happen inside the lock, atomic with pending/schedule, to avoid
    // an idx vs busy reorder race with the consumer promotion critical section.
    g_consumer_index.store(producer_slot, std::memory_order_release);
    xSemaphoreGive(g_refresh_mutex);
    xTaskNotifyGive(g_refresh_task);
    taskYIELD();
    submission.accepted = true;
    submission.revision = revision;
    submission.waveform = merged_intent.waveform;
    submission.deadline_tick = merged_intent.deadline_tick;
    return submission;
}

void AcknowledgeDisplayResult(wqn::display::DisplayRevision revision)
{
    if (g_refresh_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
    if (!UntrackOutstandingRevisionLocked(revision)) {
        ESP_LOGE(kTag, "display result ack for unknown revision=%llu outstanding=%u",
                 static_cast<unsigned long long>(revision),
                 static_cast<unsigned>(g_outstanding_display_intents));
    }
    MaybeReleaseDisplayLeaseLocked();
    xSemaphoreGive(g_refresh_mutex);
}

// ---- Consumer-side: EPD render task -----------------------------------------

void EpdRefreshTask(void*)
{
    ESP_LOGI(kTag, "EPD refresh task started");

#if CONFIG_ESP_TASK_WDT_EN
    // Try add then delete: if the task was subscribed to TWDT at creation (FreeRTOS default),
    // delete here takes effect. If add fails the task is not subscribed; delete also fails — fine either way.
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
    ESP_LOGI(kTag, "EPD refresh task unsubscribed from task watchdog");
#endif

    {
        const UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
        ESP_LOGI(
            kTag,
            "EPD refresh task initial stack HWM: %u bytes free",
            static_cast<unsigned>(stack_high_water * sizeof(StackType_t)));
    }

    std::string displayed_signature;
    while (true) {
        ESP_LOGD(kTag, "EPD refresh waiting on notify");
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGD(kTag, "EPD refresh notify received");

        while (true) {
            TickType_t wait_ticks = 0;
            xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
            if (!g_refresh_pending) {
                g_refresh_schedule = RefreshSchedule::kNone;
                MaybeReleaseDisplayLeaseLocked();
                xSemaphoreGive(g_refresh_mutex);
                wait_ticks = portMAX_DELAY;
            } else {
                wait_ticks = TicksUntil(xTaskGetTickCount(), g_refresh_due_tick);
                xSemaphoreGive(g_refresh_mutex);
            }

            if (wait_ticks == 0) {
                break;
            }

            ulTaskNotifyTake(pdTRUE, wait_ticks);
        }

        // Critical section: copy only the signature (24 bytes SSO); the frame is const-reffed
        // outside the lock (because g_consumer_index.store is inside the lock, the frame is
        // guaranteed not to be overwritten while the consumer renders it).
        std::string local_sig;
        wqn::display::DisplayIntent local_intent;
        RefreshSchedule schedule = RefreshSchedule::kNone;
        int consume_idx = 0;
        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        if (!g_refresh_pending) {
            MaybeReleaseDisplayLeaseLocked();
            xSemaphoreGive(g_refresh_mutex);
            continue;
        }
        consume_idx = g_consumer_index.load(std::memory_order_acquire);
        schedule = g_refresh_schedule;
        local_sig = g_pending_signatures[consume_idx];
        local_intent = g_pending_intents[consume_idx];
        g_refresh_pending = false;
        g_refresh_schedule = RefreshSchedule::kNone;
        g_refresh_due_tick = 0;
        g_refresh_busy = true;
        xSemaphoreGive(g_refresh_mutex);

        // const-ref outside the lock (producer can only write secondary; promotion only touches the opposite slot)
        const wqn::UiFrame& frame = g_pending_frames[consume_idx];

        esp_err_t render_result = ESP_OK;
        const bool can_deduplicate =
            local_sig == displayed_signature &&
            local_intent.waveform != wqn::display::WaveformRequirement::kFull;
        if (!can_deduplicate) {
            const int64_t refresh_start_us = esp_timer_get_time();
            render_result = RenderFrameToEpd(frame, schedule);
            const int64_t refresh_elapsed_ms = (esp_timer_get_time() - refresh_start_us) / 1000;
            if (render_result == ESP_OK) {
                displayed_signature = local_sig;
                g_last_rendered_screen = frame.screen;
                wqn::NoteEpdActivity();
                ESP_LOGI(
                    kTag,
                    "display presented: revision=%llu schedule=%s elapsed_ms=%lld",
                    static_cast<unsigned long long>(local_intent.revision),
                    RefreshScheduleName(schedule),
                    static_cast<long long>(refresh_elapsed_ms));
            } else {
                ESP_LOGW(
                    kTag,
                    "display failed: revision=%llu error=%s",
                    static_cast<unsigned long long>(local_intent.revision),
                    esp_err_to_name(render_result));
            }
        } else {
            ESP_LOGI(
                kTag,
                "display presented by dedup: revision=%llu",
                static_cast<unsigned long long>(local_intent.revision));
        }

        wqn::display::DisplayResult terminal_result;
        terminal_result.revision = local_intent.revision;
        terminal_result.error = render_result;
        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        if (render_result == ESP_OK) {
            g_last_presented_revision = local_intent.revision;
            terminal_result.status = wqn::display::DisplayStatus::kPresented;
        } else {
            terminal_result.status = wqn::display::DisplayStatus::kFailed;
        }
        terminal_result.presented_revision = g_last_presented_revision;
        xSemaphoreGive(g_refresh_mutex);
        // A result slot was reserved when the intent was accepted. The refresh
        // task may wait for the UI to drain it, preserving the exactly-once
        // terminal-result invariant instead of silently dropping failures.
        PublishDisplayResult(terminal_result, portMAX_DELAY);

        // After render: check secondary slot (producer may have submitted during render).
        // Move it into the opposite primary slot and self-notify.
        // The store(new_slot) must happen inside the lock, atomic with pending/busy, to keep
        // the new idx and the promotion state visible to the consumer's next loop iteration
        // without risking access to a slot we are mid-move on.
        bool do_promote = false;
        RefreshSchedule promote_sched = RefreshSchedule::kNone;
        int new_slot = 0;
        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        do_promote = g_secondary.pending;
        if (do_promote) {
            const int consumer_holds = g_consumer_index.load(std::memory_order_relaxed);
            new_slot = 1 - consumer_holds;
            promote_sched = g_secondary.schedule;
            g_pending_frames[new_slot] = std::move(g_secondary.frame);
            g_pending_signatures[new_slot] = std::move(g_secondary.signature);
            g_pending_intents[new_slot] = g_secondary.intent;
            g_refresh_schedule = g_secondary.schedule;
            g_refresh_due_tick = g_secondary.due_tick;
            g_secondary.intent = {};
            g_secondary.schedule = RefreshSchedule::kNone;
            g_secondary.due_tick = 0;
            g_secondary.pending = false;
            g_refresh_pending = true;
            g_refresh_busy = true;          // keep busy: about to render again
            // Index flip inside the lock, atomic with promotion state, visible to the consumer.
            g_consumer_index.store(new_slot, std::memory_order_release);
        } else {
            g_refresh_busy = false;
            MaybeReleaseDisplayLeaseLocked();
        }
        xSemaphoreGive(g_refresh_mutex);
        if (do_promote) {
            ESP_LOGI(
                kTag,
                "EPD refresh promoting secondary: primary_slot=%d schedule=%s",
                new_slot,
                RefreshScheduleName(promote_sched));
            xTaskNotifyGive(g_refresh_task);
        }
    }
}

}  // namespace device_ui_internal
