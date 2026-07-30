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

esp_err_t DrawSettingsRow(size_t row_index, int y, const std::string& title, const std::string& value, bool selected)
{
    constexpr int kX = 12;
    constexpr int kContentX = kX + 7;            // row body starts after the index gutter
    constexpr int kWidth = 376;
    constexpr int kContentWidth = kWidth - 7;
    constexpr int kHeight = 36;
    // Index gutter: the row's 2-digit index is also the selection marker. When
    // selected the index sits in a reverse-filled rounded block (the unified
    // kInvert focus language, rounded chip shape) so the left gutter is no
    // longer empty and the active row is obvious; unselected shows the plain
    // index with no block.
    constexpr int kIndexBlockX = kContentX + 4;  // 4px inside the row body
    constexpr int kIndexBlockYOff = 6;            // offset from row top, centered in 36h
    constexpr int kIndexBlockW = 36;
    constexpr int kIndexBlockH = 24;
    // Row outline: kInnerBorder when selected, plain DrawRect otherwise.
    if (selected) {
        DrawSelectionDecoration(kContentX, y, kContentWidth, kHeight, SelectionStyle::kInnerBorder);
        // Index block: reverse-fill rounded chip behind the index number.
        DrawSelectionDecoration(kIndexBlockX, y + kIndexBlockYOff, kIndexBlockW, kIndexBlockH, SelectionStyle::kInvert);
    } else {
        DrawRect(kContentX, y, kContentWidth, kHeight);
    }
    char index_label[4] = {};
    std::snprintf(index_label, sizeof(index_label), "%02u", static_cast<unsigned>(row_index + 1));
    if (selected) {
        // Index number in paper (white) on the reverse-fill block, centered in it.
        ESP_RETURN_ON_ERROR(DrawCenteredText(kIndexBlockX, y + kIndexBlockYOff + 4, kIndexBlockW, index_label, false), kTag, "draw settings index");
    } else {
        // Unselected: plain ink index in its original position (left-aligned).
        ESP_RETURN_ON_ERROR(DrawClippedText(kX + 17, y + 10, 28, index_label), kTag, "draw settings index");
    }
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
        tag = "设置";  // default word deck
    } else if (row_index == 6) {
        tag = "系统";  // firmware version
    } else if (row_index == 7) {
        tag = "重置";  // factory reset
    }
    DrawRoundedRect(kX + 298, y + 8, 54, 20, kChipRadius);
    ESP_RETURN_ON_ERROR(DrawCenteredText(kX + 298, y + 10, 54, tag), kTag, "draw settings tag");
    return ESP_OK;
}

esp_err_t DrawSettingsDialogBox(const std::string& title)
{
    FillRect(68, 48, 264, 206, false);
    // Dialog container: rounded double outline (the product's rounded dialog
    // language, same r6 as cards/containers).
    DrawRoundedRect(68, 48, 264, 206, kRoundedOuterRadius);
    DrawRoundedRect(70, 50, 260, 202, kRoundedOuterRadius);
    DrawHorizontalLine(86, 80, 228);
    DrawHorizontalLine(86, 220, 228);
    ESP_RETURN_ON_ERROR(DrawCenteredText(86, 60, 228, title), kTag, "draw settings dialog title");
    return DrawCenteredText(86, 230, 228, "确认关闭");
}

esp_err_t DrawSettingsOptionCard(int x, int y, int width, const std::string& label, bool selected)
{
    constexpr int kHeight = 32;
    // Focus: kRoundedInnerBorder (rounded chip-like card, low flicker) when
    // selected, plain rounded outline otherwise. These are the small 96x32
    // dialog option chips, so they follow the chip/card rounded language.
    if (selected) {
        DrawSelectionDecoration(x, y, width, kHeight, SelectionStyle::kRoundedInnerBorder);
    } else {
        DrawRoundedRect(x, y, width, kHeight, kChipRadius);
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
                    DrawSelectedFill(84, y, 232, kRowH - 4);
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
        "Word 默认词库",
        "固件版本",
        "恢复出厂",
    };
    const std::string values[kSettingsItemCount] = {
        settings.sync_status.empty() ? "空闲" : settings.sync_status,
        auto_sync_label,
        battery_value,
        storage_value,
        volume_label,
        settings.default_word_deck_title.empty() ? "全部词库" : settings.default_word_deck_title,
        version_value,
        "",
    };

    // Eight rows no longer fit the 300px panel at the 38px pitch; draw a
    // selection-following window instead (deterministic from the selection so
    // partial refreshes repaint consistently).
    size_t window_start = 0;
    if (settings.selected >= kSettingsVisibleRows) {
        window_start = std::min(
            settings.selected + 1 - kSettingsVisibleRows,
            kSettingsItemCount - kSettingsVisibleRows);
    }
    int y = 42;
    for (size_t i = 0; i < kSettingsVisibleRows && window_start + i < kSettingsItemCount; ++i) {
        const size_t index = window_start + i;
        ESP_RETURN_ON_ERROR(
            DrawSettingsRow(index, y, titles[index], values[index], index == settings.selected),
            kTag, "draw settings row");
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
    } else if (dialog == wqn::SettingsDialog::kDefaultWordDeck) {
        // Option 0 is the fixed 全部词库; the rest mirror the mounted deck
        // catalog. Preselect the current default when it is still mounted.
        auto& settings = state->settings;
        settings.word_deck_options.clear();
        settings.word_deck_options.push_back(wqn::WordDeckInfo{});
        settings.word_deck_selected = 0;
        for (const wqn::WordDeckInfo& deck : state->word_app.deck_catalog) {
            if (deck.deck_id == state->word_app.default_deck_id) {
                settings.word_deck_selected = settings.word_deck_options.size();
            }
            settings.word_deck_options.push_back(deck);
        }
    }
}

}  // namespace device_ui_internal
