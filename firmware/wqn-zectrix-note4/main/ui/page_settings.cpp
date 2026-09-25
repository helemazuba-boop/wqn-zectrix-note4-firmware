// Settings page rendering: row list, dialog boxes, progress bars.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <string>

#include "display_service.h"
#include "esp_log.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

esp_err_t DrawSettingsRow(wqn::SettingsView view, size_t row_index, int y, const std::string& title, const std::string& value, bool selected)
{
    // [v2] Row horizontal bounds aligned to the AI page's flush 6px edges
    // (kMarginDense): left x=6, width 388 (6..394). Vertical geometry (y, 36px
    // row height, 38px pitch) is unchanged per the agreed left/right-only fix.
    constexpr int kX = kMarginDense;
    constexpr int kContentX = kX;               // row body starts at the flush edge
    constexpr int kWidth = wqn::kEpdWidth - 2 * kMarginDense;  // 388
    constexpr int kContentWidth = kWidth;
    constexpr int kHeight = 36;
    // Index gutter: the row's 2-digit index is also the selection marker. When
    // selected the index sits in a rounded reverse-fill block (the kInvert
    // will-execute language, rounded chip shape) so the active row is obvious;
    // unselected shows the plain index with no block.
    constexpr int kIndexBlockX = kContentX + 6;  // 6px inside the row body
    constexpr int kIndexBlockYOff = 6;            // offset from row top, centered in 36h
    constexpr int kIndexBlockW = 36;
    constexpr int kIndexBlockH = 24;
    // [v2] Row outline: rounded double border (kRoundedInnerBorder) when selected,
    // plain rounded outline otherwise -- the whole settings page uses the rounded
    // container language; the square kInnerBorder/DrawRect mix is retired.
    if (selected) {
        DrawSelectionDecoration(kContentX, y, kContentWidth, kHeight, SelectionStyle::kRoundedInnerBorder);
        // Index block: rounded reverse-fill chip behind the index number.
        DrawSelectionDecoration(kIndexBlockX, y + kIndexBlockYOff, kIndexBlockW, kIndexBlockH, SelectionStyle::kInvert);
    } else {
        DrawRoundedRect(kContentX, y, kContentWidth, kHeight, kRoundedOuterRadius);
    }
    char index_label[4] = {};
    std::snprintf(index_label, sizeof(index_label), "%02u", static_cast<unsigned>(row_index + 1));
    if (selected) {
        // Index number in paper (white) on the reverse-fill block, centered in it.
        ESP_RETURN_ON_ERROR(DrawCenteredText(kIndexBlockX, y + kIndexBlockYOff + 4, kIndexBlockW, index_label, false), kTag, "draw settings index");
    } else {
        // Unselected: plain ink index, left-aligned in the index gutter.
        ESP_RETURN_ON_ERROR(DrawClippedText(kContentX + 12, y + 10, 28, index_label), kTag, "draw settings index");
    }
    // Title / value sit right of the 48px index gutter; tag is pinned to the
    // row's right edge (right-aligned at kContentX + kContentWidth - 6 inset).
    ESP_RETURN_ON_ERROR(DrawClippedText(kContentX + 48, y + 4, 220, title), kTag, "draw settings title");
    if (!value.empty()) {
        ESP_RETURN_ON_ERROR(DrawClippedText(kContentX + 48, y + 20, 250, value), kTag, "draw settings value");
    }
    const char* tag = "菜单";
    if (view == wqn::SettingsView::kDev) {
        tag = "查看";  // every dev row opens a read-only dialog
    } else if (row_index == wqn::kSettingsRowWifi) {
        tag = "设置";  // WiFi manage
    } else if (row_index == wqn::kSettingsRowSyncNow) {
        tag = "执行";  // sync now
    } else if (row_index == wqn::kSettingsRowAutoSync ||
               row_index == wqn::kSettingsRowImageRender ||
               row_index == wqn::kSettingsRowVolume ||
               row_index == wqn::kSettingsRowWordDeck ||
               row_index == wqn::kSettingsRowAiFollow) {
        tag = "设置";
    } else if (row_index == wqn::kSettingsRowBattery) {
        tag = "查看";  // read-only notice, the raw numbers live in the dev list
    } else if (row_index == wqn::kSettingsRowVersion) {
        tag = "系统";  // firmware version
#if CONFIG_WQN_DEV_MENU_ENABLE
    } else if (row_index == wqn::kSettingsRowDevMenu) {
        tag = "菜单";  // opens the second-level dev list
#endif
    } else if (row_index == wqn::kSettingsRowFactoryReset) {
        tag = "重置";  // factory reset
    } else if (row_index == wqn::kSettingsRowPowerOff) {
        tag = "电源";  // power off
    }
    DrawChip(kContentX + kContentWidth - 6 - 54, y + 8, 54, 20, tag);
    return ESP_OK;
}

esp_err_t DrawSettingsDialogBox(const std::string& title, const char* footer)
{
    FillRect(68, 48, 264, 206, false);
    // [v2] Dialog container: single rounded outline. The double-outline was the
    // 双线二义 source (双线 both decorated dialogs AND signalled selection);
    // double border is now reserved for selection only, dialogs use one line.
    DrawRoundedRect(68, 48, 264, 206, kRoundedOuterRadius);
    DrawHorizontalLine(86, 80, 228);
    DrawHorizontalLine(86, 220, 228);
    ESP_RETURN_ON_ERROR(DrawCenteredText(86, 60, 228, title), kTag, "draw settings dialog title");
    return DrawCenteredText(86, 230, 228, footer);
}

esp_err_t DrawSettingsOptionCard(int x, int y, int width, const std::string& label, bool selected)
{
    constexpr int kHeight = 32;
    // [v2] Focus: rounded reverse-fill (kInvert) when selected -- these dialog
    // options execute-on-confirm (上下选择 确认保存), which is the will-execute
    // action language, not the persistent-browse double border. Unselected is a
    // plain rounded chip outline.
    if (selected) {
        DrawSelectionDecoration(x, y, width, kHeight, SelectionStyle::kInvert);
    } else {
        DrawRoundedRect(x, y, width, kHeight, kChipRadius);
    }
    return DrawCenteredText(x, y + 9, width, label, !selected);
}

esp_err_t RenderSettingsDialog(const wqn::SettingsAppState& settings)
{
    const wqn::SettingsDiagnosticsSnapshot& diag = settings.diagnostics;
    switch (settings.dialog) {
        case wqn::SettingsDialog::kWifiManage: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("WiFi 管理", "确认进入配网"), kTag, "draw wifi dialog");
            const bool has_primary = settings.wifi_primary_ssid[0] != '\0';
            const bool has_backup = settings.wifi_backup_ssid[0] != '\0';
            ESP_RETURN_ON_ERROR(
                DrawClippedText(
                    88, 94, 224,
                    std::string("主网络: ") + (has_primary ? settings.wifi_primary_ssid : "未配置")),
                kTag,
                "draw wifi primary");
            if (has_backup) {
                ESP_RETURN_ON_ERROR(
                    DrawClippedText(88, 116, 224, std::string("备用:   ") + settings.wifi_backup_ssid),
                    kTag,
                    "draw wifi backup");
            }
            // Single focused action: start the SoftAP provisioning portal.
            ESP_RETURN_ON_ERROR(
                DrawSettingsOptionCard(88, 150, 224, "重新配网", true),
                kTag,
                "draw wifi provision action");
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(86, 196, 228, "确认启动配网  上下关闭"),
                kTag,
                "draw wifi help");
            break;
        }
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
            ESP_RETURN_ON_ERROR(
                DrawClippedText(88, 112, 224, diag.content_sync_label),
                kTag,
                "draw content sync revisions");
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
            DrawProgressBar(88, 152, 224, 9, nvs_used, nvs_total);
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 168, 224, "PSRAM used: " + BytesLabel(diag.psram_used)), kTag, "draw psram used");
            const int psram_used_kb = static_cast<int>(diag.psram_used / 1024);
            const int psram_total_kb = static_cast<int>(std::max<size_t>(1, diag.psram_total / 1024));
            DrawProgressBar(88, 188, 224, 9, psram_used_kb, psram_total_kb);
            ESP_RETURN_ON_ERROR(DrawClippedText(88, 202, 224, "PSRAM free: " + BytesLabel(diag.psram_free)), kTag, "draw psram free");
            break;
        }
        case wqn::SettingsDialog::kImageRendering: {
            ESP_RETURN_ON_ERROR(
                DrawSettingsDialogBox("图片渲染方式"), kTag,
                "draw image rendering dialog");
            ESP_RETURN_ON_ERROR(
                DrawSettingsOptionCard(
                    88, 112, 224, "黑白｜快速省电",
                    settings.image_render_selected == 0),
                kTag, "draw BW image option");
            ESP_RETURN_ON_ERROR(
                DrawSettingsOptionCard(
                    88, 156, 224, "16阶灰度｜细节优先",
                    settings.image_render_selected == 1),
                kTag, "draw gray image option");
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(86, 202, 228, "上下选择  确认保存"), kTag,
                "draw image rendering help");
            break;
        }
        case wqn::SettingsDialog::kAiFollow: {
            ESP_RETURN_ON_ERROR(
                DrawSettingsDialogBox("AI 回复时翻页"), kTag,
                "draw ai follow dialog");
            ESP_RETURN_ON_ERROR(
                DrawSettingsOptionCard(
                    88, 112, 224, "开｜自动跟到最新",
                    settings.ai_follow_selected == 0),
                kTag, "draw ai follow on option");
            ESP_RETURN_ON_ERROR(
                DrawSettingsOptionCard(
                    88, 156, 224, "关｜保持当前位置",
                    settings.ai_follow_selected == 1),
                kTag, "draw ai follow off option");
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(86, 202, 228, "上下选择  确认保存"), kTag,
                "draw ai follow help");
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
        case wqn::SettingsDialog::kDefaultWordDeck: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("Word 默认词库"), kTag, "draw word deck dialog");
            const auto& options = settings.word_deck_options;
            if (options.empty()) {
                ESP_RETURN_ON_ERROR(
                    DrawCenteredText(86, 140, 228, "词库尚未同步"), kTag, "draw word deck empty");
                break;
            }
            // Scrollable single-select list: the deck count is unbounded, so a
            // selection-following 5-row window instead of fixed option cards.
            constexpr size_t kVisible = 5;
            constexpr int kRowH = 26;
            size_t start = 0;
            if (options.size() > kVisible && settings.word_deck_selected >= kVisible) {
                start = std::min(
                    settings.word_deck_selected + 1 - kVisible,
                    options.size() - kVisible);
            }
            for (size_t i = 0; i < kVisible && start + i < options.size(); ++i) {
                const size_t index = start + i;
                const bool selected = index == settings.word_deck_selected;
                const int y = 92 + static_cast<int>(i) * kRowH;
                if (selected) {
                    // [v2] density word-deck list row selection: rounded reverse-fill.
                    DrawSelectionDecoration(84, y, 232, kRowH - 4, SelectionStyle::kRowFill);
                }
                const std::string& label = options[index].deck_id.empty()
                    ? std::string("全部词库")
                    : options[index].title;
                ESP_RETURN_ON_ERROR(
                    DrawClippedText(92, y + 4, 216, label.empty() ? "(未命名词库)" : label, !selected),
                    kTag, "draw word deck option");
            }
            ESP_RETURN_ON_ERROR(DrawCenteredText(86, 226, 228, "上下选择  确认保存"), kTag, "draw word deck help");
            break;
        }
        case wqn::SettingsDialog::kDevInfo: {
            // [dev-diag] Read-only build/runtime snapshot (DEV_DIAGNOSTICS.md
            // §4.1). Six clipped lines at the kBattery 20px pitch.
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("Dev 信息"), kTag, "draw dev info dialog");
            const std::string dev_version =
                diag.firmware_version.empty() ? WQN_FIRMWARE_VERSION : diag.firmware_version;
            ESP_RETURN_ON_ERROR(
                DrawClippedText(88, 94, 224, "固件 " + dev_version + " @ " + diag.git_commit),
                kTag, "draw dev build id");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(88, 114, 224, "构建 " + diag.build_time), kTag, "draw dev build time");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(88, 134, 224, "复位 " + diag.reset_reason_label),
                kTag, "draw dev reset reason");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(88, 154, 224, "运行 " + diag.uptime_label), kTag, "draw dev uptime");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(
                    88, 174, 224,
                    "堆 " + BytesLabel(diag.heap_free) + " 最小 " + BytesLabel(diag.heap_min_free)),
                kTag, "draw dev heap");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(
                    88, 194, 224,
                    "PSRAM " + BytesLabel(diag.psram_free) + "/" + BytesLabel(diag.psram_total)),
                kTag, "draw dev psram");
            break;
        }
        case wqn::SettingsDialog::kDevSync: {
            // [dev-diag] Sync snapshot pre-formatted at
            // UpdateSettingsDiagnostics time; the dialog only draws the seven
            // prepared lines (DEV_DIAGNOSTICS.md §4.2).
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("同步诊断"), kTag, "draw dev sync dialog");
            for (size_t i = 0; i < diag.sync_diag_lines.size(); ++i) {
                if (diag.sync_diag_lines[i].empty()) {
                    continue;
                }
                ESP_RETURN_ON_ERROR(
                    DrawClippedText(88, 94 + static_cast<int>(i) * 18, 224, diag.sync_diag_lines[i]),
                    kTag, "draw dev sync line");
            }
            break;
        }
        case wqn::SettingsDialog::kDevErrors: {
            // [dev-diag] Recent-error list from the error_recorder ring copy
            // (§4.3). Static like kBattery: no scrolling, closing and
            // reopening takes a fresh snapshot.
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("错误记录"), kTag, "draw dev errors dialog");
            if (diag.error_line_count == 0) {
                ESP_RETURN_ON_ERROR(
                    DrawCenteredText(86, 148, 228, "暂无错误记录"), kTag, "draw dev errors empty");
            } else {
                const size_t shown = std::min<size_t>(diag.error_line_count, 6);
                for (size_t i = 0; i < shown; ++i) {
                    if (!diag.error_lines[i].valid) {
                        continue;
                    }
                    ESP_RETURN_ON_ERROR(
                        DrawClippedText(
                            88, 94 + static_cast<int>(i) * 18, 224, diag.error_lines[i].text),
                        kTag, "draw dev error line");
                }
            }
            break;
        }
        case wqn::SettingsDialog::kSleepDiag: {
            // [dev-diag] Sleep/power diagnostics (DEV_DIAGNOSTICS.md §4.6). The
            // ring lives in RTC slow memory, so this is the on-battery view of
            // it; opening the dialog also asks the power coordinator to dump
            // the full ring to the log, which only helps with a USB console.
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("电源睡眠诊断"), kTag, "draw sleep diag dialog");
            if (diag.sleep_diag_line_count == 0) {
                ESP_RETURN_ON_ERROR(
                    DrawCenteredText(86, 148, 228, "暂无睡眠记录"), kTag, "draw sleep diag empty");
                break;
            }
            for (size_t i = 0; i < diag.sleep_diag_line_count; ++i) {
                if (!diag.sleep_diag_lines[i].valid) {
                    continue;
                }
                ESP_RETURN_ON_ERROR(
                    DrawClippedText(
                        88, 94 + static_cast<int>(i) * 18, 224, diag.sleep_diag_lines[i].text),
                    kTag, "draw sleep diag line");
            }
            break;
        }
        case wqn::SettingsDialog::kFactoryReset:
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("恢复出厂"), kTag, "draw factory reset dialog");
            ESP_RETURN_ON_ERROR(DrawWrappedText(54, 98, 292, "将清除 NVS 中的配对、缓存、待上传、AI 会话、单词进度和设置。", 3), kTag, "draw reset body");
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 176, 312, "这是不可撤销操作"), kTag, "draw reset warning");
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 200, 312, "长按确认执行，短按确认取消"), kTag, "draw reset help");
            break;
        case wqn::SettingsDialog::kPowerOff:
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("关机"), kTag, "draw power off dialog");
            ESP_RETURN_ON_ERROR(DrawWrappedText(54, 98, 292, "将清屏并彻底断电，屏幕保留关机提示。重新开机：长按下方翻页键（下页/电源键）。", 3), kTag, "draw power off body");
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 176, 312, "注意：确认键无法开机"), kTag, "draw power off note");
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 200, 312, "长按确认关机，短按确认取消"), kTag, "draw power off help");
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
        if (!frame.claim_code.empty()) {
            title += " (授权码: " + frame.claim_code + ")";
        } else if (frame.paired) {
            title += " (已配对)";
        } else {
            title += " (授权码获取中)";
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
    const std::string image_render_label =
        wqn::ImageRenderModeLabel(settings.image_render_mode);
    const std::string battery_value =
        diag.full ? "满电" : (diag.charging ? "充电 " + std::to_string(diag.battery_percent) + "%" : std::to_string(diag.battery_percent) + "%");
    const std::string storage_value = "NVS " + std::to_string(diag.nvs_used_entries) + "/" + std::to_string(diag.nvs_total_entries);
    const std::string version_value = diag.firmware_version.empty() ? WQN_FIRMWARE_VERSION : diag.firmware_version;

    const std::string root_titles[kSettingsItemCount] = {
        "WiFi 管理",
        "立即同步",
        "自动同步间隔",
        "电量",
        "图片渲染",
        "音量",
        "Word 默认词库",
        "AI 回复时翻页",
        "固件版本",
#if CONFIG_WQN_DEV_MENU_ENABLE
        "开发者选项",
#endif
        "恢复出厂",
        "关机",
    };
    const std::string root_values[kSettingsItemCount] = {
        settings.wifi_primary_ssid[0] != '\0' ? settings.wifi_primary_ssid : "未配置",
        settings.sync_status.empty() ? "空闲" : settings.sync_status,
        auto_sync_label,
        battery_value,
        image_render_label,
        volume_label,
        settings.default_word_deck_title.empty() ? "全部词库" : settings.default_word_deck_title,
        settings.auto_follow ? "开" : "关",
        version_value,
#if CONFIG_WQN_DEV_MENU_ENABLE
        "只读诊断",
#endif
        "",
        "",
    };
    // [dev-diag] Second-level list (DEV_DIAGNOSTICS.md §3): six rows, which is
    // exactly the panel window, so it never needs to scroll. 电量原始 and
    // 存储详情 are the two detail dialogs that used to sit on the root list.
    const std::string dev_titles[wqn::kDevItemCount] = {
        "Dev 信息",
        "同步诊断",
        "错误记录",
        "电量原始",
        "存储详情",
        "电源睡眠诊断",
    };
    const std::string dev_values[wqn::kDevItemCount] = {
        diag.git_commit,
        diag.sync_diag_summary,
        diag.error_count_label,
        std::to_string(diag.battery_mv) + " mV",
        storage_value,
        diag.sleep_diag_count_label,
    };
    const bool dev_view = settings.view == wqn::SettingsView::kDev;
    const std::string* titles = dev_view ? dev_titles : root_titles;
    const std::string* values = dev_view ? dev_values : root_values;
    const size_t item_count = dev_view ? wqn::kDevItemCount : kSettingsItemCount;
    const size_t selected = dev_view ? settings.dev_selected : settings.selected;
    // Rows no longer fit the 300px panel at the 38px pitch; draw a
    // selection-following window instead (deterministic from the selection so
    // partial refreshes repaint consistently).
    size_t window_start = 0;
    if (selected >= kSettingsVisibleRows) {
        window_start = std::min(
            selected + 1 - kSettingsVisibleRows,
            item_count - kSettingsVisibleRows);
    }
    int y = 42;
    for (size_t i = 0; i < kSettingsVisibleRows && window_start + i < item_count; ++i) {
        const size_t index = window_start + i;
        ESP_RETURN_ON_ERROR(
            DrawSettingsRow(settings.view, index, y, titles[index], values[index], index == selected),
            kTag, "draw settings row");
        y += 38;
    }
    // The dev list is a second level, so its escape hint differs. The title bar
    // above y=32 is outside kSettingsContentRect and is therefore never redrawn
    // on a kConfig partial refresh -- the orientation cue lives here instead.
    const std::string help_line =
        !settings.notice.empty() ? settings.notice
        : dev_view ? std::string("上下选择，确认查看，长按确认返回设置")
                   : std::string("上下选择，确认操作，长按确认返回首页");
    ESP_RETURN_ON_ERROR(
        DrawClippedText(kMarginDense, 274, wqn::kEpdWidth - 2 * kMarginDense, help_line),
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
        // [persist-worker] Preselect an armed-but-unsaved value (submit rejected
        // or write failed) over the durable one so a re-Confirm retries the
        // intended interval; 0 is valid, hence the *_pending_valid flag.
        const uint32_t seed = state->settings.auto_sync_pending_valid
            ? state->settings.pending_auto_sync_minutes
            : state->settings.auto_sync_interval_min;
        state->settings.auto_sync_selected = AutoSyncOptionIndex(seed);
    } else if (dialog == wqn::SettingsDialog::kVolume) {
        const int seed = state->settings.volume_pending_valid
            ? state->settings.pending_volume_percent
            : state->settings.volume_percent;
        state->settings.volume_selected = VolumeOptionIndex(seed);
    } else if (dialog == wqn::SettingsDialog::kImageRendering) {
        const wqn::ImageRenderMode seed =
            state->settings.image_render_pending_valid
            ? state->settings.pending_image_render_mode
            : state->settings.image_render_mode;
        state->settings.image_render_selected =
            seed == wqn::ImageRenderMode::kBlackWhite ? 0 : 1;
    } else if (dialog == wqn::SettingsDialog::kAiFollow) {
        // Focus the current value -- the same one the row displays.
        state->settings.ai_follow_selected =
            state->settings.auto_follow ? 0 : 1;
    } else if (dialog == wqn::SettingsDialog::kDefaultWordDeck) {
        // Option 0 is the fixed 全部词库; the rest mirror the mounted deck
        // catalog. Preselect an armed-but-unsaved switch (submit rejected or
        // transaction failed) over the installed default so a re-Confirm
        // retries the intended deck.
        auto& settings = state->settings;
        const std::string& preselect_id = settings.word_deck_pending_valid
            ? settings.pending_word_deck_id
            : state->word_app.default_deck_id;
        settings.word_deck_options.clear();
        settings.word_deck_options.push_back(wqn::WordDeckInfo{});
        settings.word_deck_selected = 0;
        for (const wqn::WordDeckInfo& deck : state->word_app.deck_catalog) {
            if (deck.deck_id == preselect_id) {
                settings.word_deck_selected = settings.word_deck_options.size();
            }
            settings.word_deck_options.push_back(deck);
        }
    }
}

}  // namespace device_ui_internal
