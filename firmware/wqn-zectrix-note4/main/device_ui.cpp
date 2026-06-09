#include "device_ui.h"

#include <string>
#include <vector>

#include "ai_session.h"
#include "button_input.h"
#include "config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "epd_display.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "online_sync.h"
#include "power_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys/time.h"
#include "storage.h"
#include "ui_model.h"
#include "wifi_manager.h"
#include "word_pack.h"
#include "wqn_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <new>

namespace {

constexpr char kTag[] = "wqn_ui";
constexpr TickType_t kUiPollDelay = pdMS_TO_TICKS(50);
constexpr TickType_t kStatusRefreshDelay = pdMS_TO_TICKS(60000);
constexpr TickType_t kCommitRefreshDelay = pdMS_TO_TICKS(120);
constexpr TickType_t kClockRefreshDelay = pdMS_TO_TICKS(80);
constexpr TickType_t kTimerRefreshDelay = pdMS_TO_TICKS(80);
constexpr TickType_t kSelectionRefreshDelay = pdMS_TO_TICKS(180);
constexpr TickType_t kConfigRefreshDelay = pdMS_TO_TICKS(120);
constexpr TickType_t kAiRefreshDelay = pdMS_TO_TICKS(120);
constexpr int kEpdTextWidth = wqn::kEpdWidth - 12;
constexpr int64_t kRepeatedLongPressMinDurationMs = 1150;
constexpr size_t kWrappedBodyMaxLines = 4;
constexpr std::time_t kMinReasonableUnixTime = 1704067200;  // 2024-01-01 UTC
constexpr gpio_num_t kChargeDetect = GPIO_NUM_2;            // CHRG_L: low means charging.
constexpr gpio_num_t kChargeFull = GPIO_NUM_1;              // STDBY_H: high means full.
constexpr int kBatteryShutdownPercent = 0;
constexpr int kBatteryShutdownMv = 3450;
constexpr int kBatteryShutdownDebounceCount = 3;
constexpr const char* kPmuStatusUnknown = "unknown";
constexpr size_t kSettingsItemCount = 6;
constexpr uint32_t kAutoSyncOptions[] = {0, 15, 30, 60, 240};

#if CONFIG_WQN_EPD_UI_ENABLE

enum class RefreshSchedule {
    kNone,
    kConfig,
    kAi,
    kClock,
    kSelection,
    kTimer,
    kCommit,
    kImmediate,
};

SemaphoreHandle_t g_refresh_mutex = nullptr;
TaskHandle_t g_refresh_task = nullptr;
QueueHandle_t g_todo_request_queue = nullptr;
QueueHandle_t g_todo_result_queue = nullptr;
TaskHandle_t g_todo_task = nullptr;
QueueHandle_t g_word_request_queue = nullptr;
QueueHandle_t g_word_result_queue = nullptr;
TaskHandle_t g_word_task = nullptr;
wqn::UiFrame g_pending_frame;
std::string g_pending_signature;
bool g_refresh_pending = false;
bool g_refresh_busy = false;
TickType_t g_refresh_due_tick = 0;
RefreshSchedule g_refresh_schedule = RefreshSchedule::kNone;
volatile bool g_todo_cloud_busy = false;
volatile bool g_word_cloud_busy = false;

void BuildHomeSummary(wqn::UiState* state);

enum class TodoCloudOp {
    kRefresh,
    kComplete,
};

struct TodoCloudRequest {
    TodoCloudOp op = TodoCloudOp::kRefresh;
    char todo_id[64] = {};
    char cursor[160] = {};
};

struct TodoCloudResult {
    TodoCloudOp op = TodoCloudOp::kRefresh;
    esp_err_t result = ESP_FAIL;
    bool auth_required = false;
    char todo_id[64] = {};
    wqn::WqnTodoListPage page;
    wqn::WqnTodoItem todo;
};

enum class WordCloudOp {
    kPackSync,
    kSubmit,
    kSearch,
    kAiLookup,
};

struct WordCloudRequest {
    WordCloudOp op = WordCloudOp::kPackSync;
    char word_id[64] = {};
    char outcome[16] = {};
    char word[80] = {};
    char query[96] = {};
};

struct WordCloudResult {
    WordCloudOp op = WordCloudOp::kPackSync;
    esp_err_t result = ESP_FAIL;
    bool auth_required = false;
    char word_id[64] = {};
    char outcome[16] = {};
    char word[80] = {};
    wqn::WordPackIndex pack_index;
    wqn::WqnWordReviewSubmitResult submit;
    wqn::WqnWordSearchResult search;
    wqn::WqnWordAiLookupResult lookup;
    std::string message;
};

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

bool TickBefore(TickType_t a, TickType_t b)
{
    return static_cast<int32_t>(a - b) < 0;
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

struct UiRect {
    int x;
    int y;
    int width;
    int height;
    const char* name;
};

struct BatteryReading {
    int raw = 0;
    int adc_mv = 0;
    int battery_mv = 0;
    int percent = 0;
    int chrg_l = 1;
    int stdby_h = 0;
    bool charging = false;
    bool full = false;
    bool power_present_or_status_known = false;
    const char* pmu_status = kPmuStatusUnknown;
    bool pmu_implemented = false;
};

bool ShouldLogBattery()
{
    static int64_t last_log_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (last_log_us == 0 || now_us - last_log_us >= 60000000LL) {
        last_log_us = now_us;
        return true;
    }
    return false;
}

bool ReadBatteryStatus(BatteryReading* reading)
{
    if (reading == nullptr) {
        return false;
    }

    static bool initialized = false;
    static adc_oneshot_unit_handle_t adc_handle = nullptr;
    static adc_cali_handle_t cali_handle = nullptr;

    if (!initialized) {
        adc_oneshot_unit_init_cfg_t init_config = {};
        init_config.unit_id = ADC_UNIT_1;
        init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
        if (adc_oneshot_new_unit(&init_config, &adc_handle) != ESP_OK) {
            return false;
        }

        adc_oneshot_chan_cfg_t channel_config = {};
        channel_config.atten = ADC_ATTEN_DB_12;
        channel_config.bitwidth = ADC_BITWIDTH_12;
        if (adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &channel_config) != ESP_OK) {
            return false;
        }

        adc_cali_curve_fitting_config_t cali_config = {};
        cali_config.unit_id = ADC_UNIT_1;
        cali_config.chan = ADC_CHANNEL_3;
        cali_config.atten = ADC_ATTEN_DB_12;
        cali_config.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) != ESP_OK) {
            return false;
        }
        initialized = true;
    }

    int raw_sum = 0;
    int adc_voltage_sum = 0;
    int battery_voltage_sum = 0;
    for (int i = 0; i < 10; ++i) {
        int raw = 0;
        int raw_voltage = 0;
        if (adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(cali_handle, raw, &raw_voltage) != ESP_OK) {
            return false;
        }
        raw_sum += raw;
        adc_voltage_sum += raw_voltage;
        battery_voltage_sum += raw_voltage * 2;
    }

    reading->raw = raw_sum / 10;
    reading->adc_mv = adc_voltage_sum / 10;
    reading->battery_mv = battery_voltage_sum / 10;
    if (reading->battery_mv <= 0) {
        return false;
    }

    int computed_percent =
        (-1 * reading->battery_mv * reading->battery_mv + 9016 * reading->battery_mv - 19189000) / 10000;
    computed_percent = std::min(100, std::max(0, computed_percent));
    reading->percent = computed_percent;
    reading->chrg_l = gpio_get_level(kChargeDetect);
    reading->stdby_h = gpio_get_level(kChargeFull);
    reading->charging = reading->chrg_l == 0;
    reading->full = reading->stdby_h == 1;
    reading->power_present_or_status_known = reading->charging || reading->full;
    reading->pmu_status = kPmuStatusUnknown;
    reading->pmu_implemented = false;
    if (ShouldLogBattery()) {
        ESP_LOGI(
            kTag,
            "battery adc_raw=%d adc_mv=%d battery_mv=%d percent=%d chrg_l=%d stdby_h=%d charging=%d full=%d pmu_status=%s pmu_implemented=%d",
            reading->raw,
            reading->adc_mv,
            reading->battery_mv,
            reading->percent,
            reading->chrg_l,
            reading->stdby_h,
            reading->charging ? 1 : 0,
            reading->full ? 1 : 0,
            reading->pmu_status,
            reading->pmu_implemented ? 1 : 0);
    }
    return true;
}

std::string BatteryLabel(const BatteryReading& reading)
{
    if (reading.full) {
        return "满电";
    }
    if (reading.charging) {
        return "充电 " + std::to_string(reading.percent) + "%";
    }
    if (reading.percent <= kBatteryShutdownPercent) {
        return "低电";
    }
    return std::to_string(reading.percent) + "%";
}

void CheckLowBatteryProtection(const BatteryReading* reading)
{
    static int shutdown_count = 0;

    if (reading == nullptr || reading->battery_mv <= 0) {
        if (shutdown_count != 0) {
            ESP_LOGI(kTag, "Battery shutdown count reset: invalid reading depleted_candidate=0 shutdown_count=0");
        }
        shutdown_count = 0;
        ESP_LOGI(kTag, "battery protection skipped: invalid reading pmu_status=%s depleted_candidate=0 shutdown_count=%d",
                 kPmuStatusUnknown,
                 shutdown_count);
        return;
    }

    const bool depleted_candidate =
        reading->percent == kBatteryShutdownPercent &&
        reading->battery_mv <= kBatteryShutdownMv &&
        !reading->charging &&
        !reading->full;
    if (!depleted_candidate) {
        if (shutdown_count != 0) {
            ESP_LOGI(kTag, "Battery shutdown count reset: percent=%d battery_mv=%d charging=%d full=%d depleted_candidate=0 shutdown_count=0",
                     reading->percent,
                     reading->battery_mv,
                     reading->charging ? 1 : 0,
                     reading->full ? 1 : 0);
        }
        shutdown_count = 0;
        ESP_LOGI(kTag, "battery protection percent=%d battery_mv=%d charging=%d full=%d pmu_status=%s depleted_candidate=0 shutdown_count=%d",
                 reading->percent,
                 reading->battery_mv,
                 reading->charging ? 1 : 0,
                 reading->full ? 1 : 0,
                 reading->pmu_status,
                 shutdown_count);
        return;
    }

    ++shutdown_count;
    ESP_LOGW(kTag, "battery protection percent=%d battery_mv=%d charging=%d full=%d pmu_status=%s depleted_candidate=1 shutdown_count=%d/%d",
             reading->percent,
             reading->battery_mv,
             reading->charging ? 1 : 0,
             reading->full ? 1 : 0,
             reading->pmu_status,
             shutdown_count,
             kBatteryShutdownDebounceCount);
    if (shutdown_count >= kBatteryShutdownDebounceCount) {
        wqn::ShutdownForBatteryDepleted();
    }
}

size_t AutoSyncOptionIndex(uint32_t minutes)
{
    for (size_t i = 0; i < sizeof(kAutoSyncOptions) / sizeof(kAutoSyncOptions[0]); ++i) {
        if (kAutoSyncOptions[i] == minutes) {
            return i;
        }
    }
    return 0;
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

std::string LimitForEpd(const std::string& text)
{
    return wqn::TruncateUtf8TextToWidth(text, kEpdTextWidth);
}

void DrawHorizontalLine(int x, int y, int width)
{
    for (int xx = 0; xx < width; ++xx) {
        wqn::DrawEpdPixel(x + xx, y, true);
    }
}

void DrawVerticalLine(int x, int y, int height)
{
    for (int yy = 0; yy < height; ++yy) {
        wqn::DrawEpdPixel(x, y + yy, true);
    }
}

void DrawRect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    DrawHorizontalLine(x, y, width);
    DrawHorizontalLine(x, y + height - 1, width);
    DrawVerticalLine(x, y, height);
    DrawVerticalLine(x + width - 1, y, height);
}

void FillRect(int x, int y, int width, int height, bool black)
{
    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            wqn::DrawEpdPixel(x + xx, y + yy, black);
        }
    }
}

void ClearRect(const UiRect& rect)
{
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(0, rect.y);
    const int x1 = std::min(wqn::kEpdWidth, rect.x + rect.width);
    const int y1 = std::min(wqn::kEpdHeight, rect.y + rect.height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    FillRect(x0, y0, x1 - x0, y1 - y0, false);
}

esp_err_t RefreshRegion(const UiRect& rect, RefreshSchedule schedule)
{
    ESP_LOGI(
        kTag,
        "EPD UI region refresh: schedule=%s region=%s rect=x%d y%d w%d h%d",
        RefreshScheduleName(schedule),
        rect.name,
        rect.x,
        rect.y,
        rect.width,
        rect.height);
    return wqn::RefreshEpdFull(true, false);
}

esp_err_t RefreshFrame(RefreshSchedule schedule)
{
    const bool allow_local_partial =
        schedule == RefreshSchedule::kClock || schedule == RefreshSchedule::kTimer ||
        schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig ||
        schedule == RefreshSchedule::kAi;
    const bool force_full_refresh = schedule == RefreshSchedule::kCommit || schedule == RefreshSchedule::kImmediate;
    return wqn::RefreshEpdFull(allow_local_partial, force_full_refresh);
}

esp_err_t DrawClippedText(int x, int y, int max_width, const std::string& text, bool black = true)
{
    const std::string clipped = wqn::TruncateUtf8TextToWidth(text, max_width);
    return wqn::DrawUtf8Text(x, y, clipped.c_str(), black);
}

esp_err_t DrawCenteredText(int x, int y, int width, const std::string& text, bool black = true)
{
    const std::string clipped = wqn::TruncateUtf8TextToWidth(text, width - 4);
    const int text_width = wqn::MeasureUtf8TextWidth(clipped.c_str());
    const int text_x = x + std::max(2, (width - text_width) / 2);
    return wqn::DrawUtf8Text(text_x, y, clipped.c_str(), black);
}

esp_err_t DrawWrappedText(int x, int y, int width, const std::string& text, int max_lines, bool black = true)
{
    const std::vector<std::string> lines = wqn::WrapUtf8TextToWidth(text, width, max_lines);
    int line_y = y;
    for (const std::string& line : lines) {
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, line_y, line.c_str(), black), kTag, "draw wrapped text");
        line_y += 18;
    }
    return ESP_OK;
}

void DrawSegment(int x, int y, int width, int height)
{
    FillRect(x, y, width, height, true);
}

int SevenSegmentDigitWidth(int scale)
{
    return 6 * scale;
}

int SevenSegmentDigitHeight(int scale)
{
    return 10 * scale;
}

int SevenSegmentTextWidth(const std::string& text, int scale)
{
    int width = 0;
    for (char c : text) {
        width += c == ':' ? 2 * scale : SevenSegmentDigitWidth(scale);
        width += scale;
    }
    return std::max(0, width - scale);
}

void DrawSevenSegmentDigit(int x, int y, int scale, char digit)
{
    static constexpr uint8_t kSegments[10] = {
        0b1111110,  // 0
        0b0110000,  // 1
        0b1101101,  // 2
        0b1111001,  // 3
        0b0110011,  // 4
        0b1011011,  // 5
        0b1011111,  // 6
        0b1110000,  // 7
        0b1111111,  // 8
        0b1111011,  // 9
    };
    if (digit < '0' || digit > '9') {
        return;
    }
    const uint8_t segments = kSegments[digit - '0'];
    const int width = SevenSegmentDigitWidth(scale);
    const int height = SevenSegmentDigitHeight(scale);
    const int t = std::max(2, scale);
    if (segments & 0b1000000) {
        DrawSegment(x + t, y, width - 2 * t, t);
    }
    if (segments & 0b0100000) {
        DrawSegment(x + width - t, y + t, t, height / 2 - t);
    }
    if (segments & 0b0010000) {
        DrawSegment(x + width - t, y + height / 2, t, height / 2 - t);
    }
    if (segments & 0b0001000) {
        DrawSegment(x + t, y + height - t, width - 2 * t, t);
    }
    if (segments & 0b0000100) {
        DrawSegment(x, y + height / 2, t, height / 2 - t);
    }
    if (segments & 0b0000010) {
        DrawSegment(x, y + t, t, height / 2 - t);
    }
    if (segments & 0b0000001) {
        DrawSegment(x + t, y + height / 2 - t / 2, width - 2 * t, t);
    }
}

void DrawSevenSegmentTextCentered(int y, const std::string& text, int scale)
{
    int x = std::max(0, (wqn::kEpdWidth - SevenSegmentTextWidth(text, scale)) / 2);
    for (char c : text) {
        if (c == ':') {
            const int dot = std::max(2, scale);
            const int colon_x = x + scale / 2;
            FillRect(colon_x, y + 3 * scale, dot, dot, true);
            FillRect(colon_x, y + 7 * scale, dot, dot, true);
            x += 3 * scale;
        } else {
            DrawSevenSegmentDigit(x, y, scale, c);
            x += SevenSegmentDigitWidth(scale) + scale;
        }
    }
}

std::string CurrentClockLabel();

std::time_t CurrentUnixTime()
{
    std::time_t now = 0;
    std::time(&now);
    return now;
}

bool SystemClockIsReasonable()
{
    return CurrentUnixTime() >= kMinReasonableUnixTime;
}

int MonthIndexFromBuildDate(const char* month)
{
    static constexpr const char* kMonths[] = {
        "Jan",
        "Feb",
        "Mar",
        "Apr",
        "May",
        "Jun",
        "Jul",
        "Aug",
        "Sep",
        "Oct",
        "Nov",
        "Dec",
    };
    for (int i = 0; i < 12; ++i) {
        if (strncmp(month, kMonths[i], 3) == 0) {
            return i;
        }
    }
    return 0;
}

void SeedClockFromBuildTimeIfNeeded()
{
    setenv("TZ", "CST-8", 1);
    tzset();
    if (SystemClockIsReasonable()) {
        return;
    }

    char month[4] = {};
    int day = 1;
    int year = 2026;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
        std::sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        ESP_LOGW(kTag, "build time parse failed; clock remains invalid");
        return;
    }

    std::tm build_tm = {};
    build_tm.tm_year = year - 1900;
    build_tm.tm_mon = MonthIndexFromBuildDate(month);
    build_tm.tm_mday = day;
    build_tm.tm_hour = hour;
    build_tm.tm_min = minute;
    build_tm.tm_sec = second;
    build_tm.tm_isdst = -1;
    const std::time_t build_time = std::mktime(&build_tm);
    if (build_time < kMinReasonableUnixTime) {
        ESP_LOGW(kTag, "build time is not reasonable; clock remains invalid");
        return;
    }
    const timeval tv = {
        .tv_sec = build_time,
        .tv_usec = 0,
    };
    settimeofday(&tv, nullptr);
    ESP_LOGI(kTag, "clock seeded from build time: %04d-%02d-%02d %02d:%02d:%02d CST", year, build_tm.tm_mon + 1, day, hour, minute, second);
}

std::string CurrentDateLabel()
{
    const std::time_t now = CurrentUnixTime();
    if (now < kMinReasonableUnixTime) {
        return "--月--日";
    }

    std::tm time_info = {};
    localtime_r(&now, &time_info);
    static constexpr const char* kWeekdays[] = {
        "星期日",
        "星期一",
        "星期二",
        "星期三",
        "星期四",
        "星期五",
        "星期六",
    };
    char buffer[32] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d月%d日 %s",
        time_info.tm_mon + 1,
        time_info.tm_mday,
        kWeekdays[time_info.tm_wday]);
    return buffer;
}

const char* TimeTileTitle(wqn::TimeTile tile)
{
    switch (tile) {
        case wqn::TimeTile::kCountdown:
            return "倒计时";
        case wqn::TimeTile::kPomodoro:
            return "番茄钟";
        case wqn::TimeTile::kClock:
        default:
            return "时间";
    }
}

void DrawStatusBar(const char* title, const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    wqn::DrawUtf8Text(10, 6, title, true);
    std::string status = CurrentClockLabel();
    if (!home.battery_label.empty()) {
        status += "  " + home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true);
}

void DrawProgressBar(int x, int y, int width, int height, int current, int total)
{
    DrawRect(x, y, width, height);
    if (total <= 0) {
        return;
    }
    const int filled = std::min(width - 2, std::max(0, (width - 2) * current / total));
    if (filled > 0) {
        FillRect(x + 1, y + 1, filled, height - 2, true);
    }
}

void DrawConfigBox(int x, int y, int width, int height, const std::string& value, const std::string& label, bool selected)
{
    if (selected) {
        FillRect(x, y, width, height, true);
    } else {
        DrawRect(x, y, width, height);
    }
    const bool black_text = !selected;
    DrawCenteredText(x, y + 7, width, value, black_text);
    DrawCenteredText(x, y + height - 20, width, label, black_text);
}

void DrawActionBox(int x, int y, int width, const std::string& label, bool selected)
{
    if (selected) {
        FillRect(x, y, width, 28, true);
    } else {
        DrawRect(x, y, width, 28);
    }
    DrawCenteredText(x, y + 6, width, label, !selected);
}

std::string TwoDigit(int value)
{
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d", std::max(0, value));
    return buffer;
}

int CountdownStartField()
{
    return 3;
}

int PomodoroStartField()
{
    return 4;
}

constexpr UiRect kStatusBarRect = {0, 0, wqn::kEpdWidth, 30, "status-bar"};
constexpr UiRect kHomePrimaryRect = {8, 33, 384, 31, "home-primary-time"};
constexpr UiRect kTimeClockRect = {24, 58, 352, 136, "time-clock"};
constexpr UiRect kTimeDateRect = {0, 164, wqn::kEpdWidth, 34, "time-date"};
constexpr UiRect kTimerRunRect = {0, 58, wqn::kEpdWidth, 186, "timer-run"};
constexpr UiRect kCountdownConfigRect = {0, 70, wqn::kEpdWidth, 205, "countdown-config"};
constexpr UiRect kPomodoroConfigRect = {0, 54, wqn::kEpdWidth, 218, "pomodoro-config"};

UiRect ConfigRefreshRect(const wqn::TimeAppState& time_app)
{
    return time_app.tile == wqn::TimeTile::kPomodoro ? kPomodoroConfigRect : kCountdownConfigRect;
}

esp_err_t RenderHomePrimaryRegion(const wqn::HomeSummary& home, RefreshSchedule schedule)
{
    ClearRect(kHomePrimaryRect);
    DrawRect(10, 35, 380, 26);
    ESP_RETURN_ON_ERROR(DrawCenteredText(10, 39, 380, home.primary_time_line), kTag, "draw home primary time region");
    return RefreshRegion(kHomePrimaryRect, schedule);
}

esp_err_t RenderTimeClockRegion(RefreshSchedule schedule, bool include_date)
{
    ClearRect(kTimeClockRect);
    DrawSevenSegmentTextCentered(70, CurrentClockLabel(), 9);
    if (include_date) {
        ClearRect(kTimeDateRect);
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 174, wqn::kEpdWidth, CurrentDateLabel()), kTag, "draw date region");
    }
    UiRect rect = kTimeClockRect;
    if (include_date) {
        rect.y = kTimeClockRect.y;
        rect.height = (kTimeDateRect.y + kTimeDateRect.height) - kTimeClockRect.y;
        rect.name = "time-clock-date";
    }
    return RefreshRegion(rect, schedule);
}

esp_err_t RenderCountdownConfigToEpd(const wqn::TimeAppState& time_app)
{
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 83, wqn::kEpdWidth, "倒计时设置"), kTag, "draw countdown config title");
    const int y = 116;
    DrawConfigBox(102, y, 56, 62, TwoDigit(time_app.countdown_hours), "时", time_app.active_field == 0);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(166, y + 22, ":", true), kTag, "draw countdown colon 1");
    DrawConfigBox(178, y, 56, 62, TwoDigit(time_app.countdown_minutes), "分", time_app.active_field == 1);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(242, y + 22, ":", true), kTag, "draw countdown colon 2");
    DrawConfigBox(254, y, 56, 62, TwoDigit(time_app.countdown_seconds), "秒", time_app.active_field == 2);
    DrawActionBox(130, 205, 68, "开始", time_app.active_field == CountdownStartField());
    DrawActionBox(212, 205, 68, "退出", time_app.active_field == CountdownStartField() + 1);
    if (time_app.is_editing) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 248, wqn::kEpdWidth, "正在调整"), kTag, "draw editing hint");
    }
    return ESP_OK;
}

esp_err_t RenderPomodoroConfigToEpd(const wqn::TimeAppState& time_app)
{
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 66, wqn::kEpdWidth, "番茄钟设置"), kTag, "draw pomodoro config title");
    DrawConfigBox(108, 95, 88, 52, std::to_string(time_app.pomodoro_rounds), "轮数", time_app.active_field == 0);
    DrawConfigBox(
        204,
        95,
        88,
        52,
        std::to_string(time_app.pomodoro_focus_minutes),
        "专注",
        time_app.active_field == 1);
    DrawConfigBox(
        108,
        155,
        88,
        52,
        std::to_string(time_app.pomodoro_break_minutes),
        "休息",
        time_app.active_field == 2);
    DrawConfigBox(
        204,
        155,
        88,
        52,
        std::to_string(time_app.pomodoro_long_break_minutes),
        "长休息",
        time_app.active_field == 3);
    DrawActionBox(130, 226, 68, "开始", time_app.active_field == PomodoroStartField());
    DrawActionBox(212, 226, 68, "退出", time_app.active_field == PomodoroStartField() + 1);
    return ESP_OK;
}

int TimerInitialSeconds(const wqn::TimeAppState& time_app)
{
    if (time_app.active_mode == wqn::TimerMode::kCountdown) {
        return std::max(1, time_app.countdown_total_seconds);
    }
    if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        switch (time_app.pomodoro_phase) {
            case wqn::PomodoroPhase::kBreak:
                return std::max(1, time_app.pomodoro_break_minutes * 60);
            case wqn::PomodoroPhase::kLongBreak:
                return std::max(1, time_app.pomodoro_long_break_minutes * 60);
            case wqn::PomodoroPhase::kFocus:
            default:
                return std::max(1, time_app.pomodoro_focus_minutes * 60);
        }
    }
    return 1;
}

esp_err_t RenderTimerRunToEpd(const wqn::TimeAppState& time_app)
{
    const std::string label =
        time_app.active_mode == wqn::TimerMode::kPomodoro ? wqn::PomodoroPhaseLabel(time_app.pomodoro_phase) : "倒计时";
    ESP_RETURN_ON_ERROR(DrawCenteredText(0, 68, wqn::kEpdWidth, label), kTag, "draw timer label");
    DrawSevenSegmentTextCentered(98, wqn::FormatTimerDuration(time_app.remaining_seconds), 7);
    if (time_app.status == wqn::TimerStatus::kPaused) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 185, wqn::kEpdWidth, "已暂停"), kTag, "draw paused label");
    } else if (time_app.status == wqn::TimerStatus::kAlerting) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 185, wqn::kEpdWidth, "时间到了"), kTag, "draw alert label");
    } else if (time_app.active_mode == wqn::TimerMode::kPomodoro) {
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(
                0,
                185,
                wqn::kEpdWidth,
                "第 " + std::to_string(time_app.pomodoro_current_round) + "/" +
                    std::to_string(time_app.pomodoro_rounds) + " 轮"),
            kTag,
            "draw pomodoro round");
    }
    const int total = TimerInitialSeconds(time_app);
    DrawProgressBar(115, 220, 170, 12, total - time_app.remaining_seconds, total);
    return ESP_OK;
}

esp_err_t RenderTimerRunRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule)
{
    ClearRect(kTimerRunRect);
    ESP_RETURN_ON_ERROR(RenderTimerRunToEpd(time_app), kTag, "draw timer run region");
    return RefreshRegion(kTimerRunRect, schedule);
}

esp_err_t RenderTimeConfigRegion(const wqn::TimeAppState& time_app, RefreshSchedule schedule)
{
    const UiRect rect = ConfigRefreshRect(time_app);
    ClearRect(rect);
    if (time_app.tile == wqn::TimeTile::kCountdown) {
        ESP_RETURN_ON_ERROR(RenderCountdownConfigToEpd(time_app), kTag, "draw countdown config region");
    } else {
        ESP_RETURN_ON_ERROR(RenderPomodoroConfigToEpd(time_app), kTag, "draw pomodoro config region");
    }
    return RefreshRegion(rect, schedule);
}

esp_err_t RenderTimeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::TimeAppState& time_app = frame.time_app;
    wqn::ClearEpdFramebuffer(true);
    DrawStatusBar(TimeTileTitle(time_app.tile), frame.home);

    if (time_app.tile == wqn::TimeTile::kClock) {
        DrawSevenSegmentTextCentered(70, CurrentClockLabel(), 9);
        ESP_RETURN_ON_ERROR(DrawCenteredText(0, 174, wqn::kEpdWidth, CurrentDateLabel()), kTag, "draw date");
        DrawHorizontalLine(32, 216, 336);
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(116, 240, "晴", true), kTag, "draw weather icon word");
        ESP_RETURN_ON_ERROR(DrawCenteredText(156, 232, 88, "杭州"), kTag, "draw city");
        ESP_RETURN_ON_ERROR(DrawCenteredText(156, 252, 88, "18~29°"), kTag, "draw temp range");
        ESP_RETURN_ON_ERROR(DrawCenteredText(258, 240, 58, "26°"), kTag, "draw temp");
    } else if (time_app.config_mode && time_app.tile == wqn::TimeTile::kCountdown) {
        ESP_RETURN_ON_ERROR(RenderCountdownConfigToEpd(time_app), kTag, "draw countdown config");
    } else if (time_app.config_mode && time_app.tile == wqn::TimeTile::kPomodoro) {
        ESP_RETURN_ON_ERROR(RenderPomodoroConfigToEpd(time_app), kTag, "draw pomodoro config");
    } else {
        ESP_RETURN_ON_ERROR(RenderTimerRunToEpd(time_app), kTag, "draw running timer");
    }

    return RefreshFrame(schedule);
}

esp_err_t DrawAiBubble(int x, int y, int width, int height, const std::string& text, bool me, bool pending)
{
    if (me) {
        FillRect(x, y, width, height, true);
    } else if (pending) {
        DrawRect(x, y, width, height);
        DrawRect(x + 2, y + 2, width - 4, height - 4);
    } else {
        DrawRect(x, y, width, height);
    }
    const int max_lines = std::max(1, (height - 14) / 18);
    return DrawWrappedText(x + 8, y + 7, width - 16, text, max_lines, !me);
}

std::string Utf8PageSlice(const std::string& text, size_t page, size_t chars_per_page)
{
    if (chars_per_page == 0) {
        return text;
    }
    const size_t start_char = page * chars_per_page;
    const size_t end_char = start_char + chars_per_page;
    size_t char_index = 0;
    size_t start_byte = text.size();
    size_t end_byte = text.size();
    for (size_t i = 0; i < text.size();) {
        if (char_index == start_char) {
            start_byte = i;
        }
        if (char_index == end_char) {
            end_byte = i;
            break;
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        if ((c & 0x80) == 0) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        if (i + step > text.size()) {
            step = 1;
        }
        i += step;
        ++char_index;
    }
    if (start_char >= char_index && start_byte == text.size()) {
        return "";
    }
    if (start_byte == text.size() && start_char == 0) {
        start_byte = 0;
    }
    if (end_byte < start_byte) {
        end_byte = text.size();
    }
    return text.substr(start_byte, end_byte - start_byte);
}

std::string JoinAiFunctionCallSummaries(const std::vector<std::string>& summaries)
{
    std::string output;
    for (const std::string& summary : summaries) {
        if (summary.empty()) {
            continue;
        }
        if (!output.empty()) {
            output += "\n";
        }
        output += summary;
    }
    return output;
}

std::string AiAssistantFallbackText(const wqn::AiSessionState& ai)
{
    if (ai.status == wqn::AiSessionStatus::kListening) {
        return "正在录音，松手后上传识别。";
    }
    if (ai.status == wqn::AiSessionStatus::kWaitingReply) {
        return ai.pending_text.empty() ? "正在上传并等待模型回复..." : ai.pending_text;
    }
    if (ai.status == wqn::AiSessionStatus::kError) {
        return "请求失败，请长按确认重试。";
    }
    return "我会在这里显示转写和回答。";
}

constexpr size_t kAiAssistantCharsPerPage = 92;

const char* AiStatusLabel(wqn::AiSessionStatus status)
{
    switch (status) {
        case wqn::AiSessionStatus::kListening:
            return "录音";
        case wqn::AiSessionStatus::kWaitingReply:
            return "识别";
        case wqn::AiSessionStatus::kReplyReady:
            return "完成";
        case wqn::AiSessionStatus::kError:
            return "错误";
        case wqn::AiSessionStatus::kIdle:
        default:
            return "空闲";
    }
}

esp_err_t DrawAiInputBar(const wqn::AiSessionState& ai, size_t page, size_t page_count)
{
    const bool listening = ai.status == wqn::AiSessionStatus::kListening;
    const bool waiting = ai.status == wqn::AiSessionStatus::kWaitingReply;
    const int x = 12;
    const int y = 252;
    const int width = 376;
    const int height = 36;
    if (listening) {
        FillRect(x, y, width, height, true);
    } else {
        DrawRect(x, y, width, height);
    }

    std::string label = "长按确认开始说话";
    std::string state = AiStatusLabel(ai.status);
    if (listening) {
        label = "正在输入";
        state = "录音";
    } else if (waiting) {
        label = "服务器处理中";
        state = "等待";
    } else if (ai.status == wqn::AiSessionStatus::kReplyReady) {
        label = "长按继续追问";
        state = "就绪";
    } else if (ai.status == wqn::AiSessionStatus::kError) {
        label = "长按重试";
        state = "错误";
    }
    if (page_count > 1 && (ai.status == wqn::AiSessionStatus::kReplyReady || ai.status == wqn::AiSessionStatus::kError)) {
        state = std::to_string(page + 1) + "/" + std::to_string(page_count);
    }

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x + 8, y + 9, label.c_str(), !listening), kTag, "draw AI input label");
    const int state_width = wqn::MeasureUtf8TextWidth(state.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(x + width - state_width - 8, y + 9, state.c_str(), !listening),
        kTag,
        "draw AI input state");

    if (listening) {
        const int wave_x = x + width - 74;
        const int base_y = y + 24;
        const int heights[] = {8, 17, 11, 15};
        for (int i = 0; i < 4; ++i) {
            FillRect(wave_x + i * 10, base_y - heights[i], 5, heights[i], false);
        }
    }
    return ESP_OK;
}

esp_err_t RenderAiToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::AiSessionState& ai = frame.ai;
    wqn::ClearEpdFramebuffer(true);
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 6, "AI", true), kTag, "draw AI title");
    std::string status = AiStatusLabel(ai.status);
    if (!frame.home.battery_label.empty()) {
        status += "  ";
        status += frame.home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true),
        kTag,
        "draw AI status");

    const bool has_user_text = !ai.user_text.empty();
    const std::string user_text = has_user_text ? ai.user_text : "长按确认键开始语音提问";
    const size_t text_page_count = wqn::AiSessionTextPageCount(ai);
    const size_t page_count = wqn::AiSessionPageCount(ai);
    const size_t page = std::min(ai.page, page_count > 0 ? page_count - 1 : 0);
    const bool action_page = !ai.function_call_summaries.empty() && page >= text_page_count;
    std::string assistant_text = action_page ? JoinAiFunctionCallSummaries(ai.function_call_summaries) : ai.assistant_text;
    if (assistant_text.empty()) {
        assistant_text = AiAssistantFallbackText(ai);
    }
    if (!action_page) {
        assistant_text = Utf8PageSlice(assistant_text, std::min(page, text_page_count - 1), kAiAssistantCharsPerPage);
    }
    if (action_page && assistant_text.empty()) {
        assistant_text = "没有云端动作。";
    }

    ESP_RETURN_ON_ERROR(
        DrawAiBubble(94, 44, 282, 58, user_text, true, ai.status == wqn::AiSessionStatus::kListening),
        kTag,
        "draw AI user bubble");
    ESP_RETURN_ON_ERROR(
        DrawAiBubble(24, 116, 352, 104, assistant_text, false, ai.status == wqn::AiSessionStatus::kWaitingReply),
        kTag,
        "draw AI assistant bubble");
    std::string detail_text;
    if ((ai.status == wqn::AiSessionStatus::kListening || ai.status == wqn::AiSessionStatus::kWaitingReply) &&
        !ai.pending_text.empty()) {
        detail_text = ai.pending_text;
    } else if (!ai.status_detail.empty()) {
        detail_text = ai.status_detail;
    } else if (!ai.function_call_summaries.empty()) {
        detail_text = ai.function_call_summaries.front();
    }
    if (!detail_text.empty()) {
        ESP_RETURN_ON_ERROR(DrawClippedText(28, 228, 344, detail_text), kTag, "draw AI detail text");
    }
    ESP_RETURN_ON_ERROR(DrawAiInputBar(ai, page, page_count), kTag, "draw AI input bar");
    return RefreshFrame(schedule == RefreshSchedule::kCommit ? RefreshSchedule::kCommit : RefreshSchedule::kAi);
}

esp_err_t RenderWordToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::WordAppSnapshot& word = frame.word_app;
    wqn::ClearEpdFramebuffer(true);
    DrawStatusBar("单词", frame.home);

    ESP_RETURN_ON_ERROR(DrawClippedText(10, 36, 250, word.status_line), kTag, "draw word status");
    ESP_RETURN_ON_ERROR(DrawClippedText(282, 36, 108, word.progress_line), kTag, "draw word progress");
    DrawHorizontalLine(10, 58, 380);

    auto draw_choice = [](int y, const std::string& title, const std::string& subtitle, bool selected) -> esp_err_t {
        const int x = 38;
        const int width = 324;
        const int height = 42;
        if (selected) {
            DrawRect(x - 3, y - 3, width + 6, height + 6);
            DrawRect(x - 1, y - 1, width + 2, height + 2);
        } else {
            DrawRect(x, y, width, height);
        }
        ESP_RETURN_ON_ERROR(DrawClippedText(x + 12, y + 7, 180, title), kTag, "draw word choice title");
        ESP_RETURN_ON_ERROR(DrawClippedText(x + 160, y + 7, 150, subtitle), kTag, "draw word choice subtitle");
        return ESP_OK;
    };

    auto draw_word_back = [&word]() -> esp_err_t {
        DrawRect(22, 128, 356, 116);
        const std::string title = word.part_of_speech.empty() ? word.meaning : word.part_of_speech + "  " + word.meaning;
        ESP_RETURN_ON_ERROR(DrawWrappedText(34, 140, 332, title, 2), kTag, "draw word meaning");
        if (!word.example.empty()) {
            ESP_RETURN_ON_ERROR(DrawWrappedText(34, 184, 332, word.example, 2), kTag, "draw word example");
        }
        if (!word.example_translation.empty()) {
            ESP_RETURN_ON_ERROR(DrawWrappedText(34, 224, 332, word.example_translation, 1), kTag, "draw word translation");
        }
        return ESP_OK;
    };

    if (word.mode == wqn::WordAppMode::kHome) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 72, 360, "单词复习"), kTag, "draw word home title");
        ESP_RETURN_ON_ERROR(
            draw_choice(110, "顺序复习", word.pack_ready ? "从词库开始" : "需同步词库", word.home_selection == wqn::WordHomeSelection::kSequential),
            kTag,
            "draw sequential choice");
        ESP_RETURN_ON_ERROR(
            draw_choice(162, "随机复习", word.pack_ready ? "打乱今日词" : "需同步词库", word.home_selection == wqn::WordHomeSelection::kRandom),
            kTag,
            "draw random choice");
        ESP_RETURN_ON_ERROR(
            draw_choice(214, "词典", word.pack_ready ? "按字母查词" : "在线同步后使用", word.home_selection == wqn::WordHomeSelection::kDictionary),
            kTag,
            "draw dictionary choice");
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        return RefreshFrame(schedule);
    }

    if (word.mode == wqn::WordAppMode::kDictionary) {
        const std::string prefix = word.dictionary_prefix.empty() ? "选择首字母" : word.dictionary_prefix;
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 70, 360, prefix), kTag, "draw dictionary prefix");
        const int start_x = 28;
        const int start_y = 108;
        const int cell_w = 42;
        const int cell_h = 30;
        for (size_t i = 0; i < word.dictionary_letters.size() && i < 24; ++i) {
            const int col = static_cast<int>(i % 8);
            const int row = static_cast<int>(i / 8);
            const int x = start_x + col * cell_w;
            const int y = start_y + row * cell_h;
            if (i == word.dictionary_letter_selected) {
                DrawRect(x - 2, y - 2, cell_w - 4, cell_h - 2);
            }
            char letter[2] = {word.dictionary_letters[i], 0};
            ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 7, cell_w - 8, letter), kTag, "draw dictionary letter");
        }
        int y = 212;
        for (size_t i = 0; i < word.dictionary_preview_words.size() && i < 3; ++i) {
            const std::string marker = i == word.dictionary_match_selected ? "> " : "  ";
            ESP_RETURN_ON_ERROR(DrawClippedText(42, y, 300, marker + word.dictionary_preview_words[i]), kTag, "draw dictionary preview");
            y += 22;
        }
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        return RefreshFrame(schedule);
    }

    if (word.mode == wqn::WordAppMode::kLookupChoice) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 76, 360, word.dictionary_prefix), kTag, "draw lookup query");
        ESP_RETURN_ON_ERROR(
            draw_choice(128, "在线搜索", "查 WQN 服务器", word.lookup_selection == wqn::WordLookupSelection::kOnlineSearch),
            kTag,
            "draw online lookup choice");
        ESP_RETURN_ON_ERROR(
            draw_choice(182, "询问 AI", "临时释义", word.lookup_selection == wqn::WordLookupSelection::kAiLookup),
            kTag,
            "draw ai lookup choice");
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        return RefreshFrame(schedule);
    }

    if (!word.has_card) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 118, 360, "词库未同步"), kTag, "draw word empty title");
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 148, 360, word.hint), kTag, "draw word empty body");
        return RefreshFrame(schedule);
    }

    const std::string position =
        word.card_count == 0 ? "" : std::to_string(word.card_position) + "/" + std::to_string(std::max<uint16_t>(1, word.card_count));
    ESP_RETURN_ON_ERROR(DrawCenteredText(20, 68, 360, position), kTag, "draw word position");

    ESP_RETURN_ON_ERROR(DrawCenteredText(20, 94, 360, word.word), kTag, "draw word headword");
    if (!word.phonetic.empty()) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 118, 360, word.phonetic), kTag, "draw word phonetic");
    }

    if (word.mode == wqn::WordAppMode::kReviewFront) {
        DrawRect(74, 164, 252, 48);
        ESP_RETURN_ON_ERROR(DrawCenteredText(74, 181, 252, "先回忆释义"), kTag, "draw recall prompt");
    } else {
        ESP_RETURN_ON_ERROR(draw_word_back(), kTag, "draw word back");
    }

    ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
    return RefreshFrame(schedule);
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

esp_err_t DrawSettingsRow(int y, const std::string& title, const std::string& value, bool selected)
{
    constexpr int kX = 14;
    constexpr int kWidth = 372;
    constexpr int kHeight = 34;
    if (selected) {
        FillRect(kX - 4, y - 3, kWidth + 8, kHeight, true);
    } else {
        DrawHorizontalLine(kX, y + kHeight - 3, kWidth);
    }
    const bool black_text = !selected;
    ESP_RETURN_ON_ERROR(DrawClippedText(kX, y + 5, 210, title, black_text), kTag, "draw settings title");
    if (!value.empty()) {
        const int value_width = wqn::MeasureUtf8TextWidth(value.c_str());
        ESP_RETURN_ON_ERROR(
            DrawClippedText(std::max(kX + 220, kX + kWidth - value_width - 4), y + 5, 150, value, black_text),
            kTag,
            "draw settings value");
    }
    return ESP_OK;
}

esp_err_t DrawSettingsDialogBox(const std::string& title)
{
    FillRect(28, 44, 344, 216, false);
    DrawRect(28, 44, 344, 216);
    DrawHorizontalLine(44, 80, 312);
    DrawRect(118, 224, 164, 28);
    ESP_RETURN_ON_ERROR(DrawCenteredText(44, 58, 312, title), kTag, "draw settings dialog title");
    return DrawCenteredText(118, 231, 164, "确认");
}

esp_err_t RenderSettingsDialog(const wqn::SettingsAppState& settings)
{
    const wqn::SettingsDiagnosticsSnapshot& diag = settings.diagnostics;
    switch (settings.dialog) {
        case wqn::SettingsDialog::kAutoSync: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("自动同步间隔"), kTag, "draw auto sync dialog");
            int y = 94;
            for (size_t i = 0; i < sizeof(kAutoSyncOptions) / sizeof(kAutoSyncOptions[0]); ++i) {
                const bool selected = i == settings.auto_sync_selected;
                if (selected) {
                    FillRect(70, y - 3, 260, 24, true);
                }
                ESP_RETURN_ON_ERROR(
                    DrawCenteredText(70, y + 1, 260, wqn::AutoSyncIntervalLabel(kAutoSyncOptions[i]), !selected),
                    kTag,
                    "draw auto sync option");
                y += 27;
            }
            ESP_RETURN_ON_ERROR(DrawCenteredText(44, 202, 312, "上下选择"), kTag, "draw auto sync help");
            break;
        }
        case wqn::SettingsDialog::kBattery: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("电量详情"), kTag, "draw battery dialog");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 92, 300, "adc_raw: " + std::to_string(diag.adc_raw)), kTag, "draw battery raw");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 112, 300, "adc_mv: " + std::to_string(diag.adc_mv)), kTag, "draw battery adc");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 132, 300, "battery_mv: " + std::to_string(diag.battery_mv)), kTag, "draw battery mv");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 152, 300, "percent: " + std::to_string(diag.battery_percent) + "%"), kTag, "draw battery percent");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(52, 172, 300, std::string("charging/full: ") + (diag.charging ? "1" : "0") + "/" + (diag.full ? "1" : "0")),
                kTag,
                "draw battery flags");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 196, 300, "公式: (-V^2+9016V-19189000)/10000"), kTag, "draw battery formula");
            break;
        }
        case wqn::SettingsDialog::kStorage: {
            ESP_RETURN_ON_ERROR(DrawSettingsDialogBox("存储详情"), kTag, "draw storage dialog");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 92, 300, "Flash: " + BytesLabel(diag.flash_size)), kTag, "draw flash size");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(
                    52,
                    114,
                    300,
                    "NVS entries: " + std::to_string(diag.nvs_used_entries) + "/" + std::to_string(diag.nvs_total_entries)),
                kTag,
                "draw nvs entries");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 136, 300, "NVS free: " + std::to_string(diag.nvs_free_entries)), kTag, "draw nvs free");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 158, 300, "PSRAM total: " + BytesLabel(diag.psram_total)), kTag, "draw psram total");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 180, 300, "PSRAM free: " + BytesLabel(diag.psram_free)), kTag, "draw psram free");
            ESP_RETURN_ON_ERROR(DrawClippedText(52, 202, 300, "PSRAM used: " + BytesLabel(diag.psram_used)), kTag, "draw psram used");
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
    wqn::ClearEpdFramebuffer(true);

    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    std::string title = "设置";
    if (!diag.mac_label.empty()) {
        title += " (MAC: " + diag.mac_label + ")";
    }
    ESP_RETURN_ON_ERROR(DrawClippedText(10, 6, 250, title), kTag, "draw settings title");
    std::string status = CurrentClockLabel();
    if (!frame.home.wifi_label.empty()) {
        status += "  " + frame.home.wifi_label;
    }
    if (!frame.home.battery_label.empty()) {
        status += "  " + frame.home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    ESP_RETURN_ON_ERROR(
        DrawClippedText(std::max(10, wqn::kEpdWidth - status_width - 10), 6, std::min(180, status_width + 4), status),
        kTag,
        "draw settings status");

    const std::string auto_sync_label = wqn::AutoSyncIntervalLabel(settings.auto_sync_interval_min);
    const std::string battery_value =
        diag.full ? "满电" : (diag.charging ? "充电 " + std::to_string(diag.battery_percent) + "%" : std::to_string(diag.battery_percent) + "%");
    const std::string storage_value = "NVS " + std::to_string(diag.nvs_used_entries) + "/" + std::to_string(diag.nvs_total_entries);
    const std::string version_value = diag.firmware_version.empty() ? WQN_FIRMWARE_VERSION : diag.firmware_version;

    const std::string titles[kSettingsItemCount] = {
        "立即同步",
        "自动同步间隔",
        "电量",
        "存储详情",
        "固件版本",
        "恢复出厂",
    };
    const std::string values[kSettingsItemCount] = {
        settings.sync_status.empty() ? "空闲" : settings.sync_status,
        auto_sync_label,
        battery_value,
        storage_value,
        version_value,
        "",
    };

    int y = 42;
    for (size_t i = 0; i < kSettingsItemCount; ++i) {
        ESP_RETURN_ON_ERROR(DrawSettingsRow(y, titles[i], values[i], i == settings.selected), kTag, "draw settings row");
        y += 38;
    }
    ESP_RETURN_ON_ERROR(
        DrawClippedText(14, 274, 372, settings.notice.empty() ? "上下选择，确认操作，长按确认返回首页" : settings.notice),
        kTag,
        "draw settings help");

    if (settings.dialog != wqn::SettingsDialog::kNone) {
        ESP_RETURN_ON_ERROR(RenderSettingsDialog(settings), kTag, "draw settings dialog");
    }
    return RefreshFrame(schedule == RefreshSchedule::kCommit ? RefreshSchedule::kCommit : RefreshSchedule::kConfig);
}

esp_err_t DrawMetricCard(int x, int y, int width, const wqn::HomeMetric& metric)
{
    constexpr int kCardHeight = 55;
    DrawRect(x, y, width, kCardHeight);
    ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 8, width, metric.value), kTag, "draw home metric value");
    ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 31, width, metric.label), kTag, "draw home metric label");
    return ESP_OK;
}

esp_err_t DrawHomeTaskRow(int x, int y, int width, int index, const wqn::HomeTask& task, bool selected)
{
    constexpr int kRowHeight = 47;
    if (selected) {
        FillRect(x - 4, y - 2, width + 8, kRowHeight, true);
    }
    const bool black_text = !selected;
    const int text_width = width - 58;
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(x, y + 4, std::to_string(index).c_str(), black_text),
        kTag,
        "draw home task index");
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 20, y + 3, text_width, task.title, black_text), kTag, "draw home task title");
    ESP_RETURN_ON_ERROR(
        DrawClippedText(x + 20, y + 24, text_width, task.subtitle, black_text),
        kTag,
        "draw home task subtitle");
    if (!task.tag.empty()) {
        const int tag_width = std::min(48, std::max(32, wqn::MeasureUtf8TextWidth(task.tag.c_str()) + 8));
        DrawRect(x + width - tag_width, y + 4, tag_width, 18);
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(x + width - tag_width, y + 5, tag_width, task.tag, black_text),
            kTag,
            "draw home task tag");
    }
    return ESP_OK;
}

esp_err_t RenderHomeToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::HomeSummary& home = frame.home;
    wqn::ClearEpdFramebuffer(true);

    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 6, "首页", true), kTag, "draw home title");
    std::string status = home.wifi_label;
    if (!home.battery_label.empty()) {
        status += "  " + home.battery_label;
    }
    const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - status_width - 10), 6, status.c_str(), true),
        kTag,
        "draw home status");

    DrawRect(10, 35, 380, 26);
    ESP_RETURN_ON_ERROR(DrawCenteredText(10, 39, 380, home.primary_time_line), kTag, "draw home primary time");

    constexpr int kCardY = 69;
    constexpr int kCardWidth = 121;
    ESP_RETURN_ON_ERROR(DrawMetricCard(10, kCardY, kCardWidth, home.review_metric), kTag, "draw review metric");
    ESP_RETURN_ON_ERROR(DrawMetricCard(139, kCardY, kCardWidth, home.todo_metric), kTag, "draw todo metric");
    ESP_RETURN_ON_ERROR(DrawMetricCard(269, kCardY, kCardWidth, home.word_metric), kTag, "draw word metric");

    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 138, "当前进行", true), kTag, "draw home section title");
    const std::string subtitle = wqn::TruncateUtf8TextToWidth(home.current_status, 210);
    const int subtitle_width = wqn::MeasureUtf8TextWidth(subtitle.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(10, wqn::kEpdWidth - subtitle_width - 10), 138, subtitle.c_str(), true),
        kTag,
        "draw home section status");

    DrawHorizontalLine(10, 161, 380);
    int y = 168;
    const size_t visible_tasks = std::min<size_t>(home.tasks.size(), 2);
    if (visible_tasks == 0) {
        ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, y, "暂无当前任务", true), kTag, "draw home empty");
    }
    for (size_t i = 0; i < visible_tasks; ++i) {
        ESP_RETURN_ON_ERROR(
            DrawHomeTaskRow(14, y, 372, static_cast<int>(i + 1), home.tasks[i], i == frame.selected_home_task),
            kTag,
            "draw home task");
        y += 53;
    }

    return RefreshFrame(schedule);
}

bool IsAsciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string TodoDueTimeLabel(const std::string& due_at)
{
    for (size_t i = 0; i + 4 < due_at.size(); ++i) {
        if (!IsAsciiDigit(due_at[i]) || !IsAsciiDigit(due_at[i + 1]) || due_at[i + 2] != ':' ||
            !IsAsciiDigit(due_at[i + 3]) || !IsAsciiDigit(due_at[i + 4])) {
            continue;
        }
        const int hour = (due_at[i] - '0') * 10 + (due_at[i + 1] - '0');
        const int minute = (due_at[i + 3] - '0') * 10 + (due_at[i + 4] - '0');
        if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
            return due_at.substr(i, 5);
        }
    }
    return "--:--";
}

std::string TodoDueDateLabel(const std::string& due_at)
{
    if (due_at.size() >= 10 && IsAsciiDigit(due_at[0]) && IsAsciiDigit(due_at[1]) &&
        IsAsciiDigit(due_at[2]) && IsAsciiDigit(due_at[3]) && due_at[4] == '-' &&
        IsAsciiDigit(due_at[5]) && IsAsciiDigit(due_at[6]) && due_at[7] == '-' &&
        IsAsciiDigit(due_at[8]) && IsAsciiDigit(due_at[9])) {
        return due_at.substr(0, 10);
    }
    return "无到期日期";
}

const char* TodoSyncStatusText(wqn::TodoSyncStatus status)
{
    switch (status) {
        case wqn::TodoSyncStatus::kLoading:
            return "同步中";
        case wqn::TodoSyncStatus::kReady:
            return "已同步";
        case wqn::TodoSyncStatus::kSyncFailed:
            return "同步失败";
        case wqn::TodoSyncStatus::kCompleting:
            return "完成中";
        case wqn::TodoSyncStatus::kCompleteFailed:
            return "完成失败";
        case wqn::TodoSyncStatus::kCompleted:
            return "已完成";
        case wqn::TodoSyncStatus::kAuthRequired:
            return "未配对";
        case wqn::TodoSyncStatus::kIdle:
        default:
            return "待同步";
    }
}

std::string TodoItemStatusText(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status)
{
    if (sync_status == wqn::TodoSyncStatus::kLoading) {
        return "同步中";
    }
    if (sync_status == wqn::TodoSyncStatus::kSyncFailed) {
        return "同步失败";
    }
    if (selected && sync_status == wqn::TodoSyncStatus::kCompleting) {
        return "完成中";
    }
    if (selected && sync_status == wqn::TodoSyncStatus::kCompleteFailed) {
        return "完成失败";
    }
    if (!item.completed_at.empty() || item.status == "completed" || item.status == "done") {
        return "已完成";
    }
    if (item.status == "cancelled" || item.status == "canceled") {
        return "已取消";
    }
    if (item.status == "overdue") {
        return "已逾期";
    }
    if (item.status == "pending" || item.status.empty()) {
        return "待完成";
    }
    return item.status;
}

std::string TodoCardMetaLabel(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status)
{
    const std::string subject = item.subject_name.empty() ? "未分类" : item.subject_name;
    return subject + " · " + TodoItemStatusText(item, selected, sync_status) + " · " + TodoDueDateLabel(item.due_at);
}

std::string TodoStatusNote(const wqn::TodoUiState& todo)
{
    switch (todo.sync_status) {
        case wqn::TodoSyncStatus::kLoading:
            return "正在同步 Todo";
        case wqn::TodoSyncStatus::kCompleting:
            return "正在完成选中待办";
        case wqn::TodoSyncStatus::kCompleteFailed:
            return "完成失败，请稍后重试";
        case wqn::TodoSyncStatus::kCompleted:
            return "已完成选中待办";
        case wqn::TodoSyncStatus::kSyncFailed:
            return "同步失败，显示当前缓存";
        case wqn::TodoSyncStatus::kAuthRequired:
            return "请重新配对后同步";
        case wqn::TodoSyncStatus::kReady:
        case wqn::TodoSyncStatus::kIdle:
        default:
            break;
    }
    return todo.status_message;
}

int TodoPendingCount(const wqn::TodoUiState& todo)
{
    if (todo.total_pending > 0) {
        return todo.total_pending;
    }
    return static_cast<int>(todo.todos.size());
}

bool ParseTodoDueTime(const std::string& due_at, std::tm* due_tm)
{
    if (due_tm == nullptr || due_at.size() < 10 || !IsAsciiDigit(due_at[0]) || !IsAsciiDigit(due_at[1]) ||
        !IsAsciiDigit(due_at[2]) || !IsAsciiDigit(due_at[3]) || due_at[4] != '-' || !IsAsciiDigit(due_at[5]) ||
        !IsAsciiDigit(due_at[6]) || due_at[7] != '-' || !IsAsciiDigit(due_at[8]) || !IsAsciiDigit(due_at[9])) {
        return false;
    }

    std::tm parsed = {};
    parsed.tm_year = (due_at[0] - '0') * 1000 + (due_at[1] - '0') * 100 + (due_at[2] - '0') * 10 +
                     (due_at[3] - '0') - 1900;
    parsed.tm_mon = (due_at[5] - '0') * 10 + (due_at[6] - '0') - 1;
    parsed.tm_mday = (due_at[8] - '0') * 10 + (due_at[9] - '0');
    parsed.tm_hour = 23;
    parsed.tm_min = 59;
    parsed.tm_sec = 59;

    if (due_at.size() >= 16 && due_at[10] == 'T' && IsAsciiDigit(due_at[11]) && IsAsciiDigit(due_at[12]) &&
        due_at[13] == ':' && IsAsciiDigit(due_at[14]) && IsAsciiDigit(due_at[15])) {
        parsed.tm_hour = (due_at[11] - '0') * 10 + (due_at[12] - '0');
        parsed.tm_min = (due_at[14] - '0') * 10 + (due_at[15] - '0');
        if (due_at.size() >= 19 && due_at[16] == ':' && IsAsciiDigit(due_at[17]) && IsAsciiDigit(due_at[18])) {
            parsed.tm_sec = (due_at[17] - '0') * 10 + (due_at[18] - '0');
        } else {
            parsed.tm_sec = 0;
        }
    }

    if (parsed.tm_mon < 0 || parsed.tm_mon > 11 || parsed.tm_mday < 1 || parsed.tm_mday > 31 ||
        parsed.tm_hour < 0 || parsed.tm_hour > 23 || parsed.tm_min < 0 || parsed.tm_min > 59 ||
        parsed.tm_sec < 0 || parsed.tm_sec > 59) {
        return false;
    }
    *due_tm = parsed;
    return true;
}

int TodoOverdueCount(const wqn::TodoUiState& todo)
{
    int count = 0;
    const std::time_t now = CurrentUnixTime();
    const bool has_valid_clock = now >= kMinReasonableUnixTime;
    for (const wqn::WqnTodoItem& item : todo.todos) {
        if (item.status == "overdue") {
            ++count;
            continue;
        }
        if (!has_valid_clock || item.due_at.empty() || item.status == "completed" || item.status == "done" ||
            item.status == "cancelled" || item.status == "canceled") {
            continue;
        }
        std::tm due_tm = {};
        if (!ParseTodoDueTime(item.due_at, &due_tm)) {
            continue;
        }
        const std::time_t due_time = mktime(&due_tm);
        if (due_time != static_cast<std::time_t>(-1) && due_time < now) {
            ++count;
        }
    }
    return count;
}

size_t TodoVisibleStart(const wqn::TodoUiState& todo, size_t selected, size_t visible_count)
{
    if (todo.todos.size() <= visible_count) {
        return 0;
    }
    if (selected + 1 >= visible_count) {
        return std::min(selected + 1 - visible_count, todo.todos.size() - visible_count);
    }
    return 0;
}

void DrawDashedVerticalLine(int x, int y, int height)
{
    constexpr int kDash = 6;
    constexpr int kGap = 5;
    for (int offset = 0; offset < height; offset += kDash + kGap) {
        DrawVerticalLine(x, y + offset, std::min(kDash, height - offset));
    }
}

void DrawTimelineNode(int cx, int cy, bool selected)
{
    constexpr int kRadius = 6;
    constexpr int kInnerRadius = 3;
    for (int dy = -kRadius; dy <= kRadius; ++dy) {
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
            const int distance = dx * dx + dy * dy;
            if (distance > kRadius * kRadius) {
                continue;
            }
            wqn::DrawEpdPixel(cx + dx, cy + dy, false);
            const bool black = selected || distance >= kInnerRadius * kInnerRadius;
            wqn::DrawEpdPixel(cx + dx, cy + dy, black);
        }
    }
}

esp_err_t DrawTodoStatusBar(const wqn::TodoUiState& todo, const wqn::HomeSummary& home)
{
    DrawHorizontalLine(0, 27, wqn::kEpdWidth);
    ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(10, 6, "Todo", true), kTag, "draw todo title");

    std::string status = "今日 " + std::to_string(TodoPendingCount(todo));
    status += "  逾期 ";
    status += std::to_string(TodoOverdueCount(todo));
    status += "  ";
    status += TodoSyncStatusText(todo.sync_status);
    status += "  ";
    status += CurrentClockLabel();
    status += "  ";
    status += home.wifi_label.empty() ? "WiFi" : home.wifi_label;
    if (!home.battery_label.empty()) {
        status += "  ";
        status += home.battery_label;
    }

    constexpr int kStatusMaxWidth = wqn::kEpdWidth - 74;
    const std::string clipped = wqn::TruncateUtf8TextToWidth(status, kStatusMaxWidth);
    const int status_width = wqn::MeasureUtf8TextWidth(clipped.c_str());
    ESP_RETURN_ON_ERROR(
        wqn::DrawUtf8Text(std::max(74, wqn::kEpdWidth - status_width - 10), 6, clipped.c_str(), true),
        kTag,
        "draw todo status");
    return ESP_OK;
}

esp_err_t DrawTodoCard(const wqn::WqnTodoItem& item, bool selected, wqn::TodoSyncStatus sync_status, int x, int y, int width, int height)
{
    DrawRect(x, y, width, height);
    if (selected) {
        DrawRect(x + 3, y + 3, width - 6, height - 6);
    }

    const std::string title = item.title.empty() ? "未命名 Todo" : item.title;
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 8, y + 7, 48, TodoDueTimeLabel(item.due_at)), kTag, "draw todo time");
    ESP_RETURN_ON_ERROR(DrawClippedText(x + 62, y + 7, width - 70, title), kTag, "draw todo title text");
    ESP_RETURN_ON_ERROR(
        DrawClippedText(x + 8, y + 31, width - 16, TodoCardMetaLabel(item, selected, sync_status)),
        kTag,
        "draw todo meta");
    return ESP_OK;
}

esp_err_t DrawTodoEmptyState(const wqn::TodoUiState& todo)
{
    std::string title = "暂无 Todo";
    std::string body = "今天没有待办事项";
    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired) {
        title = "设备未配对";
        body = "请先在网页端扫码配对";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kSyncFailed) {
        title = "Todo 同步失败";
        body = "请检查 WiFi 后重试";
    } else if (todo.sync_status == wqn::TodoSyncStatus::kLoading) {
        title = "正在同步 Todo";
        body = "正在从云端获取今天的待办";
    }

    DrawRect(28, 72, 344, 128);
    ESP_RETURN_ON_ERROR(DrawCenteredText(36, 106, 328, title), kTag, "draw todo empty title");
    ESP_RETURN_ON_ERROR(DrawCenteredText(36, 136, 328, body), kTag, "draw todo empty body");
    ESP_RETURN_ON_ERROR(DrawCenteredText(36, 174, 328, TodoSyncStatusText(todo.sync_status)), kTag, "draw todo empty status");
    return ESP_OK;
}

esp_err_t RenderTodoToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::TodoUiState& todo = frame.todo;
    wqn::ClearEpdFramebuffer(true);
    ESP_RETURN_ON_ERROR(DrawTodoStatusBar(todo, frame.home), kTag, "draw todo status bar");

    if (todo.sync_status == wqn::TodoSyncStatus::kAuthRequired || todo.todos.empty()) {
        ESP_RETURN_ON_ERROR(DrawTodoEmptyState(todo), kTag, "draw todo empty state");
        return RefreshFrame(schedule);
    }

    constexpr size_t kMaxVisibleTodos = 4;
    constexpr int kTimelineX = 34;
    constexpr int kCardX = 54;
    constexpr int kCardY = 38;
    constexpr int kCardWidth = 336;
    constexpr int kCardHeight = 54;
    constexpr int kCardGap = 8;

    const size_t selected = std::min(todo.selected, todo.todos.size() - 1);
    const size_t visible_count = std::min(kMaxVisibleTodos, todo.todos.size());
    const size_t start = TodoVisibleStart(todo, selected, visible_count);
    const int timeline_start_y = kCardY + 13;
    const int timeline_height = static_cast<int>((visible_count - 1) * (kCardHeight + kCardGap)) + 1;
    DrawDashedVerticalLine(kTimelineX, timeline_start_y, std::max(1, timeline_height));

    for (size_t visible_index = 0; visible_index < visible_count; ++visible_index) {
        const size_t item_index = start + visible_index;
        const bool is_selected = item_index == selected;
        const int y = kCardY + static_cast<int>(visible_index) * (kCardHeight + kCardGap);
        DrawTimelineNode(kTimelineX, y + 13, is_selected);
        ESP_RETURN_ON_ERROR(
            DrawTodoCard(todo.todos[item_index], is_selected, todo.sync_status, kCardX, y, kCardWidth, kCardHeight),
            kTag,
            "draw todo card");
    }

    const std::string note = TodoStatusNote(todo);
    if (!note.empty()) {
        ESP_RETURN_ON_ERROR(DrawClippedText(kCardX, 282, kCardWidth, note), kTag, "draw todo message");
    }
    return RefreshFrame(schedule);
}

std::string CurrentIsoTimestamp()
{
    const std::time_t now = CurrentUnixTime();
    if (now < kMinReasonableUnixTime) {
        return "";
    }

    std::tm time_info = {};
    gmtime_r(&now, &time_info);
    char buffer[24] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &time_info) == 0) {
        return "";
    }
    return buffer;
}

std::string CurrentClockLabel()
{
    const std::time_t now = CurrentUnixTime();
    if (now < kMinReasonableUnixTime) {
        return "--:--";
    }

    std::tm time_info = {};
    localtime_r(&now, &time_info);
    char buffer[8] = {};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M", &time_info) == 0) {
        return "--:--";
    }
    return buffer;
}

std::string ChooseHomePrimaryTimeLine(const wqn::TimeAppState& time_app)
{
    const std::string active_timer = wqn::TimeAppPrimaryLine(time_app);
    if (!active_timer.empty()) {
        return active_timer;
    }
    return CurrentClockLabel();
}

void UpdateHomePrimaryTimeLine(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }
    state->home.primary_time_line = ChooseHomePrimaryTimeLine(state->time_app);
}

int CountReviewDueLikeProblems(const std::vector<wqn::CachedProblem>& problems)
{
    int count = 0;
    for (const wqn::CachedProblem& problem : problems) {
        if (problem.status != "mastered") {
            ++count;
        }
    }
    return count;
}

bool LoadValidTokenForTodo(std::string* token)
{
    if (token == nullptr) {
        return false;
    }
    token->clear();
    const esp_err_t result = wqn::LoadAccessToken(token);
    return result == ESP_OK && !token->empty() && wqn::IsValidAccessToken(*token);
}

bool IsTodoCloudBusy()
{
    return g_todo_cloud_busy;
}

bool QueueTodoCloudRequest(const TodoCloudRequest& request)
{
    if (g_todo_request_queue == nullptr || IsTodoCloudBusy()) {
        return false;
    }
    if (xQueueSend(g_todo_request_queue, &request, 0) != pdTRUE) {
        return false;
    }
    g_todo_cloud_busy = true;
    return true;
}

bool QueueTodoRefresh()
{
    TodoCloudRequest request;
    request.op = TodoCloudOp::kRefresh;
    return QueueTodoCloudRequest(request);
}

bool QueueTodoRefreshCursor(const std::string& cursor)
{
    if (cursor.empty()) {
        return false;
    }
    TodoCloudRequest request;
    request.op = TodoCloudOp::kRefresh;
    std::snprintf(request.cursor, sizeof(request.cursor), "%s", cursor.c_str());
    return QueueTodoCloudRequest(request);
}

bool QueueTodoComplete(const std::string& todo_id)
{
    if (todo_id.empty()) {
        return false;
    }
    TodoCloudRequest request;
    request.op = TodoCloudOp::kComplete;
    std::snprintf(request.todo_id, sizeof(request.todo_id), "%s", todo_id.c_str());
    return QueueTodoCloudRequest(request);
}

void ApplyTodoList(wqn::UiState* state, wqn::WqnTodoListPage page)
{
    if (state == nullptr) {
        return;
    }
    const int selected_index = page.selected_index;
    state->todo.todos = std::move(page.todos);
    state->todo.loaded_once = true;
    state->todo.total_pending = page.total > 0 ? page.total : static_cast<int>(state->todo.todos.size());
    state->todo.previous_cursor = page.previous_cursor;
    state->todo.next_cursor = page.next_cursor;
    state->todo.has_earlier = page.has_earlier;
    state->todo.has_later = page.has_later;
    if (!state->todo.todos.empty()) {
        if (selected_index >= 0 && selected_index < static_cast<int>(state->todo.todos.size())) {
            state->todo.selected = static_cast<size_t>(selected_index);
        } else if (state->todo.selected >= state->todo.todos.size()) {
            state->todo.selected = state->todo.todos.size() - 1;
        }
    }
    state->todo.sync_status = wqn::TodoSyncStatus::kReady;
    state->todo.status_message.clear();
    wqn::ClampUiSelection(state);
}

bool ApplyTodoCloudResult(wqn::UiState* state, const TodoCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }

    if (result.op == TodoCloudOp::kRefresh) {
        if (result.result == ESP_OK) {
            ApplyTodoList(state, result.page);
        } else if (result.auth_required) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
            state->todo.total_pending = static_cast<int>(state->todo.todos.size());
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
            state->todo.status_message = "Todo sync failed";
            state->todo.total_pending = static_cast<int>(state->todo.todos.size());
        }
        BuildHomeSummary(state);
        return true;
    }

    if (result.op == TodoCloudOp::kComplete) {
        if (result.result == ESP_OK) {
            const std::string completed_id =
                !result.todo.id.empty() ? result.todo.id : std::string(result.todo_id);
            auto it = std::find_if(
                state->todo.todos.begin(),
                state->todo.todos.end(),
                [&completed_id](const wqn::WqnTodoItem& item) { return item.id == completed_id; });
            if (it != state->todo.todos.end()) {
                state->todo.todos.erase(it);
            }
            if (state->todo.total_pending > 0) {
                --state->todo.total_pending;
            }
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleted;
            state->todo.status_message = "Completed";
        } else if (result.auth_required) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleteFailed;
            state->todo.status_message = "Complete failed";
        }
        wqn::ClampUiSelection(state);
        BuildHomeSummary(state);
        return true;
    }

    return false;
}

bool ApplyWordCloudResult(wqn::UiState* state, const WordCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }
    if (result.op == WordCloudOp::kPackSync) {
        if (result.result == ESP_OK) {
            wqn::ApplyWordPackIndex(&state->word_app, result.pack_index, result.message);
        } else {
            state->word_app.cloud_sync_failed = true;
            state->word_app.cloud_loaded_once = true;
            state->word_app.cloud_sync_requested = false;
            state->word_app.message = result.auth_required ? "请重新配对" : "单词同步失败";
        }
        BuildHomeSummary(state);
        return true;
    }

    if (result.op == WordCloudOp::kSubmit) {
        if (result.result == ESP_OK) {
            if (std::strcmp(result.outcome, "unknown") == 0) {
                state->word_app.message = "已加入遗忘的单词";
            } else if (std::strcmp(result.outcome, "known") == 0) {
                state->word_app.message = "已记录";
            } else {
                state->word_app.message = "已同步";
            }
        } else {
            state->word_app.message = result.auth_required ? "请重新配对" : "单词同步失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kSearch) {
        if (result.result == ESP_OK) {
            wqn::ApplyWordSearchResult(&state->word_app, result.search);
        } else {
            state->word_app.message = result.auth_required ? "请重新配对" : "在线搜索失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kAiLookup) {
        if (result.result == ESP_OK) {
            wqn::ApplyWordAiLookupResult(&state->word_app, result.lookup);
        } else {
            state->word_app.message = result.auth_required ? "请重新配对" : "AI 查词失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    return false;
}

void SendTodoCloudResult(TodoCloudResult* result)
{
    if (result == nullptr) {
        return;
    }
    if (g_todo_result_queue == nullptr || xQueueSend(g_todo_result_queue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        g_todo_cloud_busy = false;
        delete result;
    }
}

bool IsWordCloudBusy()
{
    return g_word_cloud_busy;
}

bool QueueWordCloudRequest(const WordCloudRequest& request)
{
    if (g_word_request_queue == nullptr || IsWordCloudBusy()) {
        return false;
    }
    if (xQueueSend(g_word_request_queue, &request, 0) != pdTRUE) {
        return false;
    }
    g_word_cloud_busy = true;
    return true;
}

bool QueueWordReviewRefresh()
{
    WordCloudRequest request;
    request.op = WordCloudOp::kPackSync;
    return QueueWordCloudRequest(request);
}

bool QueueWordReviewSubmit(const wqn::WqnWordReviewSubmission& submission, const std::string& word)
{
    if (submission.word_id.empty() || submission.outcome.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kSubmit;
    std::snprintf(request.word_id, sizeof(request.word_id), "%s", submission.word_id.c_str());
    std::snprintf(request.outcome, sizeof(request.outcome), "%s", submission.outcome.c_str());
    std::snprintf(request.word, sizeof(request.word), "%s", word.c_str());
    return QueueWordCloudRequest(request);
}

bool QueueWordSearch(const wqn::WqnWordSearchRequest& search)
{
    if (search.query.empty() && search.prefix.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kSearch;
    const std::string query = !search.query.empty() ? search.query : search.prefix;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return QueueWordCloudRequest(request);
}

bool QueueWordAiLookup(const wqn::WqnWordAiLookupRequest& lookup)
{
    if (lookup.query.empty() && lookup.prefix.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kAiLookup;
    const std::string query = !lookup.query.empty() ? lookup.query : lookup.prefix;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return QueueWordCloudRequest(request);
}

void SendWordCloudResult(WordCloudResult* result)
{
    if (result == nullptr) {
        return;
    }
    if (g_word_result_queue == nullptr || xQueueSend(g_word_result_queue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        g_word_cloud_busy = false;
        delete result;
    }
}

void WordCloudTask(void*)
{
    ESP_LOGI(kTag, "Word cloud task started");
    while (true) {
        WordCloudRequest request;
        if (xQueueReceive(g_word_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        WordCloudResult* result = new (std::nothrow) WordCloudResult();
        if (result == nullptr) {
            g_word_cloud_busy = false;
            ESP_LOGW(kTag, "alloc Word cloud result failed");
            continue;
        }
        result->op = request.op;
        std::snprintf(result->word_id, sizeof(result->word_id), "%s", request.word_id);
        std::snprintf(result->outcome, sizeof(result->outcome), "%s", request.outcome);
        std::snprintf(result->word, sizeof(result->word), "%s", request.word);
        result->message.clear();

        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            result->auth_required = true;
            result->result = ESP_ERR_INVALID_STATE;
            SendWordCloudResult(result);
            continue;
        }

        if (request.op == WordCloudOp::kPackSync) {
            wqn::WqnWordPackManifest manifest;
            result->result = wqn::FetchWordPackManifest(token, &manifest);
            if (result->result == ESP_OK) {
                for (const wqn::WqnWordPackManifestItem& item : manifest.packs) {
                    if (!wqn::WordPackNeedsDownload(item)) {
                        continue;
                    }
                    std::string pack_body;
                    result->result = wqn::DownloadWordPack(token, item, &pack_body);
                    if (result->result != ESP_OK) {
                        break;
                    }
                    result->result = wqn::SaveWordPackFromBytes(item, pack_body);
                    if (result->result != ESP_OK) {
                        break;
                    }
                }
            }
            if (result->result == ESP_OK) {
                result->result = wqn::SaveWordPackManifest(manifest);
            }
            if (result->result == ESP_OK) {
                result->result = wqn::LoadWordPackIndex(&result->pack_index);
                result->message = result->pack_index.status_message;
            }
        } else if (request.op == WordCloudOp::kSubmit) {
            wqn::WqnWordReviewSubmission submission;
            submission.word_id = request.word_id;
            submission.outcome = request.outcome;
            submission.mode = "sequential";
            result->result = wqn::SubmitWordReview(token, submission, &result->submit);
        } else if (request.op == WordCloudOp::kSearch) {
            wqn::WqnWordSearchRequest search;
            search.query = request.query;
            search.limit = 8;
            result->result = wqn::SearchWords(token, search, &result->search);
        } else if (request.op == WordCloudOp::kAiLookup) {
            wqn::WqnWordAiLookupRequest lookup;
            lookup.query = request.query;
            result->result = wqn::LookupWordWithAi(token, lookup, &result->lookup);
        } else {
            result->result = ESP_ERR_INVALID_ARG;
        }

        if (result->result != ESP_OK) {
            std::string after_token;
            result->auth_required = !LoadValidTokenForTodo(&after_token);
        }
        SendWordCloudResult(result);
    }
}

void TodoCloudTask(void*)
{
    ESP_LOGI(kTag, "Todo cloud task started");
    while (true) {
        TodoCloudRequest request;
        if (xQueueReceive(g_todo_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        TodoCloudResult* result = new (std::nothrow) TodoCloudResult();
        if (result == nullptr) {
            g_todo_cloud_busy = false;
            ESP_LOGW(kTag, "alloc Todo cloud result failed");
            continue;
        }
        result->op = request.op;
        std::snprintf(result->todo_id, sizeof(result->todo_id), "%s", request.todo_id);

        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            result->auth_required = true;
            result->result = ESP_ERR_INVALID_STATE;
            SendTodoCloudResult(result);
            continue;
        }

        if (request.op == TodoCloudOp::kRefresh) {
            wqn::WqnTodoTimelineRequest timeline_request;
            timeline_request.cursor = request.cursor;
            timeline_request.limit = 24;
            result->result = wqn::FetchTodoTimeline(token, timeline_request, &result->page);
        } else if (request.op == TodoCloudOp::kComplete) {
            result->result = wqn::CompleteTodo(token, request.todo_id, &result->todo);
        } else {
            result->result = ESP_ERR_INVALID_ARG;
        }

        if (result->result != ESP_OK) {
            std::string after_token;
            result->auth_required = !LoadValidTokenForTodo(&after_token);
        }
        SendTodoCloudResult(result);
    }
}

bool RefreshTodosFromCloud(wqn::UiState* state)
{
    if (state == nullptr) {
        return false;
    }

    {
        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
            state->todo.total_pending = static_cast<int>(state->todo.todos.size());
            return true;
        }

        if (!QueueTodoRefresh()) {
            if (IsTodoCloudBusy()) {
                state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
                state->todo.status_message = "Todo syncing";
            } else {
                state->todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
                state->todo.status_message = "Todo queue failed";
            }
            return true;
        }

        state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
        state->todo.status_message = "Todo syncing";
        return true;
    }

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
        state->todo.status_message = "请重新配对";
        state->todo.total_pending = static_cast<int>(state->todo.todos.size());
        return true;
    }

    state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
    wqn::WqnTodoListPage page;
    const esp_err_t result = wqn::FetchTodoTimeline(token, &page);
    if (result == ESP_OK) {
        ApplyTodoList(state, std::move(page));
        return true;
    }

    std::string after_token;
    if (!LoadValidTokenForTodo(&after_token)) {
        state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
        state->todo.status_message = "请重新配对";
    } else {
        state->todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
        state->todo.status_message = "Todo 同步失败";
    }
    state->todo.total_pending = static_cast<int>(state->todo.todos.size());
    ESP_LOGW(kTag, "todo list refresh failed: %s", esp_err_to_name(result));
    return true;
}

RefreshSchedule CompleteSelectedTodo(wqn::UiState* state)
{
    if (state == nullptr || state->screen != wqn::UiScreen::kTodo || state->todo.todos.empty()) {
        return RefreshSchedule::kNone;
    }
    wqn::ClampUiSelection(state);
    const size_t selected = state->todo.selected;
    if (selected >= state->todo.todos.size()) {
        return RefreshSchedule::kNone;
    }

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
        state->todo.status_message = "请重新配对";
        return RefreshSchedule::kCommit;
    }

    const std::string todo_id = state->todo.todos[selected].id;
    if (!QueueTodoComplete(todo_id)) {
        if (IsTodoCloudBusy()) {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleting;
            state->todo.status_message = "完成中";
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleteFailed;
            state->todo.status_message = "Todo 完成排队失败";
        }
        return RefreshSchedule::kCommit;
    }

    state->todo.sync_status = wqn::TodoSyncStatus::kCompleting;
    state->todo.status_message = "完成中";
    return RefreshSchedule::kCommit;
}

void BuildHomeSummary(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }

    wqn::HomeSummary home;
    home.wifi_label = state->status.wifi_connected ? "WiFi" : "离线";
    BatteryReading battery = {};
    home.battery_label = ReadBatteryStatus(&battery) ? BatteryLabel(battery) : "--%";

    // UI contract: one line only. Source priority is pomodoro > countdown > clock.
    home.primary_time_line = ChooseHomePrimaryTimeLine(state->time_app);

    const int review_count = CountReviewDueLikeProblems(state->problems);
    home.review_metric.value = std::to_string(review_count);
    home.review_metric.label = "今日复习";
    home.todo_metric.value = std::to_string(std::max(0, state->todo.total_pending));
    home.todo_metric.label = "今日 Todo";
    home.word_metric.value = wqn::WordAppProgressLabel(state->word_app);
    home.word_metric.label = "单词进度";
    home.current_status =
        "本地 " + std::to_string(state->problems.size()) + " 题 · 待上传 " +
        std::to_string(state->status.pending_reviews);

    home.tasks.clear();
    if (!state->problems.empty()) {
        const wqn::CachedProblem& problem = state->problems[std::min(state->selected_problem, state->problems.size() - 1)];
        wqn::HomeTask task;
        task.title = problem.title.empty() ? problem.id : problem.title;
        task.subtitle = "错题复习" + std::string(problem.status == "mastered" ? "，已掌握" : "，待复习");
        task.tag = "错题";
        home.tasks.push_back(std::move(task));
    } else {
        home.tasks.push_back(wqn::HomeTask{"同步错题后开始复习", "当前没有本地题目缓存", "错题"});
    }
    home.tasks.push_back(wqn::HomeTask{"单词复习", wqn::WordAppStatusLine(state->word_app), "单词"});

    state->home = std::move(home);
    wqn::ClampUiSelection(state);
}

void CheckBatteryProtection()
{
    BatteryReading battery = {};
    if (ReadBatteryStatus(&battery)) {
        CheckLowBatteryProtection(&battery);
    } else {
        CheckLowBatteryProtection(nullptr);
    }
}

bool LoadUiState(wqn::UiState* state)
{
    if (state == nullptr) {
        return false;
    }

    std::vector<wqn::CachedProblem> problems;
    esp_err_t result = wqn::LoadProblems(&problems);
    if (result == ESP_OK) {
        state->problems = std::move(problems);
    } else {
        ESP_LOGW(kTag, "load UI problem cache failed: %s", esp_err_to_name(result));
    }

    result = wqn::InitWordApp(&state->word_app);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "init word app failed: %s", esp_err_to_name(result));
    }

    std::vector<wqn::PendingReviewResult> pending;
    result = wqn::LoadPendingReviewResults(&pending);
    if (result == ESP_OK) {
        state->status.pending_reviews = static_cast<int>(pending.size());
    } else {
        ESP_LOGW(kTag, "load UI pending queue failed: %s", esp_err_to_name(result));
    }

    std::string token;
    result = wqn::LoadAccessToken(&token);
    if (result == ESP_OK && !token.empty() && wqn::IsValidAccessToken(token)) {
        state->status.paired = true;
        state->status.token_mask = wqn::MaskTokenForLog(token);
    } else {
        state->status.paired = false;
        state->status.token_mask.clear();
    }

#if CONFIG_WQN_WIFI_STA_ENABLE
    state->status.wifi_enabled = true;
    state->status.wifi_connected = wqn::IsWifiStationConnected();
#else
    state->status.wifi_enabled = false;
    state->status.wifi_connected = false;
#endif

    UpdateSettingsDiagnostics(state);
    wqn::ClampUiSelection(state);
    BuildHomeSummary(state);
    return true;
}

RefreshSchedule QueueSelectedReview(wqn::UiState* state)
{
    if (state == nullptr || state->problems.empty() || state->selected_problem >= state->problems.size()) {
        return RefreshSchedule::kNone;
    }

    const wqn::CachedProblem& problem = state->problems[state->selected_problem];
    wqn::PendingReviewResult review;
    review.problem_id = problem.id;
    review.selected_status = wqn::ReviewChoiceStatus(state->selected_review);
    review.is_correct = state->selected_review == wqn::ReviewChoice::kMastered;
    review.created_at = CurrentIsoTimestamp();

    const esp_err_t result = wqn::EnqueueReviewResult(review);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "enqueue review failed: %s", esp_err_to_name(result));
        state->last_review_message = "保存失败";
        state->screen = wqn::UiScreen::kReviewQueued;
        return RefreshSchedule::kCommit;
    }

    std::vector<wqn::PendingReviewResult> pending;
    if (wqn::LoadPendingReviewResults(&pending) == ESP_OK) {
        state->status.pending_reviews = static_cast<int>(pending.size());
    } else {
        ++state->status.pending_reviews;
    }

    state->last_review_message = std::string("已保存：") + wqn::ReviewChoiceLabel(state->selected_review);
    state->status.last_sync_status = "复习结果待上传";
    state->problems[state->selected_problem].status = review.selected_status;
    const esp_err_t cache_result = wqn::SaveProblems(state->problems);
    if (cache_result != ESP_OK) {
        ESP_LOGW(kTag, "save reviewed problem cache failed: %s", esp_err_to_name(cache_result));
    }
    state->screen = wqn::UiScreen::kReviewQueued;
    ESP_LOGI(
        kTag,
        "queued review result: problem_id=%s status=%s pending=%d",
        problem.id.c_str(),
        review.selected_status.c_str(),
        state->status.pending_reviews);
    wqn::NotifyOnlineSyncRequested();
    return RefreshSchedule::kCommit;
}

bool SameTimeAppState(const wqn::TimeAppState& a, const wqn::TimeAppState& b)
{
    return a.tile == b.tile && a.active_mode == b.active_mode && a.status == b.status &&
           a.pomodoro_phase == b.pomodoro_phase && a.config_mode == b.config_mode &&
           a.is_editing == b.is_editing && a.active_field == b.active_field &&
           a.remaining_seconds == b.remaining_seconds && a.countdown_hours == b.countdown_hours &&
           a.countdown_minutes == b.countdown_minutes && a.countdown_seconds == b.countdown_seconds &&
           a.countdown_total_seconds == b.countdown_total_seconds && a.pomodoro_rounds == b.pomodoro_rounds &&
           a.pomodoro_focus_minutes == b.pomodoro_focus_minutes &&
           a.pomodoro_break_minutes == b.pomodoro_break_minutes &&
           a.pomodoro_long_break_minutes == b.pomodoro_long_break_minutes &&
           a.pomodoro_current_round == b.pomodoro_current_round;
}

bool ShouldRefreshTimeTick(const wqn::UiState& state)
{
    if (state.screen != wqn::UiScreen::kHome && state.screen != wqn::UiScreen::kTime) {
        return false;
    }
    return wqn::TimeAppHasActiveTimer(state.time_app);
}

bool ScreenUsesClockMinute(const wqn::UiState& state)
{
    if (state.screen == wqn::UiScreen::kHome) {
        return !wqn::TimeAppHasActiveTimer(state.time_app);
    }
    if (state.screen == wqn::UiScreen::kTodo) {
        return true;
    }
    return state.screen == wqn::UiScreen::kTime && state.time_app.tile == wqn::TimeTile::kClock &&
           !state.time_app.config_mode;
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
    }
}

RefreshSchedule ApplySettingsButtonEvent(const wqn::ButtonEvent& event, wqn::UiState* state)
{
    if (state == nullptr || state->screen != wqn::UiScreen::kSettings || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }

    const bool short_press = event.type == wqn::ButtonEventType::kShortPress;
    const bool long_press = event.type == wqn::ButtonEventType::kLongPress;
    const bool repeated_long_press = long_press && event.duration_ms >= kRepeatedLongPressMinDurationMs;
    if (repeated_long_press) {
        return RefreshSchedule::kNone;
    }

    if (event.type == wqn::ButtonEventType::kLongRelease) {
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kAutoSync) {
        if (short_press && event.button == wqn::ButtonId::kUp) {
            if (state->settings.auto_sync_selected == 0) {
                return RefreshSchedule::kNone;
            }
            --state->settings.auto_sync_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kDownPower) {
            if (state->settings.auto_sync_selected + 1 >= sizeof(kAutoSyncOptions) / sizeof(kAutoSyncOptions[0])) {
                return RefreshSchedule::kNone;
            }
            ++state->settings.auto_sync_selected;
            return RefreshSchedule::kConfig;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            const uint32_t minutes = kAutoSyncOptions[state->settings.auto_sync_selected];
            const esp_err_t result = wqn::SaveAutoSyncIntervalMinutes(minutes);
            if (result == ESP_OK) {
                state->settings.auto_sync_interval_min = minutes;
                state->settings.notice = "自动同步已保存：" + wqn::AutoSyncIntervalLabel(minutes);
                wqn::RequestOnlineSyncNow();
            } else {
                state->settings.notice = "自动同步保存失败";
                ESP_LOGW(kTag, "save auto sync interval failed: %s", esp_err_to_name(result));
            }
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kCommit;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kBattery ||
        state->settings.dialog == wqn::SettingsDialog::kStorage) {
        if (event.button == wqn::ButtonId::kConfirm && (short_press || long_press)) {
            state->settings.dialog = wqn::SettingsDialog::kNone;
            return RefreshSchedule::kCommit;
        }
        return RefreshSchedule::kNone;
    }

    if (state->settings.dialog == wqn::SettingsDialog::kFactoryReset) {
        if (long_press && event.button == wqn::ButtonId::kConfirm) {
            state->settings.notice = "正在恢复出厂";
            ESP_LOGW(kTag, "factory reset requested from settings page");
            const esp_err_t reset_result = wqn::FactoryResetNvsAndRestart();
            state->settings.notice = "恢复失败";
            ESP_LOGE(kTag, "factory reset failed: %s", esp_err_to_name(reset_result));
            return RefreshSchedule::kCommit;
        }
        if (short_press && event.button == wqn::ButtonId::kConfirm) {
            state->settings.dialog = wqn::SettingsDialog::kNone;
            state->settings.notice = "已取消恢复出厂";
            return RefreshSchedule::kCommit;
        }
        return RefreshSchedule::kNone;
    }

    if (long_press && event.button == wqn::ButtonId::kConfirm) {
        state->screen = wqn::UiScreen::kHome;
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }
    if (long_press && event.button == wqn::ButtonId::kUp) {
        wqn::HandleUiInput(state, wqn::UiInput::kTopPrevious);
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }
    if (long_press && event.button == wqn::ButtonId::kDownPower) {
        wqn::HandleUiInput(state, wqn::UiInput::kTopNext);
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }

    if (!short_press) {
        return RefreshSchedule::kNone;
    }

    if (event.button == wqn::ButtonId::kUp) {
        if (state->settings.selected == 0) {
            return RefreshSchedule::kNone;
        }
        --state->settings.selected;
        return RefreshSchedule::kCommit;
    }
    if (event.button == wqn::ButtonId::kDownPower) {
        if (state->settings.selected + 1 >= kSettingsItemCount) {
            return RefreshSchedule::kNone;
        }
        ++state->settings.selected;
        return RefreshSchedule::kCommit;
    }
    if (event.button != wqn::ButtonId::kConfirm) {
        return RefreshSchedule::kNone;
    }

    switch (state->settings.selected) {
        case 0:
            wqn::RequestOnlineSyncNow();
            state->settings.sync_status = "已请求同步";
            state->settings.notice = "已请求同步";
            return RefreshSchedule::kCommit;
        case 1:
            OpenSettingsDialog(state, wqn::SettingsDialog::kAutoSync);
            return RefreshSchedule::kCommit;
        case 2:
            OpenSettingsDialog(state, wqn::SettingsDialog::kBattery);
            return RefreshSchedule::kCommit;
        case 3:
            OpenSettingsDialog(state, wqn::SettingsDialog::kStorage);
            return RefreshSchedule::kCommit;
        case 4:
            UpdateSettingsDiagnostics(state);
            state->settings.notice = "固件 " + state->settings.diagnostics.firmware_version;
            return RefreshSchedule::kCommit;
        case 5:
            OpenSettingsDialog(state, wqn::SettingsDialog::kFactoryReset);
            return RefreshSchedule::kCommit;
        default:
            return RefreshSchedule::kNone;
    }
}

RefreshSchedule ApplyButtonEvent(const wqn::ButtonEvent& event, wqn::UiState* state)
{
    if (state == nullptr || !event.HasEvent()) {
        return RefreshSchedule::kNone;
    }

    const bool long_press = event.type == wqn::ButtonEventType::kLongPress;
    const bool long_release = event.type == wqn::ButtonEventType::kLongRelease;
    const bool repeated_long_press = long_press && event.duration_ms >= kRepeatedLongPressMinDurationMs;
    const bool time_value_edit_repeat =
        repeated_long_press && state->screen == wqn::UiScreen::kTime &&
        wqn::TimeAppIsEditingValue(state->time_app) &&
        (event.button == wqn::ButtonId::kUp || event.button == wqn::ButtonId::kDownPower);
    const bool time_running_exit =
        long_press && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kTime &&
        wqn::TimeAppHasActiveTimer(state->time_app);
    if (state->screen == wqn::UiScreen::kSettings) {
        return ApplySettingsButtonEvent(event, state);
    }
    if (repeated_long_press && !time_value_edit_repeat && !time_running_exit) {
        return RefreshSchedule::kNone;
    }
    if (long_release && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kAi) {
        if (state->ai.status == wqn::AiSessionStatus::kListening ||
            state->ai.status == wqn::AiSessionStatus::kWaitingReply) {
#if CONFIG_WQN_AI_ENABLE
            const esp_err_t ret = wqn::StopAiRecordingAndSubmit();
            wqn::AiSessionState ai_state;
            if (wqn::CopyAiSessionToUi(&ai_state)) {
                state->ai = ai_state;
            }
            if (ret != ESP_OK) {
                ESP_LOGW(kTag, "AI recording stop failed: %s", esp_err_to_name(ret));
                state->ai.status = wqn::AiSessionStatus::kError;
                state->ai.assistant_text = "AI 录音停止失败";
                state->ai.pending_text.clear();
                state->ai.status_since_ms = esp_timer_get_time() / 1000;
            }
            return RefreshSchedule::kAi;
#else
            state->ai.status = wqn::AiSessionStatus::kWaitingReply;
            state->ai.status_since_ms = esp_timer_get_time() / 1000;
            if (state->ai.pending_text.empty()) {
                state->ai.pending_text = "AI 功能未启用";
            }
            return RefreshSchedule::kAi;
#endif
        }
        return RefreshSchedule::kNone;
    }
    if (long_release) {
        return RefreshSchedule::kNone;
    }

    const wqn::UiScreen old_screen = state->screen;
    const size_t old_home_task = state->selected_home_task;
    const size_t old_problem = state->selected_problem;
    const size_t old_todo = state->todo.selected;
    const wqn::ReviewChoice old_review = state->selected_review;
    const wqn::TimeAppState old_time_app = state->time_app;
    const std::string old_word_signature = wqn::WordAppSignature(state->word_app);
    ESP_LOGI(
        kTag,
        "button event: id=%d type=%d duration_ms=%lld",
        static_cast<int>(event.button),
        static_cast<int>(event.type),
        static_cast<long long>(event.duration_ms));
    if (!long_press && !long_release && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kReviewScore) {
        return QueueSelectedReview(state);
    }
    if (!long_press && !long_release && event.button == wqn::ButtonId::kConfirm && state->screen == wqn::UiScreen::kTodo) {
        return CompleteSelectedTodo(state);
    }
    if (!long_press && !long_release && state->screen == wqn::UiScreen::kTodo && event.button == wqn::ButtonId::kUp &&
        state->todo.selected == 0 && state->todo.has_earlier && !state->todo.previous_cursor.empty()) {
        if (QueueTodoRefreshCursor(state->todo.previous_cursor)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
            state->todo.status_message = "Todo syncing";
            return RefreshSchedule::kSelection;
        }
    }
    if (!long_press && !long_release && state->screen == wqn::UiScreen::kTodo && event.button == wqn::ButtonId::kDownPower &&
        !state->todo.todos.empty() && state->todo.selected + 1 >= state->todo.todos.size() &&
        state->todo.has_later && !state->todo.next_cursor.empty()) {
        if (QueueTodoRefreshCursor(state->todo.next_cursor)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
            state->todo.status_message = "Todo syncing";
            return RefreshSchedule::kSelection;
        }
    }

    switch (event.button) {
        case wqn::ButtonId::kUp:
            if (long_press && state->screen == wqn::UiScreen::kTime && wqn::TimeAppIsEditingValue(state->time_app)) {
                wqn::HandleTimeAppInput(&state->time_app, wqn::TimeInput::kLongUp);
            } else {
                wqn::HandleUiInput(state, long_press ? wqn::UiInput::kTopPrevious : wqn::UiInput::kUp);
            }
            break;
        case wqn::ButtonId::kDownPower:
            if (long_press && state->screen == wqn::UiScreen::kTime && wqn::TimeAppIsEditingValue(state->time_app)) {
                wqn::HandleTimeAppInput(&state->time_app, wqn::TimeInput::kLongDown);
            } else {
                wqn::HandleUiInput(state, long_press ? wqn::UiInput::kTopNext : wqn::UiInput::kDown);
            }
            break;
        case wqn::ButtonId::kConfirm:
            if (time_running_exit) {
                wqn::HandleTimeAppInput(&state->time_app, wqn::TimeInput::kLongConfirm);
            } else {
                wqn::HandleUiInput(state, long_press ? wqn::UiInput::kLongConfirm : wqn::UiInput::kConfirm);
            }
            break;
        case wqn::ButtonId::kNone:
            return RefreshSchedule::kNone;
    }

    if (state->screen != old_screen) {
        if (state->screen == wqn::UiScreen::kTodo) {
            RefreshTodosFromCloud(state);
        } else if (state->screen == wqn::UiScreen::kWord && state->word_app.cloud_sync_requested) {
            if (!QueueWordReviewRefresh()) {
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "单词同步失败";
            } else {
                state->word_app.message = "单词同步中";
            }
        }
        BuildHomeSummary(state);
        return RefreshSchedule::kCommit;
    }
    if (state->screen == wqn::UiScreen::kAi) {
        return RefreshSchedule::kAi;
    }
    if (!SameTimeAppState(state->time_app, old_time_app)) {
        BuildHomeSummary(state);
        if (state->time_app.config_mode && old_time_app.config_mode) {
            return RefreshSchedule::kConfig;
        }
        return RefreshSchedule::kCommit;
    }
    if (state->screen == wqn::UiScreen::kWord &&
        wqn::WordAppSignature(state->word_app) != old_word_signature) {
        wqn::WqnWordReviewSubmission submission;
        std::string word;
        if (wqn::TakeWordReviewSubmission(&state->word_app, &submission, &word)) {
            if (!QueueWordReviewSubmit(submission, word)) {
                state->word_app.pending_submit_word_id = submission.word_id;
                state->word_app.pending_submit_outcome = submission.outcome;
                state->word_app.pending_submit_word = word;
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "单词同步失败";
            }
        }
        wqn::WqnWordSearchRequest search_request;
        if (wqn::TakeWordSearchRequest(&state->word_app, &search_request)) {
            if (!QueueWordSearch(search_request)) {
                state->word_app.search_pending = true;
                state->word_app.pending_search_query = search_request.query.empty() ? search_request.prefix : search_request.query;
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "在线搜索失败";
            }
        }
        wqn::WqnWordAiLookupRequest lookup_request;
        if (wqn::TakeWordAiLookupRequest(&state->word_app, &lookup_request)) {
            if (!QueueWordAiLookup(lookup_request)) {
                state->word_app.ai_lookup_pending = true;
                state->word_app.pending_ai_query = lookup_request.query.empty() ? lookup_request.prefix : lookup_request.query;
                state->word_app.message = IsWordCloudBusy() ? "单词同步中" : "AI 查词失败";
            }
        }
        BuildHomeSummary(state);
        return RefreshSchedule::kSelection;
    }
    if (state->selected_home_task != old_home_task ||
        state->selected_problem != old_problem ||
        state->todo.selected != old_todo ||
        state->selected_review != old_review) {
        return RefreshSchedule::kSelection;
    }
    return RefreshSchedule::kNone;
}

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
        signature.append(std::to_string(diag.adc_raw));
        signature.push_back('/');
        signature.append(std::to_string(diag.adc_mv));
        signature.push_back('/');
        signature.append(std::to_string(diag.battery_mv));
        signature.push_back('/');
        signature.append(std::to_string(diag.battery_percent));
        signature.push_back('/');
        signature.append(diag.charging ? "charging" : "not-charging");
        signature.push_back('/');
        signature.append(diag.full ? "full" : "not-full");
        signature.push_back('/');
        signature.append(std::to_string(diag.flash_size));
        signature.push_back('/');
        signature.append(std::to_string(diag.nvs_used_entries));
        signature.push_back('/');
        signature.append(std::to_string(diag.nvs_free_entries));
        signature.push_back('/');
        signature.append(std::to_string(diag.nvs_total_entries));
        signature.push_back('/');
        signature.append(std::to_string(diag.psram_total));
        signature.push_back('/');
        signature.append(std::to_string(diag.psram_free));
        signature.push_back('/');
        signature.append(diag.firmware_version);
        signature.push_back('/');
        signature.append(CurrentClockLabel());
        signature.push_back('/');
        signature.append(frame.home.wifi_label);
        signature.push_back('/');
        signature.append(frame.home.battery_label);
    }
    for (const wqn::UiLine& line : frame.lines) {
        signature.push_back('|');
        signature.append(std::to_string(static_cast<int>(line.style)));
        signature.push_back(':');
        signature.append(line.text);
    }
    return signature;
}

esp_err_t RenderFrameToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    if (schedule == RefreshSchedule::kClock) {
        if (frame.screen == wqn::UiScreen::kHome) {
            return RenderHomePrimaryRegion(frame.home, schedule);
        }
        if (frame.screen == wqn::UiScreen::kTime && frame.time_app.tile == wqn::TimeTile::kClock &&
            !frame.time_app.config_mode) {
            return RenderTimeClockRegion(schedule, false);
        }
    }

    if (schedule == RefreshSchedule::kTimer) {
        if (frame.screen == wqn::UiScreen::kHome) {
            return RenderHomePrimaryRegion(frame.home, schedule);
        }
        if (frame.screen == wqn::UiScreen::kTime && !frame.time_app.config_mode &&
            frame.time_app.tile != wqn::TimeTile::kClock) {
            return RenderTimerRunRegion(frame.time_app, schedule);
        }
    }

    if (schedule == RefreshSchedule::kConfig && frame.screen == wqn::UiScreen::kTime && frame.time_app.config_mode) {
        return RenderTimeConfigRegion(frame.time_app, schedule);
    }

    if (frame.screen == wqn::UiScreen::kHome) {
        return RenderHomeToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kTime) {
        return RenderTimeToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kAi) {
        return RenderAiToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kTodo) {
        return RenderTodoToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kWord) {
        return RenderWordToEpd(frame, schedule);
    }
    if (frame.screen == wqn::UiScreen::kSettings) {
        return RenderSettingsToEpd(frame, schedule);
    }

    wqn::ClearEpdFramebuffer(true);

    int y = 6;
    for (const wqn::UiLine& line : frame.lines) {
        if (y > wqn::kEpdHeight - 12) {
            break;
        }

        const bool selected = line.style == wqn::UiTextStyle::kSelected;
        const int x = selected ? 6 : 0;
        if (line.style == wqn::UiTextStyle::kWrappedBody) {
            const std::vector<std::string> wrapped =
                wqn::WrapUtf8TextToWidth(line.text, kEpdTextWidth - x, kWrappedBodyMaxLines);
            for (const std::string& wrapped_line : wrapped) {
                if (y > wqn::kEpdHeight - 12) {
                    break;
                }
                ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, y, wrapped_line.c_str(), true), kTag, "draw UI wrapped line");
                y += 18;
            }
        } else {
            const std::string text = LimitForEpd(line.text);
            ESP_RETURN_ON_ERROR(wqn::DrawUtf8Text(x, y, text.c_str(), true), kTag, "draw UI line");
            y += 18;
        }
    }

    return RefreshFrame(schedule);
}

bool RequestEpdUiRefresh(const wqn::UiFrame& frame, const std::string& signature, RefreshSchedule schedule)
{
    if (g_refresh_mutex == nullptr || g_refresh_task == nullptr || schedule == RefreshSchedule::kNone) {
        return false;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t due_tick = now + RefreshDelay(schedule);

    xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
    if (g_refresh_busy &&
        (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig ||
         schedule == RefreshSchedule::kClock || schedule == RefreshSchedule::kTimer ||
         schedule == RefreshSchedule::kAi)) {
        if (g_refresh_pending && RefreshRank(schedule) < RefreshRank(g_refresh_schedule)) {
            xSemaphoreGive(g_refresh_mutex);
            return false;
        }
        g_pending_frame = frame;
        g_pending_signature = signature;
        g_refresh_pending = true;
        g_refresh_due_tick = xTaskGetTickCount() + RefreshDelay(schedule);
        g_refresh_schedule = schedule;
        xSemaphoreGive(g_refresh_mutex);
        return true;
    }
    g_pending_frame = frame;
    g_pending_signature = signature;
    g_refresh_pending = true;
    if (schedule == RefreshSchedule::kConfig) {
        g_refresh_due_tick = due_tick;
        g_refresh_schedule = schedule;
    } else if (g_refresh_schedule == RefreshSchedule::kNone ||
        RefreshRank(schedule) >= RefreshRank(g_refresh_schedule) ||
        TickBefore(due_tick, g_refresh_due_tick)) {
        g_refresh_due_tick = due_tick;
        g_refresh_schedule = StrongerSchedule(schedule, g_refresh_schedule);
    }
    xSemaphoreGive(g_refresh_mutex);

    xTaskNotifyGive(g_refresh_task);
    return true;
}

void EpdRefreshTask(void*)
{
    ESP_LOGI(kTag, "EPD refresh task started");

    std::string displayed_signature;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

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

        wqn::UiFrame frame;
        std::string signature;
        RefreshSchedule schedule = RefreshSchedule::kNone;
        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        if (!g_refresh_pending) {
            xSemaphoreGive(g_refresh_mutex);
            continue;
        }
        frame = g_pending_frame;
        signature = g_pending_signature;
        schedule = g_refresh_schedule;
        g_refresh_pending = false;
        g_refresh_schedule = RefreshSchedule::kNone;
        g_refresh_due_tick = 0;
        g_refresh_busy = true;
        xSemaphoreGive(g_refresh_mutex);

        if (signature == displayed_signature) {
            xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
            g_refresh_busy = false;
            xSemaphoreGive(g_refresh_mutex);
            continue;
        }

        ESP_LOGI(kTag, "EPD UI refresh: schedule=%s", RefreshScheduleName(schedule));
        const int64_t refresh_start_us = esp_timer_get_time();
        const esp_err_t result = RenderFrameToEpd(frame, schedule);
        const int64_t refresh_elapsed_ms = (esp_timer_get_time() - refresh_start_us) / 1000;
        if (result == ESP_OK) {
            displayed_signature = signature;
            wqn::NoteEpdActivity();
            ESP_LOGI(kTag, "EPD UI refresh done: schedule=%s elapsed_ms=%lld", RefreshScheduleName(schedule), static_cast<long long>(refresh_elapsed_ms));
        } else {
            ESP_LOGW(kTag, "EPD UI render failed: %s", esp_err_to_name(result));
        }

        xSemaphoreTake(g_refresh_mutex, portMAX_DELAY);
        const bool has_more = g_refresh_pending && g_pending_signature != displayed_signature;
        g_refresh_busy = false;
        xSemaphoreGive(g_refresh_mutex);
        if (has_more) {
            xTaskNotifyGive(g_refresh_task);
        }
    }
}

void DeviceUiTask(void*)
{
    ESP_LOGI(kTag, "device UI task started");
    wqn::NoteUserActivity();
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

    wqn::UiState state;
    LoadUiState(&state);
    CheckBatteryProtection();
    std::string last_clock_label = CurrentClockLabel();
    std::string last_frame_signature;
    {
        const wqn::UiFrame frame = wqn::RenderUiFrame(state);
        last_frame_signature = FrameSignature(frame);
        RequestEpdUiRefresh(frame, last_frame_signature, RefreshSchedule::kImmediate);
    }
    TickType_t last_status_refresh = xTaskGetTickCount();

    while (true) {
        RefreshSchedule refresh_schedule = RefreshSchedule::kNone;
        const wqn::ButtonEvent event = wqn::PollButtonInput();
        if (event.HasEvent()) {
            wqn::NoteUserActivity();
        }
        refresh_schedule = StrongerSchedule(refresh_schedule, ApplyButtonEvent(event, &state));

        if (g_todo_result_queue != nullptr) {
            TodoCloudResult* todo_result = nullptr;
            while (xQueueReceive(g_todo_result_queue, &todo_result, 0) == pdTRUE) {
                g_todo_cloud_busy = false;
                if (todo_result != nullptr) {
                    if (ApplyTodoCloudResult(&state, *todo_result) && state.screen == wqn::UiScreen::kTodo) {
                        refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kCommit);
                    }
                    delete todo_result;
                }
            }
        }
        if (g_word_result_queue != nullptr) {
            WordCloudResult* word_result = nullptr;
            while (xQueueReceive(g_word_result_queue, &word_result, 0) == pdTRUE) {
                g_word_cloud_busy = false;
                if (word_result != nullptr) {
                    if (ApplyWordCloudResult(&state, *word_result) && state.screen == wqn::UiScreen::kWord) {
                        refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kCommit);
                    }
                    delete word_result;
                }
            }
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (wqn::TickTimeApp(&state.time_app, now_ms)) {
            UpdateHomePrimaryTimeLine(&state);
            if (ShouldRefreshTimeTick(state)) {
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kTimer);
            }
        }

        if (wqn::TickAiSession(&state, now_ms) && state.screen == wqn::UiScreen::kAi) {
            refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
        }

#if CONFIG_WQN_AI_ENABLE
        if (wqn::CopyAiSessionToUi(&state.ai) && state.screen == wqn::UiScreen::kAi) {
            refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kAi);
        }
#endif

        const std::string clock_label = CurrentClockLabel();
        if (clock_label != last_clock_label) {
            last_clock_label = clock_label;
            UpdateHomePrimaryTimeLine(&state);
            if (ScreenUsesClockMinute(state)) {
                refresh_schedule = StrongerSchedule(refresh_schedule, RefreshSchedule::kClock);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_status_refresh >= kStatusRefreshDelay) {
            const std::string before_signature = FrameSignature(wqn::RenderUiFrame(state));
            LoadUiState(&state);
            CheckBatteryProtection();
            const std::string after_signature = FrameSignature(wqn::RenderUiFrame(state));
            if (after_signature != before_signature && refresh_schedule == RefreshSchedule::kNone) {
                refresh_schedule = RefreshSchedule::kSelection;
            }
            last_status_refresh = now;
        }

        if (refresh_schedule != RefreshSchedule::kNone) {
            const wqn::UiFrame frame = wqn::RenderUiFrame(state);
            const std::string frame_signature = FrameSignature(frame);
            if (frame_signature != last_frame_signature) {
                if (RequestEpdUiRefresh(frame, frame_signature, refresh_schedule)) {
                    ESP_LOGI(kTag, "EPD UI refresh requested: schedule=%s", RefreshScheduleName(refresh_schedule));
                    last_frame_signature = frame_signature;
                }
            }
        }

        wqn::PowerOffEpdAfterIdleIfNeeded();
        wqn::EnterDeepSleepIfEnabled();
        vTaskDelay(kUiPollDelay);
    }
}

#endif  // CONFIG_WQN_EPD_UI_ENABLE

}  // namespace

namespace wqn {

esp_err_t StartDeviceUiIfEnabled()
{
#if CONFIG_WQN_EPD_UI_ENABLE
    if (g_refresh_mutex == nullptr) {
        g_refresh_mutex = xSemaphoreCreateMutex();
        if (g_refresh_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_todo_request_queue == nullptr) {
        g_todo_request_queue = xQueueCreate(2, sizeof(TodoCloudRequest));
        if (g_todo_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_todo_result_queue == nullptr) {
        g_todo_result_queue = xQueueCreate(2, sizeof(TodoCloudResult*));
        if (g_todo_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_word_request_queue == nullptr) {
        g_word_request_queue = xQueueCreate(3, sizeof(WordCloudRequest));
        if (g_word_request_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_word_result_queue == nullptr) {
        g_word_result_queue = xQueueCreate(3, sizeof(WordCloudResult*));
        if (g_word_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_refresh_task == nullptr) {
        const BaseType_t refresh_created =
            xTaskCreate(EpdRefreshTask, "wqn_epd_refresh", 8192, nullptr, 1, &g_refresh_task);
        if (refresh_created != pdPASS) {
            g_refresh_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_todo_task == nullptr) {
        const BaseType_t todo_created =
            xTaskCreate(TodoCloudTask, "wqn_todo_cloud", 8192, nullptr, 3, &g_todo_task);
        if (todo_created != pdPASS) {
            g_todo_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_word_task == nullptr) {
        const BaseType_t word_created =
            xTaskCreate(WordCloudTask, "wqn_word_cloud", 8192, nullptr, 3, &g_word_task);
        if (word_created != pdPASS) {
            g_word_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    const BaseType_t created = xTaskCreate(DeviceUiTask, "wqn_ui", 8192, nullptr, 4, nullptr);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    ESP_LOGI(kTag, "EPD device UI disabled by CONFIG_WQN_EPD_UI_ENABLE");
    return ESP_OK;
#endif
}

}  // namespace wqn
