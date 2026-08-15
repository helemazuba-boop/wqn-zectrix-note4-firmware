// Settings diagnostics: battery snapshot, flash/NVS/PSRAM stats, MAC, version.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_log.h"
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
}

}  // namespace device_ui_internal
