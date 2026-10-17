// Settings B: native typography, paper lists, full-page details, no help footer.
#include "ui_internal.h"
#include "ui_fonts.h"
#include "ui/assets/brand_logo.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace device_ui_internal {
namespace {

const lv_font_t* SettingsFont(int size)
{
    if (size == 12) return &wqn_ui_sans_12;
    if (size == 20) return &wqn_ui_sans_20;
    if (size == 24) return &wqn_ui_sans_24;
    if (size == 32) return &wqn_ui_latin_32;
    if (size == 48) return &wqn_ui_latin_48;
    return wqn::PrimaryUiFont();
}

void RemoveLastCodepoint(std::string* text)
{
    if (text->empty()) return;
    size_t end = text->size() - 1;
    while (end && (static_cast<unsigned char>((*text)[end]) & 0xc0) == 0x80) --end;
    text->resize(end);
}

std::string SettingsClip(const std::string& value, int width, const lv_font_t* font)
{
    if (width <= 0) return {};
    std::string text = value;
    std::replace_if(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }, ' ');
    if (wqn::MeasureTextWithFont(font, text.c_str()) <= width) return text;
    while (!text.empty() && wqn::MeasureTextWithFont(font, (text + "...").c_str()) > width)
        RemoveLastCodepoint(&text);
    return wqn::MeasureTextWithFont(font, "...") <= width ? text + "..." : "";
}

void SettingsText(int x, int baseline, const std::string& value, int size = 16,
                  int width = kSettingsDetailWidth, bool right = false, bool ink = true,
                  const lv_font_t* face = nullptr)
{
    const lv_font_t* font = face ? face : SettingsFont(size);
    const std::string text = SettingsClip(value, width, font);
    const int start = right ? x - wqn::MeasureTextWithFont(font, text.c_str()) : x;
    wqn::DrawTextWithFont(start, baseline - font->line_height + font->base_line,
                          font, text.c_str(), ink);
}

void SettingsLine(int y, int x = kSettingsDetailX, int width = kSettingsDetailWidth)
{
    DrawHorizontalLine(x, y, width);
}

void SettingsHeading(const wqn::UiFrame& frame, const std::string& section,
                     const std::string& title, const std::string& subtitle)
{
    SettingsText(kSettingsDetailX, kSettingsHeaderTextBaseline, section, 16, 280);
    DrawStatusBarIcons(kSettingsRight, kStatusBarIconY, frame.home);
    DrawHorizontalLine(0, kSettingsHeaderDividerY, kScreenWidth);
    SettingsText(kSettingsDetailX, kSettingsTitleBaseline, title, 24);
    SettingsText(kSettingsDetailX, kSettingsSubtitleBaseline,
                 frame.settings.notice.empty() ? subtitle : frame.settings.notice, 12);
    SettingsLine(kSettingsContentDividerY);
}

void SettingsWrapped(const std::string& value, int baseline = 177, int max_lines = 3)
{
    std::string row;
    int used = 0;
    for (size_t i = 0; i < value.size();) {
        const unsigned char byte = value[i];
        const size_t step = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        const std::string cp = value.substr(i, step);
        i += step;
        if (cp == "\n" || wqn::MeasureTextWithFont(SettingsFont(16), (row + cp).c_str()) > kSettingsDetailWidth) {
            SettingsText(kSettingsDetailX, baseline + used * 25, row);
            row.clear();
            if (++used >= max_lines) return;
        }
        if (cp != "\n") row += cp;
    }
    if (!row.empty() && used < max_lines) SettingsText(kSettingsDetailX, baseline + used * 25, row);
}

void SettingsEmpty(const std::string& title, const std::string& body)
{
    SettingsText(kSettingsDetailX, 157, title, 24);
    SettingsWrapped(body, 190);
}

void SettingsCheck(int x, int y, bool saved)
{
    DrawRect(x, y, 18, 18);
    if (!saved) return;
    FillRect(x, y, 18, 18, true);
    for (int i = 0; i < 4; ++i) FillRect(x + 4 + i, y + 8 + i, 2, 2, false);
    for (int i = 0; i < 7; ++i) FillRect(x + 7 + i, y + 11 - i, 2, 2, false);
}

void SettingsOption(size_t index, const std::string& label, const std::string& note,
                    bool saved, bool focused, int top = kSettingsOptionTop,
                    int pitch = kSettingsOptionPitch)
{
    const int y = top + static_cast<int>(index) * pitch;
    SettingsCheck(kSettingsDetailX, y + 4, saved);
    SettingsText(kSettingsOptionTextX, y + 21, label, 20, kSettingsRight - kSettingsOptionTextX);
    if (!note.empty()) SettingsText(kSettingsOptionTextX, y + 43, note, 16, kSettingsRight - kSettingsOptionTextX);
    FillRect(kSettingsOptionTextX, y + pitch - 2, kSettingsRight - kSettingsOptionTextX, focused ? 2 : 1, true);
}

void SettingsValue(const std::string& label, const std::string& value, int baseline)
{
    SettingsText(kSettingsDetailX, baseline, label, 16, 156);
    SettingsText(kSettingsRight, baseline, value, 16, 188, true);
    SettingsLine(baseline + 9);
}

void SettingsBar(int x, int y, int width, int height, bool known, size_t used, size_t total)
{
    DrawRect(x, y, width, height);
    if (known && total) {
        const int filled = static_cast<int>(static_cast<uint64_t>(width - 6) * std::min(used, total) / total);
        FillRect(x + 3, y + 3, filled, std::max(1, height - 6), true);
    } else SettingsLine(y + height / 2, x + 4, width - 8);
}

std::string MiBLabel(size_t value)
{
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%u.%u MiB", static_cast<unsigned>(value / 1048576),
                  static_cast<unsigned>((value % 1048576) * 10 / 1048576));
    return text;
}

std::string SettingsBatteryValue(const wqn::SettingsDiagnosticsSnapshot& d)
{
    return d.battery_valid ? std::to_string(std::clamp(d.battery_percent, 0, 100)) + "%" : "--%";
}

std::string SettingsIntervalLabel(uint32_t minutes)
{
    if (!minutes) return "关闭自动同步";
    if (minutes >= 60 && minutes % 60 == 0) return "每 " + std::to_string(minutes / 60) + " 小时";
    return "每 " + std::to_string(minutes) + " 分钟";
}

void DrawSettingsMenu(const wqn::UiFrame& frame)
{
    const auto& s = frame.settings;
    const auto& d = s.diagnostics;
    const bool dev = s.view == wqn::SettingsView::kDev;
    const char* root_titles[] = {"WiFi 管理", "立即同步", "自动同步间隔", "电量", "图片渲染方式",
        "音量", "默认词库", "AI 回复时翻页", "关于设备",
#if CONFIG_WQN_DEV_MENU_ENABLE
        "开发者选项",
#endif
        "恢复出厂", "关机"};
    static_assert(sizeof(root_titles) / sizeof(root_titles[0]) == wqn::kSettingsItemCount);
    const char* dev_titles[] = {"构建与运行信息", "同步诊断", "错误记录", "电量原始", "存储详情", "睡眠诊断"};
    const size_t count = dev ? wqn::kDevItemCount : wqn::kSettingsItemCount;
    const size_t selected = std::min(dev ? s.dev_selected : s.selected, count - 1);
    SettingsText(kSettingsDetailX, kSettingsHeaderTextBaseline, dev ? "设置 / 开发者" : frame.paired ? "设备设置 · 已配对" : "设备设置 · 待配对", 16, 280);
    DrawStatusBarIcons(kSettingsRight, kStatusBarIconY, frame.home);
    DrawHorizontalLine(0, kSettingsHeaderDividerY, kScreenWidth);
    SettingsText(kSettingsListX, kSettingsTitleBaseline, dev ? "开发者选项" : "设置", 24, 275);
    char position[20] = {};
    std::snprintf(position, sizeof(position), "%02u / %u", static_cast<unsigned>(selected + 1), static_cast<unsigned>(count));
    SettingsText(kSettingsRight, kSettingsTitleBaseline, position, 16, 95, true);
    SettingsText(kSettingsListX, kSettingsSubtitleBaseline, s.notice.empty() ? (dev ? "设备运行信息与诊断" : "学习、设备与同步") : s.notice, 12, kSettingsDetailWidth);
    SettingsLine(kSettingsContentDividerY, kSettingsListX, kSettingsDetailWidth);
    const size_t start = selected < kSettingsVisibleRows ? 0 : std::min(selected + 1 - kSettingsVisibleRows, count - kSettingsVisibleRows);
    for (size_t i = 0; i < kSettingsVisibleRows && start + i < count; ++i) {
        const size_t index = start + i;
        const int y = kSettingsRowTop + static_cast<int>(i) * kSettingsRowPitch;
        char number[4] = {};
        std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(index + 1));
        if (index == selected) FillRect(kSettingsListX, y + 7, 22, 22, true);
        SettingsText(kSettingsListX + 4, y + 23, number, 12, 18, false, index != selected);
        SettingsText(kSettingsTextX, y + 25, dev ? dev_titles[index] : root_titles[index], 20, 190);
        if (!dev) {
            std::string value;
            switch (index) {
                case wqn::kSettingsRowWifi: value = frame.home.wifi_connected ? "已连接" : s.wifi_primary_ssid[0] ? "未连接" : "未配置"; break;
                case wqn::kSettingsRowSyncNow: value = s.sync_status.empty() ? "空闲" : s.sync_status; break;
                case wqn::kSettingsRowAutoSync: value = s.auto_sync_interval_min ? SettingsIntervalLabel(s.auto_sync_interval_min) : "关闭"; break;
                case wqn::kSettingsRowBattery: value = SettingsBatteryValue(d); break;
                case wqn::kSettingsRowImageRender: value = s.image_render_mode == wqn::ImageRenderMode::kBlackWhite ? "黑白" : "16 阶灰度"; break;
                case wqn::kSettingsRowVolume: value = wqn::VolumeLabel(s.volume_percent); break;
                case wqn::kSettingsRowWordDeck: value = s.default_word_deck_title.empty() ? "全部词库" : s.default_word_deck_title; break;
                case wqn::kSettingsRowAiFollow: value = s.auto_follow ? "自动跟随" : "保持位置"; break;
                case wqn::kSettingsRowAbout: value = "Note4"; break;
#if CONFIG_WQN_DEV_MENU_ENABLE
                case wqn::kSettingsRowDevMenu: value = "诊断与记录"; break;
#endif
                default: break;
            }
            SettingsText(kSettingsRight - 18, y + 24, value, 16, 132, true);
        }
        for (int p = 0; p < 5; ++p) {
            wqn::DrawEpdPixel(kSettingsRight - 5 + p, y + 13 + p, true);
            wqn::DrawEpdPixel(kSettingsRight - 5 + p, y + 21 - p, true);
        }
        SettingsLine(y + kSettingsRowPitch - 1, kSettingsListX, kSettingsDetailWidth);
    }
    if (count > kSettingsVisibleRows) {
        FillRect(397, kSettingsRowTop + 5, 1, 170, true);
        FillRect(396, kSettingsRowTop + 5 + static_cast<int>(start * 102 / (count - kSettingsVisibleRows)), 3, 68, true);
    }
}

void DrawSettingsDetail(const wqn::UiFrame& frame)
{
    const auto& s = frame.settings;
    const auto& d = s.diagnostics;
    using Dialog = wqn::SettingsDialog;
    switch (s.dialog) {
        case Dialog::kAutoSync:
            SettingsHeading(frame, "设置 / 学习", "自动同步间隔", "选择自动获取学习内容的频率");
            for (size_t i = 0; i < kAutoSyncOptionsCount; ++i)
                SettingsOption(i, SettingsIntervalLabel(kAutoSyncOptions[i]), "", kAutoSyncOptions[i] == s.auto_sync_interval_min, i == s.auto_sync_selected);
            break;
        case Dialog::kVolume:
            SettingsHeading(frame, "设置 / 学习", "音量", "选择设备播放音量");
            for (size_t i = 0; i < kVolumeOptionsCount; ++i)
                SettingsOption(i, wqn::VolumeLabel(kVolumeOptions[i]), "", kVolumeOptions[i] == s.volume_percent, i == s.volume_selected);
            break;
        case Dialog::kImageRendering:
            SettingsHeading(frame, "设置 / 显示", "图片渲染方式", "选择墨水屏图片的显示方式");
            SettingsOption(0, "黑白", "快速、省电", s.image_render_mode == wqn::ImageRenderMode::kBlackWhite, s.image_render_selected == 0, kSettingsOptionTop, 62);
            SettingsOption(1, "16 阶灰度", "保留更多明暗细节", s.image_render_mode == wqn::ImageRenderMode::kGray16, s.image_render_selected == 1, kSettingsOptionTop, 62);
            break;
        case Dialog::kAiFollow:
            SettingsHeading(frame, "设置 / AI", "AI 回复时翻页", "选择新回复到达时的阅读位置");
            SettingsOption(0, "自动跟到最新", "跟随正在生成的回复", s.auto_follow, s.ai_follow_selected == 0, kSettingsOptionTop, 62);
            SettingsOption(1, "保持当前位置", "继续阅读当前内容", !s.auto_follow, s.ai_follow_selected == 1, kSettingsOptionTop, 62);
            break;
        case Dialog::kDefaultWordDeck: {
            SettingsHeading(frame, "设置 / 学习", "默认词库", "选择进入单词页时使用的词库");
            if (s.word_deck_status != wqn::SettingsDeckStatus::kReady) {
                if (s.word_deck_status == wqn::SettingsDeckStatus::kLoading)
                    SettingsEmpty("正在同步词库", "同步完成后显示可选词库。");
                else if (s.word_deck_status == wqn::SettingsDeckStatus::kFailed)
                    SettingsEmpty("词库列表未取得", "确认重试，或稍后再次同步。");
                else SettingsEmpty("词库尚未同步", "确认请求同步学习内容。");
                break;
            }
            const auto& options = s.word_deck_options;
            if (options.empty()) { SettingsEmpty("词库尚未同步", "先同步学习内容，再来选择默认词库。"); break; }
            const size_t selected = std::min(s.word_deck_selected, options.size() - 1);
            const size_t start = selected < 5 ? 0 : std::min(selected - 4, options.size() - 5);
            SettingsText(kSettingsRight, kSettingsTitleBaseline, std::to_string(selected + 1) + "/" + std::to_string(options.size()), 12, 70, true);
            for (size_t i = 0; i < 5 && start + i < options.size(); ++i) {
                const auto& option = options[start + i];
                const std::string title = option.deck_id.empty() ? "全部词库" : option.title.empty() ? "未命名词库" : option.title;
                const bool saved = option.deck_id == s.default_word_deck_id;
                SettingsOption(i, title, "", saved, start + i == selected);
            }
            break;
        }
        case Dialog::kWifiManage: {
            const bool configured = s.wifi_primary_ssid[0] != '\0';
            SettingsHeading(frame, "设置 / 网络", "WiFi 管理", frame.home.wifi_connected ? "网络已连接" : configured ? "暂未连接网络" : "尚未配置网络");
            SettingsText(kSettingsDetailX, 125, "主网络", 12); SettingsText(kSettingsDetailX, 149, configured ? s.wifi_primary_ssid : "未配置"); SettingsLine(161);
            SettingsText(kSettingsDetailX, 181, "备用网络", 12); SettingsText(kSettingsDetailX, 205, s.wifi_backup_ssid[0] ? s.wifi_backup_ssid : "未配置");
            for (size_t i = 0; i < 2; ++i) {
                const int y = 224 + static_cast<int>(i) * 37;
                const bool focused = s.wifi_action_selected == i;
                if (focused) FillRect(kSettingsDetailX, y, kSettingsDetailWidth, 30, true); else DrawRect(kSettingsDetailX, y, kSettingsDetailWidth, 30);
                SettingsText(kSettingsDetailX + 12, y + 22, i == 0 ? (configured ? "重新配网" : "开始配网") : "设备配对", 16, 328, false, !focused);
            }
            break;
        }
        case Dialog::kPairing:
            SettingsHeading(frame, "设置 / 网络", "设备配对", "在账号的设备页输入配对码");
            if (frame.paired) SettingsEmpty("已配对", "设备已与账号关联。");
            else if (!frame.home.wifi_connected) SettingsEmpty("先连接网络", "连接 WiFi 后可获取配对码。");
            else if (frame.claim_code.empty()) SettingsEmpty("正在获取配对码", "联网后自动获取，请稍候。");
            else { SettingsText(kSettingsDetailX, 157, frame.claim_code, 32); SettingsText(kSettingsDetailX, 191, "请在浏览器中完成配对"); }
            break;
        case Dialog::kProvisioningResult:
            SettingsHeading(frame, "设置 / 网络", "手机配网", "使用手机完成网络配置");
            if (frame.provisioning_phase == wqn::ProvisionState::kFailed)
                SettingsEmpty("配网未完成", "确认重试，或长按返回 WiFi 管理。");
            else SettingsEmpty("配置已保存", "确认返回 WiFi 管理。");
            break;
        case Dialog::kAbout:
            SettingsText(kSettingsDetailX, kSettingsHeaderTextBaseline, "关于设备", 16); DrawStatusBarIcons(kSettingsRight, kStatusBarIconY, frame.home); DrawHorizontalLine(0, kSettingsHeaderDividerY, kScreenWidth);
            DrawWqnBitmapAsset(kSettingsDetailX, 37, kWLogo, true);
            SettingsText(kSettingsDetailX + 26, 62, "rongNoteBook", 24, kSettingsDetailWidth - 26, false, true, &wqn_ui_latin_24);
            SettingsText(kSettingsDetailX, 105, "Note4", 32);
            SettingsText(kSettingsDetailX, 130, "4.2 英寸墨水屏 · 400 × 300", 12); SettingsLine(149);
            SettingsValue("固件版本", d.firmware_version.empty() ? WQN_FIRMWARE_VERSION : d.firmware_version, 179);
            SettingsValue("处理器", d.idf_target == "esp32s3" ? "ESP32-S3" : d.idf_target, 211);
            SettingsValue("运行环境", "ESP-IDF " + d.idf_version, 243);
            break;
        case Dialog::kBatterySummary: {
            SettingsHeading(frame, "设置 / 设备", "电量详情", "查看剩余电量与充电状态");
            const std::string value = d.battery_valid ? std::to_string(std::clamp(d.battery_percent, 0, 100)) : "--";
            SettingsText(kSettingsDetailX, 166, value, 48, 180);
            SettingsText(kSettingsDetailX + 10 + wqn::MeasureTextWithFont(SettingsFont(48), value.c_str()), 165, "%", 16, 30);
            SettingsBar(215, 137, kSettingsRight - 215, 14, d.battery_valid, std::clamp(d.battery_percent, 0, 100), 100);
            SettingsText(kSettingsDetailX, 206, !d.battery_valid ? "暂未取得电量读数" : d.charging ? "正在充电" : d.full && d.external_power_present ? "电池已充满" : "未充电，运行正常");
            SettingsLine(223); SettingsText(kSettingsDetailX, 250, "充电状态与电量读数分别显示", 12);
            break;
        }
        case Dialog::kBattery:
            SettingsHeading(frame, "设置 / 开发者", "电量原始", "设备运行快照");
            SettingsValue("ADC 原始", d.battery_valid ? std::to_string(d.adc_raw) : "--", 129);
            SettingsValue("ADC 电压", d.battery_valid ? std::to_string(d.adc_mv) + " mV" : "--", 154);
            SettingsValue("电池电压", d.battery_valid ? std::to_string(d.battery_mv) + " mV" : "--", 179);
            SettingsValue("电量", SettingsBatteryValue(d), 204);
            SettingsValue("正在充电", !d.battery_valid ? "--" : d.charging ? "是" : "否", 229);
            SettingsValue("已充满", !d.battery_valid ? "--" : d.full ? "是" : "否", 254);
            SettingsText(kSettingsDetailX, 285, "(-V^2+9016V-19189000)/10000", 12);
            break;
        case Dialog::kStorage:
            SettingsHeading(frame, "设置 / 开发者", "存储详情", "容量、条目数与运行内存");
            SettingsValue("Flash 容量", d.flash_valid ? MiBLabel(d.flash_size) : "--", 128);
            SettingsText(kSettingsDetailX, 166, "NVS 条目"); SettingsText(kSettingsRight, 166, d.nvs_valid ? std::to_string(d.nvs_used_entries) + " / " + std::to_string(d.nvs_total_entries) : "-- / --", 16, 180, true);
            SettingsBar(kSettingsDetailX, 179, kSettingsDetailWidth, 10, d.nvs_valid, d.nvs_used_entries, d.nvs_total_entries);
            SettingsText(kSettingsDetailX, 217, "PSRAM 运行内存"); SettingsText(kSettingsRight, 240, d.psram_valid ? MiBLabel(d.psram_used) + " / " + MiBLabel(d.psram_total) : "-- / --", 16, 300, true);
            SettingsBar(kSettingsDetailX, 248, kSettingsDetailWidth, 10, d.psram_valid, d.psram_used, d.psram_total);
            SettingsText(kSettingsDetailX, 286, d.content_sync_label, 12);
            break;
        case Dialog::kDevInfo:
            SettingsHeading(frame, "设置 / 开发者", "构建与运行信息", "设备运行快照");
            SettingsValue("固件", d.firmware_version, 129); SettingsValue("提交", d.git_commit, 154);
            SettingsValue("构建", d.build_time, 179); SettingsValue("复位原因", d.reset_reason_label, 204);
            SettingsValue("运行时长", d.uptime_label, 229); SettingsValue("可用堆", BytesLabel(d.heap_free), 254);
            SettingsText(kSettingsDetailX, 286, "最小堆 " + BytesLabel(d.heap_min_free) + "  PSRAM " + BytesLabel(d.psram_free), 12);
            break;
        case Dialog::kDevSync:
            SettingsHeading(frame, "设置 / 开发者", "同步诊断", "同步状态与最近一次结果");
            for (size_t i = 0; i < d.sync_diag_lines.size(); ++i) {
                SettingsText(kSettingsDetailX, 127 + static_cast<int>(i) * 22, d.sync_diag_lines[i]);
                SettingsLine(130 + static_cast<int>(i) * 22);
            }
            break;
        case Dialog::kDevErrors:
        case Dialog::kSleepDiag: {
            const bool errors = s.dialog == Dialog::kDevErrors;
            SettingsHeading(frame, "设置 / 开发者", errors ? "错误记录" : "睡眠诊断", "设备运行快照");
            const size_t count = errors ? d.error_line_count : d.sleep_diag_line_count;
            if (!count) SettingsEmpty(errors ? "暂无错误记录" : "暂无睡眠记录", "新的记录会显示在这里。");
            for (size_t i = 0; i < std::min<size_t>(count, 6); ++i) {
                const bool valid = errors ? d.error_lines[i].valid : d.sleep_diag_lines[i].valid;
                if (!valid) continue;
                SettingsText(kSettingsDetailX, 129 + static_cast<int>(i) * 25, errors ? d.error_lines[i].text : d.sleep_diag_lines[i].text);
                SettingsLine(138 + static_cast<int>(i) * 25);
            }
            break;
        }
        case Dialog::kFactoryReset:
        case Dialog::kPowerOff: {
            const bool reset = s.dialog == Dialog::kFactoryReset;
            SettingsHeading(frame, "设置 / 系统", reset ? "恢复出厂" : "关机", reset ? "将清除本机学习与配对数据" : "结束使用并关闭设备");
            FillRect(kSettingsDetailX, 119, 28, 28, true);
            SettingsText(kSettingsDetailX + 8, 141, "!", 24, 20, false, false, &wqn_ui_latin_24);
            SettingsText(kSettingsDetailX + 38, 140, reset ? "此操作不可撤销" : "确认键无法重新开机", 16, 310);
            SettingsWrapped(reset ? "配对、缓存、待上传内容、AI 会话、\n单词进度和设置将被清除。" : "将清屏并关闭电源。\n重新开机：长按下方翻页键。");
            break;
        }
        case Dialog::kNone: break;
    }
}

}  // namespace

esp_err_t RenderSettingsToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    // Compose the complete framebuffer even on config/selection updates: the
    // section header changes with the page, and a cold framebuffer has no base.
    // RefreshStableRegion retains the existing display-owner/waveform policy.
    wqn::ClearEpdFramebuffer(true);
    if (frame.settings.dialog == wqn::SettingsDialog::kNone) DrawSettingsMenu(frame);
    else DrawSettingsDetail(frame);
    if (!frame.navigation.visible && (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig))
        return RefreshStableRegion(kSettingsContentRect, schedule);
    return RefreshFrame(frame, schedule);
}

esp_err_t RenderProvisioningToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    wqn::ClearEpdFramebuffer(true);
    SettingsHeading(frame, "设置 / 网络", "手机配网", "使用手机完成网络配置");
    if (frame.provisioning_phase == wqn::ProvisionState::kStarting)
        SettingsEmpty("正在准备热点", "请稍候，设备正在启动配网。");
    else if (frame.provisioning_phase == wqn::ProvisionState::kFailed)
        SettingsEmpty("热点未能启动", "确认重试，检查网络配置。");
    else if (frame.provisioning_phase == wqn::ProvisionState::kConnected)
        SettingsEmpty("配置已保存", "正在连接网络，请稍候。");
    else {
        SettingsText(kSettingsDetailX, 126, "1  连接设备热点"); SettingsText(kSettingsDetailX + 22, 153, frame.provisioning_ssid, 16, 330); SettingsLine(166);
        SettingsText(kSettingsDetailX, 190, "2  打开浏览器地址"); SettingsText(kSettingsDetailX + 22, 216, "192.168.4.1", 16, 330);
        SettingsText(kSettingsDetailX, 250, "保存 WiFi 后返回设备", 12);
    }
    return RefreshFrame(frame, schedule);
}

bool UpdateSettingsDeckStatus(wqn::UiState* state)
{
    auto& settings = state->settings;
    const auto& word = state->word_app;
    const auto status = !word.deck_catalog.empty() || (word.pack_index.has_manifest && !word.pack_index.pack_error)
        ? wqn::SettingsDeckStatus::kReady
        : IsWordPackCloudBusy() ? wqn::SettingsDeckStatus::kLoading
        : word.cloud_sync_failed || word.pack_index.pack_error ? wqn::SettingsDeckStatus::kFailed
        : wqn::SettingsDeckStatus::kEmpty;
    const bool changed = settings.word_deck_status != status;
    settings.word_deck_status = status;
    return changed;
}

void InstallSettingsDeckOptions(wqn::UiState* state, bool preserve_focus)
{
    auto& settings = state->settings;
    std::string selected_id = settings.word_deck_pending_valid ? settings.pending_word_deck_id : state->word_app.default_deck_id;
    if (preserve_focus && settings.word_deck_selected < settings.word_deck_options.size())
        selected_id = settings.word_deck_options[settings.word_deck_selected].deck_id;
    settings.word_deck_options.clear();
    settings.word_deck_options.push_back(wqn::WordDeckInfo{});
    settings.word_deck_selected = 0;
    for (const auto& deck : state->word_app.deck_catalog) {
        if (deck.deck_id == selected_id) settings.word_deck_selected = settings.word_deck_options.size();
        settings.word_deck_options.push_back(deck);
    }
    UpdateSettingsDeckStatus(state);
}

void OpenSettingsDialog(wqn::UiState* state, wqn::SettingsDialog dialog)
{
    if (state == nullptr) {
        return;
    }
    // Every entry, including the WiFi -> pairing -> WiFi path, shares the
    // Prepare/reserve/apply gate before reading durable settings snapshots.
    if (AnyLocalPersistPending(*state)) {
        state->word_app.session.batch_flush_requested = true;
        state->settings.notice = "正在保存，请稍后";
        return;
    }
    UpdateSettingsDiagnostics(state);
    state->settings.dialog = dialog;
    state->settings.notice.clear();
    state->settings.sync_confirm_pending = false;
    state->settings.default_word_deck_id = state->word_app.default_deck_id;
    if (dialog == wqn::SettingsDialog::kWifiManage) state->settings.wifi_action_selected = 0;
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
        // [ai-follow] Preselect an armed-but-unsaved choice (submit rejected or
        // write failed) over the durable one so a re-Confirm retries the
        // intended value; the durable one is what the row displays.
        const bool seed = state->settings.auto_follow_pending_valid
            ? state->settings.pending_auto_follow
            : state->settings.auto_follow;
        state->settings.ai_follow_selected = seed ? 0 : 1;
    } else if (dialog == wqn::SettingsDialog::kDefaultWordDeck) {
        // Option 0 is the fixed 全部词库; the rest mirror the mounted deck
        // catalog. Preselect an armed-but-unsaved switch (submit rejected or
        // transaction failed) over the installed default so a re-Confirm
        // retries the intended deck.
        InstallSettingsDeckOptions(state, false);
    }
}

}  // namespace device_ui_internal
