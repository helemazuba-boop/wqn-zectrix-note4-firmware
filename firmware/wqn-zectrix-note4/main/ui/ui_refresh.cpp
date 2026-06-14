// EPD refresh task: double-buffered frame slot, secondary slot for producer-during-render,
// promotion path, dedup. This is the file where the [fix epd-hang] lives.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "epd_display.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";
constexpr int64_t kUiPollDelay = pdMS_TO_TICKS(50);
constexpr int64_t kUiIdlePollDelay = pdMS_TO_TICKS(500);
constexpr int64_t kStatusRefreshDelay = pdMS_TO_TICKS(60000);

// ---- Global refresh state (definitions live here) --------------------------

SemaphoreHandle_t g_refresh_mutex = nullptr;
TaskHandle_t g_refresh_task = nullptr;
wqn::UiFrame g_pending_frames[2];
std::string g_pending_signatures[2];
std::atomic<int> g_consumer_index{0};
bool g_refresh_pending = false;
bool g_refresh_busy = false;
TickType_t g_refresh_due_tick = 0;
RefreshSchedule g_refresh_schedule = RefreshSchedule::kNone;
SecondarySlot g_secondary;
volatile wqn::UiScreen g_last_rendered_screen = wqn::UiScreen::kHome;

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
        signature.append(frame.home.battery_label);
        signature.push_back('/');
        signature.append(std::to_string(static_cast<int>(frame.ai.status)));
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
        for (const std::string& summary : frame.ai.function_call_summaries) {
            signature.push_back('/');
            signature.append(summary);
        }
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
        signature.append(std::to_string(static_cast<int>(frame.word_app.home_selection)));
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
        signature.append(frame.settings.sync_status);
        signature.push_back('/');
        signature.append(frame.settings.notice);
        signature.push_back('/');
        signature.append(diag.mac_label);
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

bool RequestEpdUiRefresh(const wqn::UiFrame& frame, const std::string& signature, RefreshSchedule schedule)
{
    if (g_refresh_mutex == nullptr || g_refresh_task == nullptr || schedule == RefreshSchedule::kNone) {
        return false;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t due_tick = now + RefreshDelay(schedule);

    xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
    if (g_refresh_busy) {
        // Consumer is rendering: the main path buffer cannot be written without breaking
        // the in-flight frame. Deep-copy once into the secondary slot (rare in steady state).
        if (g_secondary.pending && RefreshRank(schedule) < RefreshRank(g_secondary.schedule)) {
            xSemaphoreGive(g_refresh_mutex);
            return false;
        }
        ESP_LOGI(kTag, "RequestEpdUiRefresh: busy, queuing into secondary slot schedule=%s sig_len=%zu",
                 RefreshScheduleName(schedule), signature.size());
        g_secondary.frame = frame;
        g_secondary.signature = signature;
        g_secondary.schedule = schedule;
        g_secondary.due_tick = due_tick;
        g_secondary.pending = true;
        xSemaphoreGive(g_refresh_mutex);
        return true;
    }

    // Main path: lock-free write into the producer-owned buffer, then flip the consumer index.
    const int consumer_holds = g_consumer_index.load(std::memory_order_acquire);
    const int producer_slot = 1 - consumer_holds;
    ESP_LOGI(kTag, "RequestEpdUiRefresh: primary path schedule=%s sig_len=%zu slot=%d",
             RefreshScheduleName(schedule), signature.size(), producer_slot);
    g_pending_frames[producer_slot] = frame;       // deep-copy once (independent of consumer render path)
    g_pending_signatures[producer_slot] = signature;
    g_refresh_pending = true;
    g_refresh_schedule = schedule;
    g_refresh_due_tick = due_tick;
    // Index flip must happen inside the lock, atomic with pending/schedule, to avoid
    // an idx vs busy reorder race with the consumer promotion critical section.
    g_consumer_index.store(producer_slot, std::memory_order_release);
    xSemaphoreGive(g_refresh_mutex);
    xTaskNotifyGive(g_refresh_task);
    taskYIELD();
    return true;
}

// ---- Consumer-side: EPD render task -----------------------------------------

void EpdRefreshTask(void*)
{
    esp_rom_printf("I (%u) wqn_ui: EPD refresh task started\n", (unsigned)xTaskGetTickCount());

#if CONFIG_ESP_TASK_WDT_EN
    // Try add then delete: if the task was subscribed to TWDT at creation (FreeRTOS default),
    // delete here takes effect. If add fails the task is not subscribed; delete also fails — fine either way.
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
    esp_rom_printf("I (%u) wqn_ui: EPD refresh task unsubscribed from task watchdog\n", (unsigned)xTaskGetTickCount());
#endif

    {
        const UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
        esp_rom_printf("I (%u) wqn_ui: EPD refresh task initial stack HWM: %u bytes free\n",
                       (unsigned)xTaskGetTickCount(), (unsigned)(stack_high_water * sizeof(StackType_t)));
    }

    std::string displayed_signature;
    while (true) {
        esp_rom_printf("I (%u) wqn_ui: EPD refresh: waiting on notify\n", (unsigned)xTaskGetTickCount());
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_rom_printf("I (%u) wqn_ui: EPD refresh: notify received\n", (unsigned)xTaskGetTickCount());

        while (true) {
            TickType_t wait_ticks = 0;
            xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
            if (!g_refresh_pending) {
                g_refresh_schedule = RefreshSchedule::kNone;
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
        RefreshSchedule schedule = RefreshSchedule::kNone;
        int consume_idx = 0;
        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        if (!g_refresh_pending) {
            xSemaphoreGive(g_refresh_mutex);
            continue;
        }
        consume_idx = g_consumer_index.load(std::memory_order_acquire);
        schedule = g_refresh_schedule;
        local_sig = g_pending_signatures[consume_idx];
        g_refresh_pending = false;
        g_refresh_schedule = RefreshSchedule::kNone;
        g_refresh_due_tick = 0;
        g_refresh_busy = true;
        xSemaphoreGive(g_refresh_mutex);

        // const-ref outside the lock (producer can only write secondary; promotion only touches the opposite slot)
        const wqn::UiFrame& frame = g_pending_frames[consume_idx];

        // [fix epd-hang] Even when primary dedup hits, if the secondary slot still holds a frame
        // we must take the promote path. Otherwise a frame that the producer pushed into
        // secondary during our render gets permanently lost and the screen stays stale.
        // Trigger: EPD render is slow (1-3s) and the user presses a button mid-render →
        //   producer writes secondary → consumer finishes and short-circuits on dedup
        //   → skips promote → subsequent button events get swallowed by the still-pending
        //   secondary slot.
        bool has_secondary = false;
        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        has_secondary = g_secondary.pending;
        xSemaphoreGive(g_refresh_mutex);

        if (local_sig == displayed_signature && !has_secondary) {
            xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
            g_refresh_busy = false;
            xSemaphoreGive(g_refresh_mutex);
            continue;
        }

        if (local_sig != displayed_signature) {
            const int64_t refresh_start_us = esp_timer_get_time();
            const esp_err_t result = RenderFrameToEpd(frame, schedule);
            const int64_t refresh_elapsed_ms = (esp_timer_get_time() - refresh_start_us) / 1000;
            if (result == ESP_OK) {
                displayed_signature = local_sig;
                wqn::NoteEpdActivity();
                esp_rom_printf("I (%u) wqn_ui: EPD UI refresh done: schedule=%s elapsed_ms=%lld\n",
                               (unsigned)xTaskGetTickCount(), RefreshScheduleName(schedule), static_cast<long long>(refresh_elapsed_ms));
            } else {
                esp_rom_printf("W (%u) wqn_ui: EPD UI render failed: %s\n",
                               (unsigned)xTaskGetTickCount(), esp_err_to_name(result));
            }
        }

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
            g_refresh_schedule = g_secondary.schedule;
            g_refresh_due_tick = g_secondary.due_tick;
            g_secondary.schedule = RefreshSchedule::kNone;
            g_secondary.due_tick = 0;
            g_secondary.pending = false;
            g_refresh_pending = true;
            g_refresh_busy = true;          // keep busy: about to render again
            // Index flip inside the lock, atomic with promotion state, visible to the consumer.
            g_consumer_index.store(new_slot, std::memory_order_release);
        } else {
            g_refresh_busy = false;
        }
        xSemaphoreGive(g_refresh_mutex);
        if (do_promote) {
            esp_rom_printf("I (%u) wqn_ui: EPD refresh: promoting secondary to primary slot=%d schedule=%s\n",
                           (unsigned)xTaskGetTickCount(),
                           new_slot, RefreshScheduleName(promote_sched));
            xTaskNotifyGive(g_refresh_task);
        }
    }
}

}  // namespace device_ui_internal
