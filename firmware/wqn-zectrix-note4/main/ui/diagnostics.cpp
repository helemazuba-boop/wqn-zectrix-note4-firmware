// Settings diagnostics: battery snapshot, flash/NVS/PSRAM stats, MAC, version.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "nvs.h"
#include "online_sync.h"

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

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        snapshot.flash_size = flash_size;
    }

    nvs_stats_t nvs_stats = {};
    if (nvs_get_stats(nullptr, &nvs_stats) == ESP_OK) {
        snapshot.nvs_used_entries = nvs_stats.used_entries;
        snapshot.nvs_free_entries = nvs_stats.free_entries;
        snapshot.nvs_total_entries = nvs_stats.total_entries;
    }

    if (esp_psram_is_initialized()) {
        snapshot.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        snapshot.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snapshot.psram_used = snapshot.psram_total >= snapshot.psram_free ? snapshot.psram_total - snapshot.psram_free : 0;
    } else {
        snapshot.psram_total = 0;
        snapshot.psram_free = 0;
        snapshot.psram_used = 0;
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buffer[24] = {};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
        snapshot.mac_label = buffer;
    }

    snapshot.firmware_version = WQN_FIRMWARE_VERSION;
    snapshot.board_id = WQN_BOARD_ID;
    snapshot.idf_target = CONFIG_IDF_TARGET;

    wqn::OnlineSyncSnapshot online = {};
    wqn::GetOnlineSyncSnapshot(&online);
    if (online.status[0] != '\0') {
        state->settings.sync_status = OnlineSyncStatusLabel(online.status);
    }
}

}  // namespace device_ui_internal
