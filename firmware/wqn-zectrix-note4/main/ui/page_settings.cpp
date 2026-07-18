// Settings page rendering: row list, dialog boxes, progress bars.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <string>

#include "epd_display.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

esp_err_t DrawSettingsRow(size_t row_index, int y, const std::string& title, const std::string& value, bool selected)
{
    constexpr int kX = 12;
    constexpr int kWidth = 376;
    constexpr int kHeight = 36;
    if (selected) {
        FillRect(kX, y, 4, kHeight, true);
        DrawRect(kX + 7, y, kWidth - 7, kHeight);
        DrawRect(kX + 9, y + 2, kWidth - 11, kHeight - 4);
    } else {
        DrawRect(kX + 7, y, kWidth - 7, kHeight);
    }
    char index_label[4] = {};
    std::snprintf(index_label, sizeof(index_label), "%02u", static_cast<unsigned>(row_index + 1));
    ESP_RETURN_ON_ERROR(DrawClippedText(kX + 17, y + 10, 28, index_label), kTag, "draw settings index");
    ESP_RETURN_ON_ERROR(DrawClippedText(kX + 52, y + 4, 174, title), kTag, "draw settings title");
    if (!value.empty()) {
        ESP_RETURN_ON_ERROR(DrawClippedText(kX + 52, y + 20, 214, value), kTag, "draw settings value");
    }
    const char* tag = "菜单";
    if (row_index == 0) {
        tag = "执行";
    } else if (row_index == 1) {
        tag = "设置";
    } else if (row_index == 4) {
        tag = "设置";  // volume
    } else if (row_index == 5) {
        tag = "系统";  // firmware version
    } else if (row_index == 6) {
        tag = "重置";  // factory reset
    }
    DrawRect(kX + 298, y + 8, 54, 20);
    ESP_RETURN_ON_ERROR(DrawCenteredText(kX + 298, y + 10, 54, tag), kTag, "draw settings tag");
    return ESP_OK;
}

esp_err_t DrawSettingsDialogBox(const std::string& title)
{
    FillRect(68, 48, 264, 206, false);
    DrawRect(68, 48, 264, 206);
    DrawRect(70, 50, 260, 202);
    DrawHorizontalLine(86, 80, 228);
    DrawHorizontalLine(86, 220, 228);
    ESP_RETURN_ON_ERROR(DrawCenteredText(86, 60, 228, title), kTag, "draw settings dialog title");
    return DrawCenteredText(86, 230, 228, "确认关闭");
}

esp_err_t DrawSettingsOptionCard(int x, int y, int width, const std::string& label, bool selected)
{
    constexpr int kHeight = 32;
    DrawRect(x, y, width, kHeight);
    if (selected) {
        DrawRect(x + 2, y + 2, width - 4, kHeight - 4);
    }
    return DrawCenteredText(x, y + 9, width, label);
}

void DrawSettingsProgressBar(int x, int y, int width, int current, int total)
{
    constexpr int kHeight = 9;
    DrawRect(x, y, width, kHeight);
    if (total <= 0 || current <= 0) {
        return;
    }
    const int filled = std::clamp((width - 2) * current / total, 0, width - 2);
    FillRect(x + 1, y + 1, filled, kHeight - 2, true);
}

esp_err_t RenderSettingsDialog(const wqn::SettingsAppState& settings)
{
    const wqn::SettingsDiagnosticsSnapshot& diag = settings.diagnostics;
    switch (settings.dialog) {
        case wqn::SettingsDialog::kAutoSync: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("自动同步间隔"), kTag, "draw auto sync dialog");
            for (size_t i = 0; i < kAutoSyncOptionsCount; ++i) {
                const int x = (i % 2 == 0) ? 88 : 204;
                const int y = 94 + static_cast<int>(i / 2) * 40;
                const bool selected = i == settings.auto_sync_selected;
                ESP_RETURN_ON_ERROR(
                    DrawSettingsOptionCard(x, y, 96, wqn::AutoSyncIntervalLabel(kAutoSyncOptions[i]), selected),
                    kTag,
                    "draw auto sync option");
            }
            ESP_RETURN_ON_ERROR(DrawCenteredText(86, 202, 228, "上下选择  确认保存"), kTag, "draw auto sync help");
            break;
        }
        case wqn::SettingsDialog::kBattery: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("电量详情"), kTag, "draw battery dialog");
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 94, 224, "adc_raw: " + std::to_string(diag.adc_raw)), kTag, "draw battery raw");
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 114, 224, "adc_mv: " + std::to_string(diag.adc_mv)), kTag, "draw battery adc");
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 134, 224, "battery_mv: " + std::to_string(diag.battery_mv)), kTag, "draw battery mv");
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 154, 224, "percent: " + std::to_string(diag.battery_percent) + "%"), kTag, "draw battery percent");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(88, 174, 224, std::string("charging/full: ") + (diag.charging ? "1" : "0") + "/" + (diag.full ? "1" : "0")),
                kTag,
                "draw battery flags");
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 198, 224, "(-V^2+9016V-19189000)/10000"), kTag, "draw battery formula");
            break;
        }
        case wqn::SettingsDialog::kStorage: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("存储详情"), kTag, "draw storage dialog");
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 94, 224, "Flash: " + BytesLabel(diag.flash_size)), kTag, "draw flash size");
            DrawRect(88, 114, 224, 9);
            ESP_RETURN_ON_ERROR(
                DrawClippedText(
                    88,
                    132,
                    224,
                    "NVS entries: " + std::to_string(diag.nvs_used_entries) + "/" + std::to_string(diag.nvs_total_entries)),
                kTag,
                "draw nvs entries");
            const int nvs_used = static_cast<int>(diag.nvs_used_entries);
            const int nvs_total = static_cast<int>(std::max<size_t>(1, diag.nvs_total_entries));
            DrawSettingsProgressBar(88, 152, 224, nvs_used, nvs_total);
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 168, 224, "PSRAM used: " + BytesLabel(diag.psram_used)), kTag, "draw psram used");
            const int psram_used_kb = static_cast<int>(diag.psram_used / 1024);
            const int psram_total_kb = static_cast<int>(std::max<size_t>(1, diag.psram_total / 1024));
            DrawSettingsProgressBar(88, 188, 224, psram_used_kb, psram_total_kb);
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 202, 224, "PSRAM free: " + BytesLabel(diag.psram_free)), kTag, "draw psram free");
            break;
        }
        case wqn::SettingsDialog::kVolume: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("音量"), kTag, "draw volume dialog");
            for (size_t i = 0; i < kVolumeOptionsCount; ++i) {
                const int x = (i % 2 == 0) ? 88 : 204;
                const int y = 94 + static_cast<int>(i / 2) * 40;
                const bool selected = i == settings.volume_selected;
                ESP_RETURN_ON_ERROR(
                    DrawSettingsOptionCard(x, y, 96, wqn::VolumeLabel(kVolumeOptions[i]), selected),
                    kTag,
                    "draw volume option");
            }
            ESP_RETURN_ON_ERROR(DrawCenteredText(86, 202, 228, "上下选择  确认保存"), kTag, "draw volume help");
            break;
        }
        case wqn::SettingsDialog::kFactoryReset:
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("恢复出厂"), kTag, "draw factory reset dialog");
            ESP_RETURN_ON_ERROR(DrawWrappedText(54, 98, 292, "将清除 NVS 中的配对、缓存、待上传、AI 会话、单词进度和设置。", 3), kTag, "draw reset body");
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 176, 312, "这是不可撤销操作"), kTag, "draw reset warning");
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 200, 312, "长按确认执行，短按确认取消"), kTag, "draw reset help");
            break;
        case wqn::SettingsDialog::kNone:
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t RenderSettingsToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::SettingsAppState& settings = frame.settings;
    const wqn::SettingsDiagnosticsSnapshot& diag = settings.diagnostics;
    const bool partial_settings = schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig;
    if (partial_settings) {
        ClearRect(kSettingsContentRect);
    } else {
        wqn::ClearEpdFramebuffer(true);
    }

    if (!partial_settings) {
        DrawHorizontalLine(0, 27, wqn::kEpdWidth);
        std::string title = "设置";
        if (!diag.mac_label.empty()) {
            title += " (MAC: " + diag.mac_label + ")";
        }
        ESP_RETURN_ON_ERROR(DrawClippedText(10, 6, 250, title), kTag, "draw settings title");
        // Right-edge icon cluster (wifi + battery); time right-aligned to its left.
        const int icons_left = DrawStatusBarIcons(wqn::kEpdWidth - 10, 6, frame.home);
        const std::string time_str = CurrentClockLabel();
        const int time_w = wqn::MeasureUtf8TextWidth(time_str.c_str());
        const int time_x = std::max(10, icons_left - 6 - time_w);
        ESP_RETURN_ON_ERROR(
            DrawClippedText(time_x, 6, std::min(180, time_w + 4), time_str),
            kTag,
            "draw settings status");
    }

    const std::string auto_sync_label = wqn::AutoSyncIntervalLabel(settings.auto_sync_interval_min);
    const std::string volume_label = wqn::VolumeLabel(settings.volume_percent);
    const std::string battery_value =
        diag.full ? "满电" : (diag.charging ? "充电 " + std::to_string(diag.battery_percent) + "%" : std::to_string(diag.battery_percent) + "%");
    const std::string storage_value = "NVS " + std::to_string(diag.nvs_used_entries) + "/" + std::to_string(diag.nvs_total_entries);
    const std::string version_value = diag.firmware_version.empty() ? WQN_FIRMWARE_VERSION : diag.firmware_version;

    const std::string titles[kSettingsItemCount] = {
        "立即同步",
        "自动同步间隔",
        "电量",
        "存储详情",
        "音量",
        "固件版本",
        "恢复出厂",
    };
    const std::string values[kSettingsItemCount] = {
        settings.sync_status.empty() ? "空闲" : settings.sync_status,
        auto_sync_label,
        battery_value,
        storage_value,
        volume_label,
        version_value,
        "",
    };

    int y = 42;
    for (size_t i = 0; i < kSettingsItemCount; ++i) {
        ESP_RETURN_ON_ERROR(DrawSettingsRow(i, y, titles[i], values[i], i == settings.selected), kTag, "draw settings row");
        y += 38;
    }
    ESP_RETURN_ON_ERROR(
        DrawClippedText(14, 274, 372, settings.notice.empty() ? "上下选择，确认操作，长按确认返回首页" : settings.notice),
        kTag,
        "draw settings help");

    if (settings.dialog != wqn::SettingsDialog::kNone) {
        ESP_RETURN_ON_ERROR(RenderSettingsDialog(settings), kTag, "draw settings dialog");
    }
    if (schedule == RefreshSchedule::kSelection) {
        return RefreshStableRegion(kSettingsContentRect, schedule);
    }
    if (schedule == RefreshSchedule::kConfig) {
        return RefreshStableRegion(kSettingsContentRect, schedule);
    }
    return RefreshFrame(frame, schedule);
}

void OpenSettingsDialog(wqn::UiState* state, wqn::SettingsDialog dialog)
{
    if (state == nullptr) {
        return;
    }
    UpdateSettingsDiagnostics(state);
    state->settings.dialog = dialog;
    if (dialog == wqn::SettingsDialog::kAutoSync) {
        state->settings.auto_sync_selected = AutoSyncOptionIndex(state->settings.auto_sync_interval_min);
    } else if (dialog == wqn::SettingsDialog::kVolume) {
        state->settings.volume_selected = VolumeOptionIndex(state->settings.volume_percent);
    }
}

}  // namespace device_ui_internal
