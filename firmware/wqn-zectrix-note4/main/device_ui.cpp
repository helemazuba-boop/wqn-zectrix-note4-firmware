// Device UI entry points: StartDeviceUiIfEnabled() + DeviceUiTask().
// All other responsibilities (rendering, refresh, cloud, state, input) live in main/ui/.

#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include "ai_session.h"
#include "button_input.h"
#include "config.h"
#include "device_ui.h"
#include "display_service.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flash_session.h"
#include "power_manager.h"
#include "runtime/wake_context.h"
#include "services/sync_service.h"
#include "ui/ui_internal.h"
#include "ui/ui_runtime.h"
#include "ui_model.h"
#include "wqn_api.h"

namespace device_ui_internal {
// Last screen survives deep sleep via RTC slow memory. Initialized to kHome (=3)
// rather than 0 so cold boot / RTC corruption can't accidentally land on kAi.
// LoadUiState() validates the restored value before use.
RTC_DATA_ATTR int g_rtc_screen_val = static_cast<int>(wqn::UiScreen::kHome);

// [timer-skip] Last successfully-rendered (screen, clock-label) pair, stored
// in RTC slow memory. On a timer wake-up we compare the freshly-rendered
// frame's clock label + screen against these and skip the entire refresh
// pipeline if both match -- the panel already shows this content.
// Initialized to a sentinel screen value so the first wake-up after a cold
// boot / RTC reset is never mistakenly skipped.
RTC_DATA_ATTR int g_rtc_last_rendered_screen_id = -1;
RTC_DATA_ATTR char g_rtc_last_rendered_clock[8] = {};
} // namespace device_ui_internal

namespace {

// [timer-skip] Compute the screen-id that should participate in the
// timer-wakeup skip decision. Configurable / editing screens must never be
// skipped because their state can change between refreshes without touching
// the clock label.
bool ScreenSupportsTimerSkip(wqn::UiScreen screen)
{
    return screen == wqn::UiScreen::kHome ||
           screen == wqn::UiScreen::kTime ||
           screen == wqn::UiScreen::kWord ||
           screen == wqn::UiScreen::kTodo;
}

bool TrySkipInitRefresh(wqn::UiScreen screen, const char* clock_label)
{
    using device_ui_internal::g_rtc_last_rendered_screen_id;
    using device_ui_internal::g_rtc_last_rendered_clock;
    const wqn::runtime::WakeContext& wake = wqn::runtime::GetWakeContext();
    ESP_LOGI("wqn_ui",
             "TrySkipInitRefresh: wake=%s raw=%d ext1=0x%llx cache=%d screen=%d clock=%s "
             "rtc_screen=%d rtc_clock=\"%s\"",
             wqn::runtime::WakeKindName(wake.kind), static_cast<int>(wake.raw_cause),
             static_cast<unsigned long long>(wake.ext1_status), wake.panel_cache_trusted ? 1 : 0,
             static_cast<int>(screen), clock_label ? clock_label : "(null)",
             g_rtc_last_rendered_screen_id, g_rtc_last_rendered_clock);
    if (wake.kind != wqn::runtime::WakeKind::kScheduledTimer || !wake.panel_cache_trusted) {
        return false;
    }
    if (!ScreenSupportsTimerSkip(screen)) {
        return false;
    }
    if (clock_label == nullptr || clock_label[0] == '\0') {
        return false;
    }
    if (g_rtc_last_rendered_screen_id < 0) {
        // True cold boot / RTC never programmed -- nothing valid to compare.
        return false;
    }
    if (static_cast<int>(screen) != g_rtc_last_rendered_screen_id) {
        return false;
    }
    if (std::strcmp(clock_label, g_rtc_last_rendered_clock) != 0) {
        return false;
    }
    ESP_LOGI("wqn_ui", "Init refresh skipped: panel already shows this exact frame (screen=%d clock=%s wake=%s ext1=0x%llx)",
             static_cast<int>(screen), clock_label, wqn::runtime::WakeKindName(wake.kind),
             static_cast<unsigned long long>(wake.ext1_status));
    return true;
}

void RecordPresentedRefresh(wqn::UiScreen screen, const char* clock_label)
{
    using device_ui_internal::g_rtc_last_rendered_screen_id;
    using device_ui_internal::g_rtc_last_rendered_clock;
    if (!ScreenSupportsTimerSkip(screen) || clock_label == nullptr || clock_label[0] == '\0') {
        return;
    }
    g_rtc_last_rendered_screen_id = static_cast<int>(screen);
    std::strncpy(g_rtc_last_rendered_clock, clock_label, sizeof(g_rtc_last_rendered_clock) - 1);
    g_rtc_last_rendered_clock[sizeof(g_rtc_last_rendered_clock) - 1] = '\0';
}

// [timer-skip] Main-loop minute-tick gate. Returns true when the minute has
// not advanced since the last successful refresh and ScreenUsesClockMinute
// would otherwise issue a kClock schedule. By suppressing the schedule here
// we avoid the full-refresh forced by g_previous_framebuffer_synced=false
// after every deep-sleep wake-up.
bool ShouldSkipMinuteTickRefresh(wqn::UiScreen screen, const char* clock_label)
{
    using device_ui_internal::g_rtc_last_rendered_screen_id;
    using device_ui_internal::g_rtc_last_rendered_clock;
    if (clock_label == nullptr || clock_label[0] == '\0') {
        return false;
    }
    if (!ScreenSupportsTimerSkip(screen)) {
        return false;
    }
    if (g_rtc_last_rendered_screen_id < 0) {
        return false;
    }
    if (static_cast<int>(screen) != g_rtc_last_rendered_screen_id) {
        return false;
    }
    return std::strcmp(clock_label, g_rtc_last_rendered_clock) == 0;
}

constexpr char kTag[] = "wqn_ui";
constexpr TickType_t kUiPollDelayTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kUiIdlePollDelayTicks = pdMS_TO_TICKS(500);
constexpr TickType_t kStatusRefreshDelayTicks = pdMS_TO_TICKS(60000);
// Full UI snapshot reloads touch several persistent domains. Keep them out of
// the word interaction window; typed sync events already update live status.
constexpr int64_t kStatusReloadInteractionQuietUs = 5LL * 1000LL * 1000LL;
constexpr UBaseType_t kSyncEventQueueDepth = 4;

using device_ui_internal::BuildHomeSummary;
using device_ui_internal::CheckBatteryProtection;
using device_ui_internal::CurrentClockLabel;
using device_ui_internal::FrameSignature;
using device_ui_internal::RefreshSchedule;
using device_ui_internal::RequestEpdUiRefresh;
using device_ui_internal::SeedClockFromBuildTimeIfNeeded;
using device_ui_internal::StrongerSchedule;
using device_ui_internal::AcknowledgeDisplayResult;
using device_ui_internal::FinishTodoCloudRequest;
using device_ui_internal::FinishWordCloudRequest;
using device_ui_internal::FinishNoteCloudRequest;
using device_ui_internal::g_todo_result_queue;
using device_ui_internal::g_display_result_queue;
using device_ui_internal::g_rtc_screen_val;
using device_ui_internal::g_word_result_queue;
using device_ui_internal::g_note_result_queue;

// Local copy — only DeviceUiTask reads/writes this; ui_refresh.cpp's
// g_last_active_us is the cross-file symbol declared in ui_internal.h
// (currently only forwarded, no definition site yet).
int64_t g_last_active_us_local = 0;

struct SubmittedDisplayRecord {
    bool occupied = false;
    wqn::display::DisplayRevision revision = wqn::display::kInvalidDisplayRevision;
    std::string signature;
    wqn::UiScreen screen = wqn::UiScreen::kHome;
    char clock_label[8] = {};
    RefreshSchedule schedule = RefreshSchedule::kNone;
    bool full_refresh = false;
};

struct DisplayTrackingState {
    std::array<SubmittedDisplayRecord, wqn::display::kDisplayResultQueueDepth> records;
    std::string desired_signature;
    std::string submitted_signature;
    std::string presented_signature;
    wqn::display::DisplayRevision submitted_revision =
        wqn::display::kInvalidDisplayRevision;
    wqn::display::DisplayRevision presented_revision =
        wqn::display::kInvalidDisplayRevision;
    RefreshSchedule submitted_schedule = RefreshSchedule::kNone;
    bool submitted_full_refresh = false;
    bool force_next_submission = false;
};

// This ledger is bounded but contains 16 std::string-bearing records. Keeping
// it inside DeviceUiTask inflated that task's fixed stack frame beyond 5 KiB;
// LoadUiState() then nested into the word-pack SHA verifier (1 KiB local read
// buffer) and overflowed the original 8 KiB task stack, corrupting heap TLSF
// metadata. The UI has one owner/task, so static storage is the correct lifetime.
DisplayTrackingState g_display_tracking;
device_ui_internal::UiRuntime g_ui_runtime;
wqn::AppState g_ui_reload_snapshot;
StaticQueue_t g_sync_event_queue_storage;
uint8_t g_sync_event_queue_buffer[
    kSyncEventQueueDepth * sizeof(wqn::services::SyncEvent)] = {};
QueueHandle_t g_sync_event_queue = nullptr;

void UiSyncEventSink(const wqn::services::SyncEvent& event)
{
    if (g_sync_event_queue == nullptr ||
        xQueueSend(g_sync_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(kTag, "drop sync event: sequence=%lu",
                 static_cast<unsigned long>(event.sequence));
    }
}

void LogUiStackHighWater(const char* phase)
{
    const UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(kTag, "UI stack HWM: phase=%s free_bytes=%u",
             phase,
             static_cast<unsigned>(high_water_words * sizeof(StackType_t)));
}

SubmittedDisplayRecord* FindSubmittedRecord(
    DisplayTrackingState* tracking,
    wqn::display::DisplayRevision revision)
{
    for (SubmittedDisplayRecord& record : tracking->records) {
        if (record.occupied && record.revision == revision) {
            return &record;
        }
    }
    return nullptr;
}

void RecomputeLatestSubmission(DisplayTrackingState* tracking)
{
    SubmittedDisplayRecord* latest = nullptr;
    for (SubmittedDisplayRecord& record : tracking->records) {
        if (record.occupied && (latest == nullptr || record.revision > latest->revision)) {
            latest = &record;
        }
    }
    if (latest == nullptr) {
        tracking->submitted_revision = wqn::display::kInvalidDisplayRevision;
        tracking->submitted_signature.clear();
        tracking->submitted_schedule = RefreshSchedule::kNone;
        tracking->submitted_full_refresh = false;
        return;
    }
    tracking->submitted_revision = latest->revision;
    tracking->submitted_signature = latest->signature;
    tracking->submitted_schedule = latest->schedule;
    tracking->submitted_full_refresh = latest->full_refresh;
}

bool TrackDisplaySubmission(
    DisplayTrackingState* tracking,
    const wqn::display::DisplaySubmission& submission,
    const std::string& signature,
    wqn::UiScreen screen,
    const char* clock_label,
    RefreshSchedule schedule)
{
    for (SubmittedDisplayRecord& record : tracking->records) {
        if (!record.occupied) {
            record.occupied = true;
            record.revision = submission.revision;
            record.signature = signature;
            record.screen = screen;
            std::strncpy(record.clock_label, clock_label ? clock_label : "",
                         sizeof(record.clock_label) - 1);
            record.clock_label[sizeof(record.clock_label) - 1] = '\0';
            record.schedule = schedule;
            record.full_refresh =
                submission.waveform == wqn::display::WaveformRequirement::kFull;
            tracking->submitted_revision = record.revision;
            tracking->submitted_signature = record.signature;
            tracking->submitted_schedule = record.schedule;
            tracking->submitted_full_refresh = record.full_refresh;
            return true;
        }
    }
    ESP_LOGE(kTag, "display submission ledger overflow: revision=%llu",
             static_cast<unsigned long long>(submission.revision));
    return false;
}

const char* DisplayStatusName(wqn::display::DisplayStatus status)
{
    switch (status) {
        case wqn::display::DisplayStatus::kPresented:
            return "presented";
        case wqn::display::DisplayStatus::kSuperseded:
            return "superseded";
        case wqn::display::DisplayStatus::kFailed:
        default:
            return "failed";
    }
}

wqn::display::WaveformRequirement RequestedWaveform(
    const wqn::UiFrame& frame,
    RefreshSchedule schedule,
    bool force_full)
{
    if (force_full || schedule == RefreshSchedule::kCommit) {
        return wqn::display::WaveformRequirement::kFull;
    }
    // Home frames prefer full redraws for general changes, while clock/timer
    // schedules deliberately use a small regional refresh.
    if (schedule == RefreshSchedule::kClock || schedule == RefreshSchedule::kTimer) {
        return wqn::display::WaveformRequirement::kPartial;
    }
    if (frame.prefer_full_refresh) {
        return wqn::display::WaveformRequirement::kFull;
    }
    if (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig) {
        return wqn::display::WaveformRequirement::kPartial;
    }
    return wqn::display::WaveformRequirement::kAuto;
}

void DrainDisplayResults(
    device_ui_internal::UiRuntime* runtime,
    DisplayTrackingState* tracking,
    RefreshSchedule* refresh_schedule)
{
    if (runtime == nullptr || g_display_result_queue == nullptr) {
        return;
    }
    wqn::display::DisplayResult result;
    while (xQueueReceive(g_display_result_queue, &result, 0) == pdTRUE) {
        const device_ui_internal::UiUpdate update =
            runtime->DispatchDisplayResult(result);
        *refresh_schedule = StrongerSchedule(*refresh_schedule, update.refresh);
        SubmittedDisplayRecord* record = FindSubmittedRecord(tracking, result.revision);
        if (record == nullptr) {
            ESP_LOGE(kTag, "display result has no submission record: revision=%llu status=%s",
                     static_cast<unsigned long long>(result.revision),
                     DisplayStatusName(result.status));
            AcknowledgeDisplayResult(result.revision);
            continue;
        }

        ESP_LOGI(kTag,
                 "display result: revision=%llu status=%s presented=%llu replacement=%llu error=%s",
                 static_cast<unsigned long long>(result.revision),
                 DisplayStatusName(result.status),
                 static_cast<unsigned long long>(result.presented_revision),
                 static_cast<unsigned long long>(result.replacement_revision),
                 esp_err_to_name(result.error));
        if (result.status == wqn::display::DisplayStatus::kPresented) {
            tracking->presented_revision = result.revision;
            tracking->presented_signature = record->signature;
            // RTC state is a physical-display commit record. It must never be
            // advanced merely because a request entered the refresh pipeline.
            RecordPresentedRefresh(record->screen, record->clock_label);
        } else if (result.status == wqn::display::DisplayStatus::kFailed) {
            tracking->force_next_submission = true;
        }

        const bool was_latest = tracking->submitted_revision == result.revision;
        record->occupied = false;
        record->revision = wqn::display::kInvalidDisplayRevision;
        record->signature.clear();
        record->schedule = RefreshSchedule::kNone;
        record->full_refresh = false;
        if (was_latest) {
            RecomputeLatestSubmission(tracking);
        }
        AcknowledgeDisplayResult(result.revision);
    }
}

void DeviceUiTask(void*)
{
    ESP_LOGI(kTag, "device UI task started");
    LogUiStackHighWater("start");
    // Skip the wake-up timer case: we just woke from RTC and the user did
    // not interact with the device, so recording activity here would
    // defeat the idle-off timer the moment the user stops touching the
    // keypad after a long sleep.
    if (wqn::runtime::GetWakeContext().kind != wqn::runtime::WakeKind::kScheduledTimer) {
        wqn::NoteUserActivity();
    }
    SeedClockFromBuildTimeIfNeeded();

    esp_err_t result = wqn::InitButtonInput();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "button input init failed: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    result = wqn::InitEpdDisplay();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "EPD display init failed: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "EPD display init OK, preparing first frame");

    device_ui_internal::UiRuntime& ui_runtime = g_ui_runtime;
    ESP_LOGI(kTag, "DeviceUiTask: calling LoadUiState");
    const bool state_loaded =
        device_ui_internal::LoadUiState(&g_ui_reload_snapshot);
    ui_runtime.Initialize(std::move(g_ui_reload_snapshot));
    const wqn::AppState& state = ui_runtime.state();
    ESP_LOGI(kTag, "DeviceUiTask: LoadUiState done");
    if (!state_loaded) {
        ESP_LOGW(kTag, "UI state loaded in degraded mode");
    }
    LogUiStackHighWater("state-loaded");
    // [power-fix] Do NOT seed g_last_rendered_screen from state.screen here:
    // a deep-sleep wake-up that goes through USB_UART_CHIP_RESET loses this
    // RAM variable, and unconditionally re-writing it from the freshly loaded
    // UI state would later trip the "screen change detected" branch in
    // RefreshFrame() and force a full refresh on every wake-up. Treat -1 as
    // "unknown", which forces the first refresh to be full but keeps subsequent
    // wake-ups inside the same screen and clock label on the RTC-CRC fast path.
    CheckBatteryProtection();
    ESP_LOGI(kTag, "DeviceUiTask: CheckBatteryProtection done");
    std::string last_clock_label = CurrentClockLabel();
    DisplayTrackingState& display_tracking = g_display_tracking;
    RefreshSchedule pending_refresh_schedule = RefreshSchedule::kNone;
    {
        const wqn::UiFrame frame = wqn::RenderUiFrame(state);
        display_tracking.desired_signature = FrameSignature(frame);
        RefreshSchedule init_schedule = RefreshSchedule::kImmediate;
        // [epd-crc-bypass-fix] On timer wake-up we want to skip the
        // un-conditional full refresh, but only if the current screen can
        // tolerate a partial pipeline. Screens like AI / Word / Todo have
        // scrolling overlays or partial-screen markers that would ghost if
        // we only refreshed the clock region, so fall back to kImmediate.
        if (wqn::runtime::GetWakeContext().kind == wqn::runtime::WakeKind::kScheduledTimer &&
            (state.screen == wqn::UiScreen::kTime ||
             state.screen == wqn::UiScreen::kHome)) {
            init_schedule = RefreshSchedule::kClock;
        }
        // [timer-skip] If a timer wake-up finds the freshly-rendered frame's
        // (screen, clock label) identical to the last successful render --
        // e.g. a per-minute RTC tick that fires within the same minute --
        // skip the refresh pipeline entirely. The panel already shows this
        // content, so we avoid any SPI traffic and any flicker.
        const char* const init_clock = last_clock_label.c_str();
        const bool skip_init_refresh = TrySkipInitRefresh(state.screen, init_clock);
        ESP_LOGI(kTag, "Requesting initial EPD refresh: signature_len=%zu schedule=%s skip=%d",
                 display_tracking.desired_signature.size(),
                 device_ui_internal::RefreshScheduleName(init_schedule),
                 skip_init_refresh ? 1 : 0);
        if (skip_init_refresh) {
            display_tracking.presented_signature = display_tracking.desired_signature;
        } else {
            const wqn::display::DisplaySubmission submission =
                RequestEpdUiRefresh(
                    frame, display_tracking.desired_signature, ui_runtime.revision(), init_schedule,
                    RequestedWaveform(frame, init_schedule, false));
            ESP_LOGI(kTag,
                     "initial display submission: accepted=%d revision=%llu task=%p",
                     submission.accepted ? 1 : 0,
                     static_cast<unsigned long long>(submission.revision),
                     device_ui_internal::g_refresh_task);
            if (submission.accepted) {
                TrackDisplaySubmission(&display_tracking, submission,
                                       display_tracking.desired_signature,
                                       state.screen, init_clock, init_schedule);
            } else {
                pending_refresh_schedule = init_schedule;
            }
        }
    }
    TickType_t last_status_refresh = xTaskGetTickCount();
    g_last_active_us_local = esp_timer_get_time();
    TickType_t poll_delay = kUiPollDelayTicks;
    bool word_pack_refresh_pending = false;

    while (true) {
        RefreshSchedule refresh_schedule = pending_refresh_schedule;
        pending_refresh_schedule = RefreshSchedule::kNone;
        DrainDisplayResults(&ui_runtime, &display_tracking, &refresh_schedule);
        bool todo_cloud_completed = false;
        bool word_cloud_completed = false;
        bool note_cloud_completed = false;
        if (g_sync_event_queue != nullptr) {
            wqn::services::SyncEvent sync_event;
            while (xQueueReceive(g_sync_event_queue, &sync_event, 0) == pdTRUE) {
                const device_ui_internal::UiUpdate update =
                    ui_runtime.DispatchSyncResult(sync_event);
                refresh_schedule =
                    StrongerSchedule(refresh_schedule, update.refresh);
                if (sync_event.status ==
                        wqn::services::SyncEventStatus::kSucceeded &&
                    sync_event.scope ==
                        wqn::services::SyncEventScope::kFull) {
                    // Sync notifications can arrive while a session page,
                    // search, or an earlier pack refresh owns WordCloud. Keep
                    // one coalesced refresh request instead of misreporting
                    // the busy owner as a full queue.
                    word_pack_refresh_pending = true;
                }
            }
        }
        const wqn::ButtonEvent event = wqn::PollButtonInput();
        if (event.HasEvent()) {
            wqn::NoteUserActivity();
            wqn::NoteEpdActivity();
            if (state.screen == wqn::UiScreen::kWord ||
                state.screen == wqn::UiScreen::kNote) {
                // Both study screens feed a durable outbox; an in-flight upload
                // batch yields to active input, then resumes after a quiet gap.
                wqn::services::NoteWordInteraction();
            }
            poll_delay = kUiPollDelayTicks;
            g_last_active_us_local = esp_timer_get_time();
        }
        // Apply the button event regardless of EPD refresh state. The previous
        // "skip while refreshing" guard silently dropped press/release events
        // whenever a slow partial refresh was in flight (a timer-page layout
        // change can stay in flight for 100-300 ms, long enough to swallow the
        // user's next press entirely). The
        // refresh task itself dedups via frame_signature and the primary /
        // secondary queue pair in ui_refresh.cpp, so racing with a refresh in
        // flight is safe -- the worst case is the new event lands in the
        // secondary slot and is picked up on the next consume.
        if (event.HasEvent()) {
            const RefreshSchedule before_sched = refresh_schedule;
            const device_ui_internal::UiUpdate update =
                ui_runtime.DispatchButton(event, esp_timer_get_time() / 1000);
            const RefreshSchedule after_sched =
                StrongerSchedule(refresh_schedule, update.refresh);
            if (after_sched != before_sched && after_sched == RefreshSchedule::kAi) {
                ESP_LOGI(kTag,
                         "AI button refresh scheduled: button=%d type=%d revision=%llu screen=%d tier=%d",
                         static_cast<int>(event.button), static_cast<int>(event.type),
                         static_cast<unsigned long long>(update.revision),
                         static_cast<int>(state.screen),
                         static_cast<int>(state.ai.tier));
            }
            refresh_schedule = after_sched;
        }

        if (g_todo_result_queue != nullptr) {
            device_ui_internal::TodoCloudResultReady ready;
            while (xQueueReceive(g_todo_result_queue, &ready, 0) == pdTRUE) {
                todo_cloud_completed = true;
                const device_ui_internal::TodoCloudResult* todo_result =
                    device_ui_internal::PeekTodoCloudResult(ready.generation);
                if (todo_result != nullptr) {
                    const device_ui_internal::UiUpdate update =
                        ui_runtime.DispatchTodoCloudResult(*todo_result);
                    refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
                } else {
                    ESP_LOGE(kTag, "stale Todo result signal: generation=%lu",
                             static_cast<unsigned long>(ready.generation));
                }
            }
        }
        if (g_word_result_queue != nullptr) {
            device_ui_internal::WordCloudResultReady ready;
            while (xQueueReceive(g_word_result_queue, &ready, 0) == pdTRUE) {
                word_cloud_completed = true;
                device_ui_internal::WordCloudResult* word_result =
                    device_ui_internal::PeekWordCloudResult(ready.generation);
                if (word_result != nullptr) {
                    const device_ui_internal::UiUpdate update =
                        ui_runtime.DispatchWordCloudResult(*word_result);
                    refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
                } else {
                    ESP_LOGE(kTag, "stale Word result signal: generation=%lu",
                             static_cast<unsigned long>(ready.generation));
                }
            }
        }
        if (g_note_result_queue != nullptr) {
            device_ui_internal::NoteCloudResultReady ready;
            while (xQueueReceive(g_note_result_queue, &ready, 0) == pdTRUE) {
                note_cloud_completed = true;
                device_ui_internal::NoteCloudResult* note_result =
                    device_ui_internal::PeekNoteCloudResult(ready.generation);
                if (note_result != nullptr) {
                    const device_ui_internal::UiUpdate update =
                        ui_runtime.DispatchNoteCloudResult(*note_result);
                    refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
                } else {
                    ESP_LOGE(kTag, "stale Note result signal: generation=%lu",
                             static_cast<unsigned long>(ready.generation));
                }
            }
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        const device_ui_internal::UiUpdate time_update =
            ui_runtime.DispatchTimeTick(now_ms);
        refresh_schedule = StrongerSchedule(refresh_schedule, time_update.refresh);

        const device_ui_internal::UiUpdate ai_tick_update =
            ui_runtime.DispatchAiTick(now_ms);
        refresh_schedule = StrongerSchedule(refresh_schedule, ai_tick_update.refresh);

        // [shell] Status-bar edit mode: 3 s inactivity auto-exit.
        const device_ui_internal::UiUpdate status_timeout_update =
            ui_runtime.DispatchStatusEditTimeout(now_ms);
        refresh_schedule = StrongerSchedule(
            refresh_schedule, status_timeout_update.refresh);

        // v2: while the user is recording, the top toast label needs to tick
        // up so "● 录音中 00:04" advances once a second. We do this here on
        // the UI task (the audio task only knows about stream samples).
        if (state.screen == wqn::UiScreen::kAi &&
            state.ai.status == wqn::AiSessionStatus::kListening) {
            // Use the recording start time stored in g_state via status_since_ms.
            const int32_t elapsed = static_cast<int32_t>(now_ms - state.ai.status_since_ms);
            if (elapsed >= 0) {
                wqn::SetAiRecordingLabel(elapsed);
            }
        }

wqn::AiStreamingStatusView streaming_view{};
        bool streaming_changed = false;
#if CONFIG_WQN_AI_ENABLE
        streaming_changed = wqn::CopyAiStreamingStatus(&streaming_view);
#endif
        if (streaming_changed) {
            const device_ui_internal::UiUpdate update =
                ui_runtime.DispatchAiStreamingSnapshot(streaming_view);
            refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
        }

#if CONFIG_WQN_AI_ENABLE
        wqn::AiSessionState ai_snapshot;
        if (wqn::CopyAiSessionToUi(&ai_snapshot)) {
            const device_ui_internal::UiUpdate update =
                ui_runtime.DispatchAiSessionSnapshot(ai_snapshot);
            refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
        }
        // [amp-fix] Pump the flash amp idle-tail timer regardless of screen so
        // stale open-amp state is closed even when the user has navigated away.
        wqn::PollFlashAmpIdle();
        wqn::FlashUiState flash_ui;
        if (wqn::CopyFlashStateToUi(&flash_ui)) {
            const device_ui_internal::UiUpdate update =
                ui_runtime.DispatchFlashSnapshot(flash_ui);
            refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
        }
#endif

        const std::string clock_label = CurrentClockLabel();
        if (clock_label != last_clock_label) {
            last_clock_label = clock_label;
            // [timer-skip] If the new minute label matches the last
            // successfully-rendered clock label we still need to update
            // state.time_app / home.primary_time_line above, but the on-screen
            // pixels are already correct -- suppress the refresh request so we
            // don't fall into the deep-sleep-induced forced-full-refresh path.
            const bool minute_changed_on_panel =
                !ShouldSkipMinuteTickRefresh(state.screen, clock_label.c_str());
            const device_ui_internal::UiUpdate update =
                ui_runtime.DispatchClockMinute(minute_changed_on_panel);
            refresh_schedule = StrongerSchedule(refresh_schedule, update.refresh);
        }

        const TickType_t now = xTaskGetTickCount();
        const bool status_reload_due =
            now - last_status_refresh >= kStatusRefreshDelayTicks;
        const bool interaction_quiet =
            esp_timer_get_time() - g_last_active_us_local >=
            kStatusReloadInteractionQuietUs;
        if (status_reload_due && refresh_schedule == RefreshSchedule::kNone &&
            !event.HasEvent() && interaction_quiet) {
            if (state.screen != wqn::UiScreen::kWord) {
                // Storage/Wi-Fi reads are an effect. Build the typed snapshot
                // outside UiRuntime, then reduce that immutable observation
                // into the single owned AppState. The word page is excluded:
                // its session has one live owner and receives typed sync
                // events, so reloading every persistent domain is stale work.
                g_ui_reload_snapshot = state;
                device_ui_internal::LoadUiState(&g_ui_reload_snapshot);
                const device_ui_internal::UiUpdate update =
                    ui_runtime.DispatchStatusReload(
                        std::move(g_ui_reload_snapshot));
                refresh_schedule =
                    StrongerSchedule(refresh_schedule, update.refresh);
            }
            CheckBatteryProtection();
            last_status_refresh = now;
        }

        if (refresh_schedule != RefreshSchedule::kNone) {
            wqn::UiFrame frame = wqn::RenderUiFrame(state);
            // [force-full-fix] Consume the one-shot flag HERE, after the final
            // render frame is built. RenderUiFrame above checks (but doesn't
            // clear) the flag, so all signature-comparison calls earlier in
            // this tick also saw it. Now we apply it to the actual frame that
            // will be pushed to the EPD, and clear it for the next tick.
            const bool consumed_force_full = wqn::ConsumeForceFullRefresh();
            if (consumed_force_full || display_tracking.force_next_submission) {
                frame.prefer_full_refresh = true;
            }
            const std::string frame_signature = FrameSignature(frame);
            display_tracking.desired_signature = frame_signature;
            const bool has_submission =
                display_tracking.submitted_revision != wqn::display::kInvalidDisplayRevision;
            const bool same_as_submitted =
                has_submission && frame_signature == display_tracking.submitted_signature;
            const bool same_as_presented =
                frame_signature == display_tracking.presented_signature;
            const bool force_submission =
                consumed_force_full || display_tracking.force_next_submission ||
                refresh_schedule == RefreshSchedule::kCommit;
            const wqn::display::WaveformRequirement waveform =
                RequestedWaveform(frame, refresh_schedule, force_submission);
            const bool requires_full =
                waveform == wqn::display::WaveformRequirement::kFull;
            const bool stronger_than_submitted =
                same_as_submitted &&
                (device_ui_internal::RefreshRank(refresh_schedule) >
                     device_ui_internal::RefreshRank(display_tracking.submitted_schedule) ||
                 (requires_full && !display_tracking.submitted_full_refresh));
            const bool should_submit =
                (!same_as_submitted || stronger_than_submitted || force_submission) &&
                (has_submission || !same_as_presented || force_submission);
            ESP_LOGI(kTag,
                     "dispatch refresh: schedule=%s desired_len=%zu submitted_match=%d presented_match=%d force=%d screen=%d tier=%d",
                     device_ui_internal::RefreshScheduleName(refresh_schedule),
                     frame_signature.size(), same_as_submitted ? 1 : 0,
                     same_as_presented ? 1 : 0, force_submission ? 1 : 0,
                     static_cast<int>(state.screen), static_cast<int>(state.ai.tier));
            if (should_submit) {
                // [flash-throttle] Flash streaming updates the sig every ~70ms but
                // the EPD partial takes ~734ms; coalesce into ~300ms refreshes so
                // the panel isn't buried and partial-charge ghosting stays bounded.
                bool skip_for_throttle = false;
                if (state.screen == wqn::UiScreen::kAi &&
                    state.ai.tier == wqn::AiTier::kFlash &&
                    state.ai.flash_is_streaming) {
                    static int64_t last_flash_render_ms = 0;
                    const int64_t now_ms_d = esp_timer_get_time() / 1000;
                    if (now_ms_d - last_flash_render_ms < 300) {
                        skip_for_throttle = true;
                    } else {
                        last_flash_render_ms = now_ms_d;
                    }
                }
                if (skip_for_throttle) {
                    ESP_LOGI(kTag, "Flash refresh throttled (coalescing streaming deltas)");
                    pending_refresh_schedule = StrongerSchedule(
                        pending_refresh_schedule, refresh_schedule);
                    display_tracking.force_next_submission = force_submission;
                } else {
                    const wqn::display::DisplaySubmission submission =
                        RequestEpdUiRefresh(
                            frame, frame_signature, ui_runtime.revision(),
                            refresh_schedule, waveform);
                    if (submission.accepted) {
                        ESP_LOGI(kTag,
                                 "display submitted: revision=%llu schedule=%s",
                                 static_cast<unsigned long long>(submission.revision),
                                 device_ui_internal::RefreshScheduleName(refresh_schedule));
                        if (TrackDisplaySubmission(
                                &display_tracking, submission, frame_signature,
                                state.screen, last_clock_label.c_str(), refresh_schedule)) {
                            display_tracking.force_next_submission = false;
                        }
                        g_last_active_us_local = esp_timer_get_time();
                        poll_delay = kUiPollDelayTicks;
                    } else {
                        ESP_LOGW(kTag, "display submission deferred: schedule=%s",
                                 device_ui_internal::RefreshScheduleName(refresh_schedule));
                        pending_refresh_schedule = StrongerSchedule(
                            pending_refresh_schedule, refresh_schedule);
                        display_tracking.force_next_submission = force_submission;
                    }
                }
            } else {
                ESP_LOGI(kTag, "display submission skipped: desired state already represented");
            }
        }

        // Hand cloud ownership to the display pipeline without an unguarded
        // gap: if the result changed the visible page, RequestEpdUiRefresh()
        // acquired the display lease above before these cloud leases release.
        if (todo_cloud_completed) {
            FinishTodoCloudRequest();
        }
        if (word_cloud_completed) {
            FinishWordCloudRequest();
        }
        if (note_cloud_completed) {
            FinishNoteCloudRequest();
        }
        device_ui_internal::PumpWordCandidatePrefetch(&ui_runtime);
        device_ui_internal::PumpNoteCandidatePrefetch(&ui_runtime);
        device_ui_internal::PumpNoteImageFetch(&ui_runtime);
        if (word_pack_refresh_pending &&
            !device_ui_internal::IsWordCloudBusy() &&
            device_ui_internal::QueueWordReviewRefresh()) {
            word_pack_refresh_pending = false;
            ESP_LOGI(kTag, "queued coalesced word pack refresh after sync");
        }

        wqn::PowerOffEpdAfterIdleIfNeeded();

        // Automatic light sleep is owned by ESP-IDF tickless idle. SleepLease
        // maps active service work to ESP_PM_NO_LIGHT_SLEEP; GPIO17 sleep-mode
        // selection is disabled by the Note4 HAL.
        vTaskDelay(pdMS_TO_TICKS(10));

        g_rtc_screen_val = static_cast<int>(state.screen);
        const bool enable_timer_wakeup = (state.screen == wqn::UiScreen::kHome ||
                                          state.screen == wqn::UiScreen::kTime);
        wqn::SetDeepSleepTimerWakePreference(enable_timer_wakeup);

        const int64_t idle_ms = (esp_timer_get_time() - g_last_active_us_local) / 1000;
        const bool screen_active = (state.screen == wqn::UiScreen::kTime ||
                                    state.screen == wqn::UiScreen::kAi ||
                                    state.screen == wqn::UiScreen::kWord);
        if (refresh_schedule != RefreshSchedule::kNone || screen_active) {
            poll_delay = kUiPollDelayTicks;
        } else if (idle_ms < 3000) {
            poll_delay = kUiPollDelayTicks;
        } else {
            poll_delay = kUiIdlePollDelayTicks;
        }

        vTaskDelay(poll_delay);
    }
}

esp_err_t EnsureDisplayPipelineStarted()
{
    if (device_ui_internal::g_refresh_mutex == nullptr) {
        device_ui_internal::g_refresh_mutex = xSemaphoreCreateMutex();
        if (device_ui_internal::g_refresh_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_display_result_queue == nullptr) {
        device_ui_internal::g_display_result_queue =
            xQueueCreate(wqn::display::kDisplayResultQueueDepth,
                         sizeof(wqn::display::DisplayResult));
        if (device_ui_internal::g_display_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_refresh_task == nullptr) {
        const BaseType_t refresh_created =
            xTaskCreate(
                device_ui_internal::EpdRefreshTask,
                "wqn_epd_refresh",
                12288,
                nullptr,
                5,
                &device_ui_internal::g_refresh_task);
        if (refresh_created != pdPASS) {
            device_ui_internal::g_refresh_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t StartDeviceUiIfEnabled()
{
#if CONFIG_WQN_EPD_UI_ENABLE
    const esp_err_t display_result = EnsureDisplayPipelineStarted();
    if (display_result != ESP_OK) {
        return display_result;
    }

    if (g_sync_event_queue == nullptr) {
        g_sync_event_queue = xQueueCreateStatic(
            kSyncEventQueueDepth,
            sizeof(wqn::services::SyncEvent),
            g_sync_event_queue_buffer,
            &g_sync_event_queue_storage);
        if (g_sync_event_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        wqn::services::SetSyncEventSink(UiSyncEventSink);
    }

    if (device_ui_internal::g_todo_request_queue == nullptr) {
        device_ui_internal::g_todo_request_queue = xQueueCreate(2, sizeof(device_ui_internal::TodoCloudRequest));
        if (device_ui_internal::g_todo_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_todo_result_queue == nullptr) {
        device_ui_internal::g_todo_result_queue =
            xQueueCreate(1, sizeof(device_ui_internal::TodoCloudResultReady));
        if (device_ui_internal::g_todo_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_word_request_queue == nullptr) {
        device_ui_internal::g_word_request_queue = xQueueCreate(3, sizeof(device_ui_internal::WordCloudRequest));
        if (device_ui_internal::g_word_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_word_result_queue == nullptr) {
        device_ui_internal::g_word_result_queue =
            xQueueCreate(1, sizeof(device_ui_internal::WordCloudResultReady));
        if (device_ui_internal::g_word_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_note_request_queue == nullptr) {
        device_ui_internal::g_note_request_queue = xQueueCreate(3, sizeof(device_ui_internal::NoteCloudRequest));
        if (device_ui_internal::g_note_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_note_result_queue == nullptr) {
        device_ui_internal::g_note_result_queue =
            xQueueCreate(1, sizeof(device_ui_internal::NoteCloudResultReady));
        if (device_ui_internal::g_note_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_todo_task == nullptr) {
        const BaseType_t todo_created =
            xTaskCreate(device_ui_internal::TodoCloudTask, "wqn_todo_cloud", 8192, nullptr, 3, &device_ui_internal::g_todo_task);
        if (todo_created != pdPASS) {
            device_ui_internal::g_todo_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_word_task == nullptr) {
        const BaseType_t word_created =
            xTaskCreate(device_ui_internal::WordCloudTask, "wqn_word_cloud", 8192, nullptr, 3, &device_ui_internal::g_word_task);
        if (word_created != pdPASS) {
            device_ui_internal::g_word_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (device_ui_internal::g_note_task == nullptr) {
        const BaseType_t note_created =
            xTaskCreate(device_ui_internal::NoteCloudTask, "wqn_note_cloud", 8192, nullptr, 3, &device_ui_internal::g_note_task);
        if (note_created != pdPASS) {
            device_ui_internal::g_note_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    // UI state loading verifies word-pack files with a 1 KiB local read buffer,
    // while page rendering has several deep C++ call chains. Keep explicit
    // headroom and monitor it through the HWM logs above.
    const BaseType_t created = xTaskCreate(DeviceUiTask, "wqn_ui", 12288, nullptr, 4, nullptr);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    ESP_LOGI(kTag, "EPD device UI disabled by CONFIG_WQN_EPD_UI_ENABLE");
    return ESP_OK;
#endif
}

esp_err_t ShowStorageRecoveryUi(esp_err_t storage_error)
{
#if CONFIG_WQN_EPD_UI_ENABLE
    const esp_err_t start_result = EnsureDisplayPipelineStarted();
    if (start_result != ESP_OK) {
        return start_result;
    }

    char error_line[80] = {};
    std::snprintf(
        error_line,
        sizeof(error_line),
        "错误：%s (0x%lx)",
        esp_err_to_name(storage_error),
        static_cast<unsigned long>(storage_error));

    wqn::UiFrame frame;
    frame.screen = wqn::UiScreen::kProvisioning;
    frame.prefer_full_refresh = true;
    frame.lines = {
        {wqn::UiTextStyle::kTitle, "存储恢复失败"},
        {wqn::UiTextStyle::kWarning, "业务服务已安全停止"},
        {wqn::UiTextStyle::kBody, error_line},
        {wqn::UiTextStyle::kWrappedBody, "请重启设备；若仍失败，请重新烧录或检查 Flash。"},
    };

    constexpr wqn::display::DisplayRevision kRecoveryRevision = 1;
    const wqn::display::DisplaySubmission submission =
        device_ui_internal::RequestEpdUiRefresh(
            frame,
            "m7-storage-recovery",
            kRecoveryRevision,
            device_ui_internal::RefreshSchedule::kCommit,
            wqn::display::WaveformRequirement::kFull);
    if (!submission.accepted) {
        return ESP_ERR_INVALID_STATE;
    }

    wqn::display::DisplayResult result;
    if (xQueueReceive(
            device_ui_internal::g_display_result_queue,
            &result,
            pdMS_TO_TICKS(45000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    device_ui_internal::AcknowledgeDisplayResult(result.revision);
    if (result.status != wqn::display::DisplayStatus::kPresented) {
        return result.error == ESP_OK ? ESP_FAIL : result.error;
    }

    // Give the panel controller its configured cooling interval, then use the
    // same display-owner idle path as the normal UI before entering the inert
    // recovery loop in app_main.
    vTaskDelay(pdMS_TO_TICKS(CONFIG_WQN_EPD_IDLE_POWER_OFF_MS + 100));
    wqn::PowerOffEpdAfterIdleIfNeeded();
    return ESP_OK;
#else
    ESP_LOGE(kTag, "storage recovery UI unavailable because EPD UI is disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

}  // namespace wqn
