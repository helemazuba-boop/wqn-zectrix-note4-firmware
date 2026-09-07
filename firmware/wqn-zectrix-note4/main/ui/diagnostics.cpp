// Settings diagnostics: battery snapshot, flash/NVS/PSRAM stats, MAC, version.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "error_recorder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "diagnostics.h"
#include "services/sync_service.h"
#include "storage.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

size_t AutoSyncOptionIndex(uint32_t minutes)
{
    for (size_t i = 0; i < kAutoSyncOptionsCount; ++i) {
        if (kAutoSyncOptions[i] == minutes) {
            return i;
        }
    }
    return 0;
}

size_t VolumeOptionIndex(int percent)
{
    for (size_t i = 0; i < kVolumeOptionsCount; ++i) {
        if (kVolumeOptions[i] == percent) {
            return i;
        }
    }
    return kVolumeOptionsCount - 1;  // default to max volume
}

std::string OnlineSyncStatusLabel(const char* status)
{
    if (status == nullptr || status[0] == '\0') {
        return "空闲";
    }
    if (std::strcmp(status, "syncing") == 0) {
        return "正在同步";
    }
    if (std::strcmp(status, "success") == 0) {
        return "已同步";
    }
    if (std::strcmp(status, "partial") == 0) {
        return "部分完成，待重试";
    }
    if (std::strcmp(status, "failed") == 0) {
        return "同步失败";
    }
    if (std::strcmp(status, "waiting-pair") == 0) {
        return "等待配对";
    }
    if (std::strcmp(status, "wifi-disabled") == 0) {
        return "WiFi 未启用";
    }
    return status;
}

std::string BytesLabel(size_t bytes)
{
    char buffer[32] = {};
    if (bytes >= 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%lu MB", static_cast<unsigned long>(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%lu KB", static_cast<unsigned long>(bytes / 1024));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lu B", static_cast<unsigned long>(bytes));
    }
    return buffer;
}

// [dev-diag] Monotonic uptime rendered as a short human label ("3h12m").
std::string UptimeLabel()
{
    const int64_t minutes = esp_timer_get_time() / 1000000 / 60;
    if (minutes < 60) {
        return std::to_string(minutes) + "m";
    }
    return std::to_string(minutes / 60) + "h" + std::to_string(minutes % 60) + "m";
}

// [dev-diag] Sync content/outbox phase labels (DEV_DIAGNOSTICS.md §4.2).
std::string SyncContentPhaseLabel(wqn::services::SyncContentPhase phase)
{
    switch (phase) {
        case wqn::services::SyncContentPhase::kClean:
            return "干净";
        case wqn::services::SyncContentPhase::kFetching:
            return "拉取中";
        case wqn::services::SyncContentPhase::kInstalling:
            return "安装中";
        case wqn::services::SyncContentPhase::kBackoff:
            return "回退重试";
        case wqn::services::SyncContentPhase::kBlocked:
            return "受阻";
        default:
            return "?";
    }
}

std::string SyncOutboxPhaseLabel(wqn::services::SyncOutboxPhase phase)
{
    switch (phase) {
        case wqn::services::SyncOutboxPhase::kDrained:
            return "已清空";
        case wqn::services::SyncOutboxPhase::kPending:
            return "待上传";
        case wqn::services::SyncOutboxPhase::kYielded:
            return "让路中";
        case wqn::services::SyncOutboxPhase::kBlocked:
            return "受阻";
        default:
            return "?";
    }
}

std::string SyncDomainLine(
    const char* title,
    const std::string& phase_label,
    uint8_t retry_attempt,
    const char* last_error)
{
    std::string line = std::string(title) + " " + phase_label;
    if (retry_attempt > 0) {
        line += " 重试" + std::to_string(retry_attempt);
    }
    if (last_error != nullptr && last_error[0] != '\0') {
        line += " ";
        line += last_error;
    }
    return line;
}

void UpdateSettingsDiagnostics(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }

    uint32_t minutes = 0;
    if (wqn::LoadAutoSyncIntervalMinutes(&minutes) == ESP_OK) {
        state->settings.auto_sync_interval_min = minutes;
        state->settings.auto_sync_selected = AutoSyncOptionIndex(minutes);
    } else {
        state->settings.auto_sync_interval_min = 0;
        state->settings.auto_sync_selected = 0;
    }

    int volume_percent = 100;
    if (wqn::LoadVolumePercent(&volume_percent) == ESP_OK) {
        state->settings.volume_percent = volume_percent;
        state->settings.volume_selected = VolumeOptionIndex(volume_percent);
    } else {
        state->settings.volume_percent = 100;
        state->settings.volume_selected = VolumeOptionIndex(100);
    }

    wqn::ImageRenderMode image_mode = wqn::ImageRenderMode::kGray16;
    if (wqn::LoadImageRenderMode(&image_mode) != ESP_OK) {
        image_mode = wqn::ImageRenderMode::kGray16;
    }
    state->settings.image_render_mode = image_mode;
    state->settings.image_render_selected =
        image_mode == wqn::ImageRenderMode::kBlackWhite ? 0 : 1;

    // [wifi-redundancy] Stored WiFi identity for the WiFi-manage row/dialog:
    // the configured networks (preferred + backup), independent of the
    // transient connection state.
    {
        wqn::WifiCredentialStore wifi_store;
        if (wqn::LoadWifiCredentialStore(&wifi_store) == ESP_OK && wifi_store.count > 0) {
            std::snprintf(
                state->settings.wifi_primary_ssid,
                sizeof(state->settings.wifi_primary_ssid),
                "%s",
                wifi_store.slots[wifi_store.preferred].ssid);
            if (wifi_store.count >= 2) {
                const uint8_t backup = 1 - wifi_store.preferred;
                std::snprintf(
                    state->settings.wifi_backup_ssid,
                    sizeof(state->settings.wifi_backup_ssid),
                    "%s",
                    wifi_store.slots[backup].ssid);
            } else {
                state->settings.wifi_backup_ssid[0] = '\0';
            }
        } else {
            state->settings.wifi_primary_ssid[0] = '\0';
            state->settings.wifi_backup_ssid[0] = '\0';
        }
    }

    wqn::SettingsDiagnosticsSnapshot& snapshot = state->settings.diagnostics;
    BatteryReading battery = {};
    if (ReadBatteryStatus(&battery)) {
        snapshot.adc_raw = battery.raw;
        snapshot.adc_mv = battery.adc_mv;
        snapshot.battery_mv = battery.battery_mv;
        snapshot.battery_percent = battery.percent;
        snapshot.charging = battery.charging;
        snapshot.full = battery.full;
    }

    wqn::PlatformDiagnosticsSnapshot platform;
    if (wqn::ReadPlatformDiagnosticsSnapshot(&platform)) {
        if (platform.flash_valid) {
            snapshot.flash_size = platform.flash_size;
        }
        if (platform.psram_valid) {
            snapshot.psram_total = platform.psram_total;
            snapshot.psram_free = platform.psram_free;
            snapshot.psram_used =
                snapshot.psram_total >= snapshot.psram_free
                    ? snapshot.psram_total - snapshot.psram_free
                    : 0;
        } else {
            snapshot.psram_total = 0;
            snapshot.psram_free = 0;
            snapshot.psram_used = 0;
        }
        if (platform.wifi_mac_valid) {
            char buffer[24] = {};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%02X:%02X:%02X:%02X:%02X:%02X",
                platform.wifi_mac[0],
                platform.wifi_mac[1],
                platform.wifi_mac[2],
                platform.wifi_mac[3],
                platform.wifi_mac[4],
                platform.wifi_mac[5]);
            snapshot.mac_label = buffer;
        }
    }

    wqn::StorageCapacitySnapshot storage;
    if (wqn::ReadStorageCapacitySnapshot(&storage) && storage.nvs_valid) {
        snapshot.nvs_used_entries = storage.nvs_used_entries;
        snapshot.nvs_free_entries = storage.nvs_free_entries;
        snapshot.nvs_total_entries = storage.nvs_total_entries;
    }

    snapshot.firmware_version = WQN_FIRMWARE_VERSION;
    snapshot.board_id = WQN_BOARD_ID;
    snapshot.idf_target = CONFIG_IDF_TARGET;

    // [dev-diag] Dev info dialog fields (DEV_DIAGNOSTICS.md §4.1). All cheap
    // queries on the UI task; no locks beyond the spinlocks inside the heap
    // APIs. heap_min_free is the all-time watermark, i.e. the tightest the
    // internal heap has ever been since boot.
    snapshot.git_commit = WQN_GIT_COMMIT;
    snapshot.build_time = WQN_BUILD_TIME;
    snapshot.reset_reason_label = wqn::ResetReasonToString(esp_reset_reason());
    snapshot.uptime_label = UptimeLabel();
    snapshot.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snapshot.heap_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    // [dev-diag] Error-log ring copy (DEV_DIAGNOSTICS.md §4.3/§5). The ring is
    // snapshotted under its own spinlock via CopyRecentErrors, then formatted
    // here on the UI task; render draws the prepared lines verbatim.
    wqn::ErrorRecord records[wqn::kErrorRecordDepth] = {};
    const size_t record_count = wqn::CopyRecentErrors(records, wqn::kErrorRecordDepth);
    snapshot.error_line_count = record_count > 6 ? 6 : record_count;
    snapshot.error_count_label =
        record_count == 0 ? std::string("无") : std::to_string(record_count) + " 条";
    for (size_t i = 0; i < snapshot.error_line_count; ++i) {
        const wqn::ErrorRecord& record = records[record_count - 1 - i];  // newest first
        wqn::SettingsErrorLine& line = snapshot.error_lines[i];
        char time_label[10] = {};
        const std::time_t record_time = static_cast<std::time_t>(record.unix_sec);
        if (record_time >= kMinReasonableUnixTime) {
            std::tm local = {};
            localtime_r(&record_time, &local);
            std::strftime(time_label, sizeof(time_label), "%H:%M", &local);
        } else {
            std::snprintf(
                time_label, sizeof(time_label), "+%lum",
                static_cast<unsigned long>(record.uptime_ms / 60000));
        }
        std::snprintf(
            line.text, sizeof(line.text), "%s [%s] %s", time_label, record.tag,
            record.msg);
        line.valid = true;
    }
    for (size_t i = snapshot.error_line_count; i < 6; ++i) {
        snapshot.error_lines[i].valid = false;
    }

    wqn::services::SyncSnapshot online = {};
    wqn::services::GetSyncSnapshot(&online);
    char content_label[96] = {};
    std::snprintf(
        content_label,
        sizeof(content_label),
        "W %llu/%llu  N %llu/%llu  P %llu/%llu",
        static_cast<unsigned long long>(online.word_packs.applied_revision),
        static_cast<unsigned long long>(online.word_packs.desired_revision),
        static_cast<unsigned long long>(online.note_packs.applied_revision),
        static_cast<unsigned long long>(online.note_packs.desired_revision),
        static_cast<unsigned long long>(online.problem_packs.applied_revision),
        static_cast<unsigned long long>(online.problem_packs.desired_revision));
    snapshot.content_sync_label = content_label;
    if (online.status[0] != '\0') {
        state->settings.sync_status = OnlineSyncStatusLabel(online.status);
    }

    // [dev-diag] Sync diagnostics dialog lines (DEV_DIAGNOSTICS.md §4.2),
    // formatted here so the render path stays dumb. `online` is the copy
    // GetSyncSnapshot already made under its spinlock — no shared state is
    // referenced past this point.
    snapshot.sync_diag_summary =
        online.status[0] != '\0' ? OnlineSyncStatusLabel(online.status) : "空闲";
    snapshot.sync_diag_lines[0] =
        std::string(online.last_round_success ? "上轮 成功" : "上轮 未完成") +
        " · 成" + std::to_string(online.success_count) +
        "/部" + std::to_string(online.partial_count) +
        "/败" + std::to_string(online.failure_count);
    snapshot.sync_diag_lines[1] = SyncDomainLine(
        "词包", SyncContentPhaseLabel(online.word_packs.phase),
        online.word_packs.retry_attempt, online.word_packs.last_error);
    snapshot.sync_diag_lines[2] = SyncDomainLine(
        "笔记", SyncContentPhaseLabel(online.note_packs.phase),
        online.note_packs.retry_attempt, online.note_packs.last_error);
    snapshot.sync_diag_lines[3] = SyncDomainLine(
        "错题", SyncContentPhaseLabel(online.problem_packs.phase),
        online.problem_packs.retry_attempt, online.problem_packs.last_error);
    snapshot.sync_diag_lines[4] = SyncDomainLine(
        "词箱", SyncOutboxPhaseLabel(online.word_outbox.phase),
        online.word_outbox.retry_attempt, online.word_outbox.last_error);
    snapshot.sync_diag_lines[5] = SyncDomainLine(
        "笔箱", SyncOutboxPhaseLabel(online.note_outbox.phase),
        online.note_outbox.retry_attempt, online.note_outbox.last_error);
    snapshot.sync_diag_lines[6] = SyncDomainLine(
        "题箱", SyncOutboxPhaseLabel(online.problem_outbox.phase),
        online.problem_outbox.retry_attempt, online.problem_outbox.last_error);
}

}  // namespace device_ui_internal
