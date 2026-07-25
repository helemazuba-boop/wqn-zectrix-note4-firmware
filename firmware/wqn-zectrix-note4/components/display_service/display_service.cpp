#include "display_service.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_crc.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "font_fmt.h"
#include "runtime/wake_context.h"
#include "sdkconfig.h"

#ifndef CONFIG_WQN_EPD_IDLE_POWER_OFF_MS
#define CONFIG_WQN_EPD_IDLE_POWER_OFF_MS 1500
#endif

namespace wqn {

static void PowerOffEpd();

namespace {

constexpr char kTag[] = "wqn_epd";

constexpr gpio_num_t kEpdPower = GPIO_NUM_6;
constexpr gpio_num_t kEpdBusy = GPIO_NUM_8;
constexpr gpio_num_t kEpdReset = GPIO_NUM_9;
constexpr gpio_num_t kEpdDc = GPIO_NUM_10;
constexpr gpio_num_t kEpdCs = GPIO_NUM_11;
constexpr gpio_num_t kEpdSck = GPIO_NUM_12;
constexpr gpio_num_t kEpdMosi = GPIO_NUM_13;
constexpr spi_host_device_t kEpdSpiHost = SPI3_HOST;

constexpr int kSpiClockHz = 40 * 1000 * 1000;
constexpr int kBusyTimeoutMs = 30000;
// Partial-waveform completion wait. Detection threshold for a wedged panel:
// across every HIL session healthy partial waveforms complete in <=800 ms and
// wedged ones never complete -- nothing has ever finished in the 1.5-4 s band
// (the earlier "stretched waveform" reading was wrong), so a longer wait only
// lengthens the stall before the automatic full-refresh recovery kicks in.
constexpr int kPartialRefreshBusyTimeoutMs = 1500;
constexpr int kPartialCommandBusyTimeoutMs = 1500;
constexpr int kLocalPartialMaxHeight = 170;
// [epd-health] SSDs (SSD1683 / WaveShare 4.2") accumulate DC bias after
// consecutive partial refreshes; without interleaving full refreshes the
// panel's BUSY line eventually sticks high past kPartialCommandBusyTimeoutMs
// and the only recovery is a full refresh (~2-3s), which the user feels as
// a hard freeze. Forcing a full refresh every N partials is far less
// disruptive than letting the panel stall in the middle of a UI flow.
// [epd-tune] Bumped from 6 to 20 after observing SSD1683 stayed clean
// through 10+ consecutive partial refreshes (UI smoke tests with a 10s
// countdown). The timer-page once-per-second tick used to hit the old 6
// threshold every 7s, producing a visible flash in the middle of a running
// timer. 20 covers a 21-second timer run window without forcing a full
// refresh, and a 6-min countdown still gets an interleaved full refresh
// every 21 partials.
constexpr uint32_t kMaxPartialRefreshesBeforeFull = 20;
// Heavy partials (large diffs: list-row highlight flips ~17%, body scrolls
// ~5-6%) stress the panel far more than a timer's once-per-second tick
// (~0.02%). Field logs show the panel drifting from the framebuffer (stale
// highlight rows on screen while the framebuffer had moved on) after ~11
// consecutive heavy partials. Routine cleanup happens at idle (see
// PowerOffEpdAfterIdleIfNeeded) so scrolling never eats a 1.2 s flash
// mid-flow; this cap is only the in-flow safety net just under the observed
// drift onset for uninterrupted 10+ step scrolls.
constexpr float kHeavyPartialDiffRatio = 0.02f;
constexpr uint32_t kMaxHeavyPartialsBeforeFull = 10;
// Run the deferred cleanup full refresh at idle once at least this many heavy
// partials accumulated since the last full.
constexpr uint32_t kIdleCleanupHeavyPartials = 2;
constexpr int kTextGlyphWidth = 5;
constexpr int kTextGlyphHeight = 7;
constexpr int kTextCellWidth = 6;
constexpr int kTextLineHeight = 9;
constexpr int kCjkLineHeight = 18;
constexpr int kCjkFallbackWidth = 16;
constexpr int kCjkFontHeight = 16;
constexpr int kCjkFontBaseLine = 2;

// [L3-baseline] Vertical offset for 5x7 ASCII glyphs in DrawUtf8Text so the
// ASCII baseline aligns with the CJK baseline. CJK baseline sits at
// y + (kCjkFontHeight - kCjkFontBaseLine) = y + 14. This 5x7 font has no
// descenders ('A' legs and 'g'/'p'/'y' tails all end on row 6 = the baseline),
// so ASCII baseline = glyph top + (kTextGlyphHeight - 1) = top + 6, and the
// top must be at y + 14 - 6 = 8. Replaces the hard-coded +5 that left ASCII
// ~3px above the CJK baseline in mixed lines like "WiFi 已连接".
constexpr int kAsciiBaselineOffset = (kCjkFontHeight - kCjkFontBaseLine) - (kTextGlyphHeight - 1);  // = 8

spi_device_handle_t g_spi = nullptr;
uint8_t* g_framebuffer = nullptr;
uint8_t* g_previous_framebuffer = nullptr;
bool g_bus_initialized = false;
bool g_initialized = false;
bool g_epd_rail_powered = false;
bool g_epd_powered = false;
bool g_previous_framebuffer_synced = false;
bool g_hot_refresh_ok = false;
uint32_t g_partial_refreshes_since_full = 0;
uint32_t g_heavy_partials_since_full = 0;
// [epd-wedge-fix] True when the most recent successful refresh was a
// full-frame partial. HIL logs show a windowed local partial issued right
// after a full-frame partial wedges the SSD1683: BUSY stays low past the 4 s
// probe (entry=0 exit=0 trans=no) and only a full-refresh recovery revives the
// panel (three-for-three reproduction in one session, while LP->LP, FFP->FFP
// and FULL->LP sequences all run clean).
bool g_last_partial_was_full_frame = false;
int64_t g_last_epd_refresh_us = 0;
int64_t g_last_epd_activity_ms = 0;
bool g_epd_idle_cut = false;

// [power-fix] Persisted across deep-sleep resets so the EPD refresh task
// can skip redundant panel updates after an RTC-timer wakeup.  Without this,
// RAM state is lost on every deep sleep and the driver always forces a full
// refresh, causing visible flicker on every clock tick.
RTC_DATA_ATTR uint32_t g_rtc_last_frame_crc = 0;
RTC_DATA_ATTR bool g_rtc_last_frame_crc_valid = false;

SemaphoreHandle_t EpdOperationMutex()
{
    // Recursive because a full refresh powers the panel off from inside the
    // already-serialized refresh operation.
    static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutex();
    return mutex;
}

class EpdOperationGuard {
public:
    explicit EpdOperationGuard(TickType_t timeout)
        : mutex_(EpdOperationMutex())
    {
        locked_ = mutex_ != nullptr && xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
    }

    ~EpdOperationGuard()
    {
        if (locked_) {
            xSemaphoreGiveRecursive(mutex_);
        }
    }

    EpdOperationGuard(const EpdOperationGuard&) = delete;
    EpdOperationGuard& operator=(const EpdOperationGuard&) = delete;

    bool locked() const { return locked_; }

private:
    SemaphoreHandle_t mutex_ = nullptr;
    bool locked_ = false;
};

extern "C" const lv_font_t SourceHanSansSC_Regular_slim;

extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t)
{
    return false;
}

extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*, uint32_t)
{
    return nullptr;
}

const uint8_t kFont5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // space
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // !
    {0x00, 0x07, 0x00, 0x07, 0x00},  // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // $
    {0x23, 0x13, 0x08, 0x64, 0x62},  // %
    {0x36, 0x49, 0x55, 0x22, 0x50},  // &
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // (
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // )
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // *
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // +
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ,
    {0x08, 0x08, 0x08, 0x08, 0x08},  // -
    {0x00, 0x60, 0x60, 0x00, 0x00},  // .
    {0x20, 0x10, 0x08, 0x04, 0x02},  // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
    {0x00, 0x36, 0x36, 0x00, 0x00},  // :
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ;
    {0x08, 0x14, 0x22, 0x41, 0x00},  // <
    {0x14, 0x14, 0x14, 0x14, 0x14},  // =
    {0x00, 0x41, 0x22, 0x14, 0x08},  // >
    {0x02, 0x01, 0x51, 0x09, 0x06},  // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
    {0x46, 0x49, 0x49, 0x49, 0x31},  // S
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // W
    {0x63, 0x14, 0x08, 0x14, 0x63},  // X
    {0x07, 0x08, 0x70, 0x08, 0x07},  // Y
    {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
    {0x00, 0x7F, 0x41, 0x41, 0x00},  // [
    {0x02, 0x04, 0x08, 0x10, 0x20},  // backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00},  // ]
    {0x04, 0x02, 0x01, 0x02, 0x04},  // ^
    {0x40, 0x40, 0x40, 0x40, 0x40},  // _
    {0x00, 0x01, 0x02, 0x04, 0x00},  // `
    {0x20, 0x54, 0x54, 0x54, 0x78},  // a
    {0x7F, 0x48, 0x44, 0x44, 0x38},  // b
    {0x38, 0x44, 0x44, 0x44, 0x20},  // c
    {0x38, 0x44, 0x44, 0x48, 0x7F},  // d
    {0x38, 0x54, 0x54, 0x54, 0x18},  // e
    {0x08, 0x7E, 0x09, 0x01, 0x02},  // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E},  // g
    {0x7F, 0x08, 0x04, 0x04, 0x78},  // h
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // i
    {0x20, 0x40, 0x44, 0x3D, 0x00},  // j
    {0x7F, 0x10, 0x28, 0x44, 0x00},  // k
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // l
    {0x7C, 0x04, 0x18, 0x04, 0x78},  // m
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // n
    {0x38, 0x44, 0x44, 0x44, 0x38},  // o
    {0x7C, 0x14, 0x14, 0x14, 0x08},  // p
    {0x08, 0x14, 0x14, 0x18, 0x7C},  // q
    {0x7C, 0x08, 0x04, 0x04, 0x08},  // r
    {0x48, 0x54, 0x54, 0x54, 0x20},  // s
    {0x04, 0x3F, 0x44, 0x40, 0x20},  // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C},  // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C},  // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C},  // w
    {0x44, 0x28, 0x10, 0x28, 0x44},  // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C},  // y
    {0x44, 0x64, 0x54, 0x4C, 0x44},  // z
    {0x00, 0x08, 0x36, 0x41, 0x00},  // {
    {0x00, 0x00, 0x7F, 0x00, 0x00},  // |
    {0x00, 0x41, 0x36, 0x08, 0x00},  // }
    {0x08, 0x04, 0x08, 0x10, 0x08},  // ~
};

void SetCs(bool high)
{
    gpio_set_level(kEpdCs, high ? 1 : 0);
}

void SetDc(bool high)
{
    gpio_set_level(kEpdDc, high ? 1 : 0);
}

void SetReset(bool high)
{
    gpio_set_level(kEpdReset, high ? 1 : 0);
}

esp_err_t WaitBusyTimeout(int timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    const int entry_level = gpio_get_level(kEpdBusy);
    int first_transition_level = entry_level;
    TickType_t transition_tick = 0;
    bool transition_seen = false;
    while (gpio_get_level(kEpdBusy) == 0) {
        const int lvl = gpio_get_level(kEpdBusy);
        if (!transition_seen && lvl != entry_level) {
            transition_seen = true;
            first_transition_level = lvl;
            transition_tick = xTaskGetTickCount();
        }
        if ((xTaskGetTickCount() - start) > timeout) {
            ESP_LOGW(kTag, "[BUSY-PROBE] TIMEOUT to=%dms entry=%d exit=%d trans=%s first_trans_lvl=%d elapsed=%dms",
                     timeout_ms, entry_level, static_cast<int>(gpio_get_level(kEpdBusy)),
                     transition_seen ? "yes" : "no", first_transition_level,
                     static_cast<int>(pdTICKS_TO_MS(xTaskGetTickCount() - start)));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGD(kTag, "[BUSY-PROBE] OK to=%dms entry=%d exit=%d trans=%s first_trans_lvl=%d trans_at=%dms elapsed=%dms",
             timeout_ms, entry_level, static_cast<int>(gpio_get_level(kEpdBusy)),
             transition_seen ? "yes" : "no", first_transition_level,
             transition_seen ? static_cast<int>(pdTICKS_TO_MS(transition_tick - start)) : 0,
             static_cast<int>(pdTICKS_TO_MS(xTaskGetTickCount() - start)));
    return ESP_OK;
}

esp_err_t WaitBusy()
{
    return WaitBusyTimeout(kBusyTimeoutMs);
}

esp_err_t SendByte(uint8_t data)
{
    spi_transaction_t transaction = {};
    transaction.length = 8;
    transaction.tx_buffer = &data;
    return spi_device_polling_transmit(g_spi, &transaction);
}

esp_err_t SendCommand(uint8_t command)
{
    SetDc(false);
    SetCs(false);
    const esp_err_t ret = SendByte(command);
    SetCs(true);
    return ret;
}

esp_err_t SendData(uint8_t data)
{
    SetDc(true);
    SetCs(false);
    const esp_err_t ret = SendByte(data);
    SetCs(true);
    return ret;
}

esp_err_t WriteBytes(const uint8_t* data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    SetDc(true);
    SetCs(false);
    spi_transaction_t transaction = {};
    transaction.length = len * 8;
    transaction.tx_buffer = data;
    const esp_err_t ret = spi_device_polling_transmit(g_spi, &transaction);
    SetCs(true);
    return ret;
}

esp_err_t RecvByte(uint8_t* data)
{
    spi_transaction_t transaction = {};
    transaction.length = 8;
    transaction.rx_buffer = data;
    return spi_device_polling_transmit(g_spi, &transaction);
}

esp_err_t InitSpiBus(bool rx_mode)
{
    if (g_spi != nullptr) {
        ESP_RETURN_ON_ERROR(spi_bus_remove_device(g_spi), kTag, "remove EPD SPI device");
        g_spi = nullptr;
    }
    if (g_bus_initialized) {
        const esp_err_t free_ret = spi_bus_free(kEpdSpiHost);
        if (free_ret != ESP_OK && free_ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(free_ret, kTag, "free EPD SPI bus");
        }
        g_bus_initialized = false;
    }

    spi_bus_config_t bus_config = {};
    bus_config.miso_io_num = rx_mode ? kEpdMosi : -1;
    bus_config.mosi_io_num = rx_mode ? -1 : kEpdMosi;
    bus_config.sclk_io_num = kEpdSck;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz = kEpdFramebufferSize;

    spi_device_interface_config_t device_config = {};
    device_config.spics_io_num = -1;
    device_config.clock_speed_hz = rx_mode ? 8 * 1000 * 1000 : kSpiClockHz;
    device_config.mode = 0;
    device_config.queue_size = 1;

    ESP_RETURN_ON_ERROR(spi_bus_initialize(kEpdSpiHost, &bus_config, SPI_DMA_CH_AUTO), kTag, "init EPD SPI bus");
    g_bus_initialized = true;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(kEpdSpiHost, &device_config, &g_spi), kTag, "add EPD SPI device");
    return ESP_OK;
}

esp_err_t RecvData(uint8_t* data)
{
    ESP_RETURN_ON_ERROR(InitSpiBus(true), kTag, "switch EPD SPI to RX");
    SetDc(true);
    SetCs(false);
    const esp_err_t ret = RecvByte(data);
    SetCs(true);
    const esp_err_t tx_ret = InitSpiBus(false);
    ESP_RETURN_ON_ERROR(ret, kTag, "read EPD data");
    return tx_ret;
}

esp_err_t InitGpio()
{
    gpio_config_t outputs = {};
    outputs.intr_type = GPIO_INTR_DISABLE;
    outputs.mode = GPIO_MODE_OUTPUT;
    outputs.pin_bit_mask = (1ULL << kEpdPower) | (1ULL << kEpdReset) | (1ULL << kEpdDc) | (1ULL << kEpdCs);
    outputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
    outputs.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), kTag, "configure EPD outputs");

    gpio_config_t busy = {};
    busy.intr_type = GPIO_INTR_DISABLE;
    busy.mode = GPIO_MODE_INPUT;
    busy.pin_bit_mask = (1ULL << kEpdBusy);
    busy.pull_down_en = GPIO_PULLDOWN_DISABLE;
    busy.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&busy), kTag, "configure EPD busy with pull-up");

    SetCs(true);
    SetDc(true);
    SetReset(true);
    gpio_set_level(kEpdPower, 0);
    return ESP_OK;
}

uint8_t* AllocateFramebuffer()
{
    uint8_t* fb = static_cast<uint8_t*>(heap_caps_malloc(kEpdFramebufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fb != nullptr) {
        ESP_LOGI(kTag, "allocated EPD framebuffer in PSRAM, size=%u", static_cast<unsigned>(kEpdFramebufferSize));
        return fb;
    }

    ESP_LOGW(
        kTag,
        "PSRAM EPD framebuffer allocation failed, falling back to internal RAM: size=%u free_psram=%u free_internal=%u",
        static_cast<unsigned>(kEpdFramebufferSize),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    fb = static_cast<uint8_t*>(heap_caps_malloc(kEpdFramebufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (fb == nullptr) {
        ESP_LOGE(
            kTag,
            "EPD framebuffer allocation failed: size=%u free_psram=%u free_internal=%u",
            static_cast<unsigned>(kEpdFramebufferSize),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    } else {
        ESP_LOGW(kTag, "allocated EPD framebuffer in internal RAM, size=%u", static_cast<unsigned>(kEpdFramebufferSize));
    }
    return fb;
}

void FreeFramebuffer(uint8_t*& fb)
{
    if (fb != nullptr) {
        heap_caps_free(fb);
        fb = nullptr;
    }
}

void PowerOnEpd()
{
    // [epd-leak-fix] Release hold on SPI pins before driving the rail up.
    // PowerOffEpd() holds kEpd{Sck,Mosi,Cs,Dc,Reset} low to block diode
    // backflow into the de-powered EPD module. That hold survives deep sleep,
    // so on the next refresh we must explicitly release it or the panel
    // remains hardware-reset and SPI never reaches the controller.
    constexpr gpio_num_t kSpiPins[] = {kEpdSck, kEpdMosi, kEpdCs, kEpdDc, kEpdReset};
    for (gpio_num_t pin : kSpiPins) {
        gpio_hold_dis(pin);
    }

    gpio_hold_dis(kEpdPower);
    gpio_set_level(kEpdPower, 1);
    gpio_hold_en(kEpdPower);
    g_epd_rail_powered = true;
}

void DropEpdHotState(bool cut_rail, bool invalidate_framebuffer)
{
    if (cut_rail) {
        gpio_hold_dis(kEpdPower);
        gpio_set_level(kEpdPower, 0);
        gpio_hold_en(kEpdPower);
        g_epd_rail_powered = false;
    }
    g_epd_powered = false;
    if (invalidate_framebuffer) {
        g_previous_framebuffer_synced = false;
        g_partial_refreshes_since_full = 0;
        g_heavy_partials_since_full = 0;
        g_last_partial_was_full_frame = false;
    }
    g_hot_refresh_ok = false;
    g_last_epd_refresh_us = 0;
}

esp_err_t InitPanelSequence()
{
    PowerOnEpd();
    g_epd_powered = false;
    vTaskDelay(pdMS_TO_TICKS(10));
    SetReset(true);
    vTaskDelay(pdMS_TO_TICKS(10));
    SetReset(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetReset(true);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(WaitBusy(), kTag, "wait after EPD reset");

    ESP_RETURN_ON_ERROR(SendCommand(0x00), kTag, "EPD panel setting");
    ESP_RETURN_ON_ERROR(SendData(0x2F), kTag, "EPD panel setting data0");
    ESP_RETURN_ON_ERROR(SendData(0x0E), kTag, "EPD panel setting data1");

    ESP_RETURN_ON_ERROR(SendCommand(0xE9), kTag, "EPD OTP command");
    ESP_RETURN_ON_ERROR(SendData(0x01), kTag, "EPD OTP data");
    return WaitBusy();
}

uint8_t TemperatureToVcom(uint8_t temp)
{
    if (temp <= 5) {
        return 232;
    }
    if (temp <= 10) {
        return 235;
    }
    if (temp <= 20) {
        return 238;
    }
    if (temp <= 30) {
        return 241;
    }
    if (temp <= 127) {
        return 244;
    }
    return 232;
}

void Pack1bppToSsd2683(uint8_t in, uint8_t* out0, uint8_t* out1)
{
    uint8_t first = 0;
    uint8_t second = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = (in >> (7 - i)) & 0x01;
        if (i < 4) {
            first |= bit << (8 - 2 * (i + 1));
        } else {
            second |= bit << (14 - 2 * i);
        }
    }
    *out0 = first;
    *out1 = second;
}

void InterleavePreviousAndCurrent(uint8_t previous, uint8_t current, uint8_t* out0, uint8_t* out1)
{
    uint16_t packed = 0;
    for (int bit = 0; bit < 8; ++bit) {
        const int src_bit = 7 - bit;
        packed |= static_cast<uint16_t>((previous >> src_bit) & 0x01U) << (src_bit * 2 + 1);
        packed |= static_cast<uint16_t>((current >> src_bit) & 0x01U) << (src_bit * 2);
    }
    *out0 = static_cast<uint8_t>(packed >> 8);
    *out1 = static_cast<uint8_t>(packed & 0xFF);
}

uint8_t Popcount8(uint8_t value)
{
    value = static_cast<uint8_t>(value - ((value >> 1) & 0x55));
    value = static_cast<uint8_t>((value & 0x33) + ((value >> 2) & 0x33));
    return static_cast<uint8_t>((value + (value >> 4)) & 0x0F);
}

struct DirtyRect {
    int x_start_byte = 0;
    int x_end_byte = 0;
    int y_start = 0;
    int y_end = 0;
    bool valid = false;
};

size_t CountFramebufferDiffBits(const uint8_t* previous, const uint8_t* current)
{
    if (previous == nullptr || current == nullptr) {
        return kEpdFramebufferSize * 8;
    }

    size_t diff_bits = 0;
    for (int i = 0; i < kEpdFramebufferSize; ++i) {
        diff_bits += Popcount8(static_cast<uint8_t>(previous[i] ^ current[i]));
    }
    return diff_bits;
}

DirtyRect FindDirtyRect(const uint8_t* previous, const uint8_t* current)
{
    DirtyRect rect;
    if (previous == nullptr || current == nullptr) {
        rect.x_start_byte = 0;
        rect.x_end_byte = kEpdBytesPerRow - 1;
        rect.y_start = 0;
        rect.y_end = kEpdHeight - 1;
        rect.valid = true;
        return rect;
    }

    int min_x_byte = kEpdBytesPerRow;
    int max_x_byte = -1;
    int min_y = kEpdHeight;
    int max_y = -1;
    for (int y = 0; y < kEpdHeight; ++y) {
        const uint8_t* previous_row = previous + y * kEpdBytesPerRow;
        const uint8_t* current_row = current + y * kEpdBytesPerRow;
        for (int xb = 0; xb < kEpdBytesPerRow; ++xb) {
            if (previous_row[xb] == current_row[xb]) {
                continue;
            }
            min_x_byte = std::min(min_x_byte, xb);
            max_x_byte = std::max(max_x_byte, xb);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }

    if (max_x_byte < 0) {
        return rect;
    }

    rect.x_start_byte = std::max(0, min_x_byte - 1);
    rect.x_end_byte = std::min(kEpdBytesPerRow - 1, max_x_byte + 1);
    rect.y_start = std::max(0, min_y - 4);
    rect.y_end = std::min(kEpdHeight - 1, max_y + 4);
    rect.valid = true;
    return rect;
}

DirtyRect AlignDirtyRect(const DirtyRect& rect)
{
    DirtyRect aligned = rect;
    if (!aligned.valid) {
        return aligned;
    }
    const int x_start_px = aligned.x_start_byte * 8;
    const int x_end_px = (aligned.x_end_byte + 1) * 8 - 1;
    const int aligned_x_start = (x_start_px / 8) * 8;
    const int aligned_x_end = ((x_end_px + 8) / 8) * 8 - 1;
    aligned.x_start_byte = std::max(0, aligned_x_start / 8);
    aligned.x_end_byte = std::min(kEpdBytesPerRow - 1, (aligned_x_end + 1) / 8 - 1);
    aligned.y_start = std::max(0, (aligned.y_start / 8) * 8);
    aligned.y_end = std::min(kEpdHeight - 1, ((aligned.y_end + 8) / 8) * 8 - 1);
    return aligned;
}

DirtyRect ClampDirtyRect(const DirtyRect& rect)
{
    DirtyRect clamped = rect;
    if (!clamped.valid) {
        return clamped;
    }
    clamped.x_start_byte = std::max(0, clamped.x_start_byte);
    clamped.x_end_byte = std::min(kEpdBytesPerRow - 1, clamped.x_end_byte);
    clamped.y_start = std::max(0, clamped.y_start);
    clamped.y_end = std::min(kEpdHeight - 1, clamped.y_end);
    if (clamped.x_end_byte < clamped.x_start_byte || clamped.y_end < clamped.y_start) {
        clamped.valid = false;
    }
    return clamped;
}

int DirtyRectPixelWidth(const DirtyRect& rect)
{
    return (rect.x_end_byte - rect.x_start_byte + 1) * 8;
}

int DirtyRectHeight(const DirtyRect& rect)
{
    return rect.y_end - rect.y_start + 1;
}

float DirtyRectAreaRatio(const DirtyRect& rect)
{
    if (!rect.valid) {
        return 0.0f;
    }
    const int pixels = DirtyRectPixelWidth(rect) * DirtyRectHeight(rect);
    return static_cast<float>(pixels) / static_cast<float>(kEpdWidth * kEpdHeight);
}

esp_err_t WaitPartialCooldown()
{
#ifdef CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE
    constexpr int64_t kPartialCooldownUs = static_cast<int64_t>(CONFIG_WQN_EPD_PARTIAL_COOLDOWN_MS) * 1000;
#else
    constexpr int64_t kPartialCooldownUs = 0;
#endif
    if (kPartialCooldownUs <= 0 || g_last_epd_refresh_us == 0) {
        return ESP_OK;
    }

    const int64_t elapsed_us = esp_timer_get_time() - g_last_epd_refresh_us;
    if (elapsed_us < kPartialCooldownUs) {
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((kPartialCooldownUs - elapsed_us + 999) / 1000)));
    }
    return ESP_OK;
}

esp_err_t WaitPanelStatusReady(int max_retries)
{
    for (int i = 0; i < max_retries; ++i) {
        ESP_RETURN_ON_ERROR(SendCommand(0xE0), kTag, "EPD status check cmd");
        ESP_RETURN_ON_ERROR(SendData(0x00), kTag, "EPD status check data");
        ESP_RETURN_ON_ERROR(SendCommand(0xA5), kTag, "EPD status probe cmd");
        const esp_err_t busy_ret = WaitBusyTimeout(kPartialCommandBusyTimeoutMs);
        if (busy_ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            return ESP_OK;
        }
        ESP_LOGW(kTag, "EPD panel status probe retry %d/%d", i + 1, max_retries);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(kTag, "EPD panel status not ready after %d retries", max_retries);
    return ESP_ERR_TIMEOUT;
}

bool ShouldKeepEpdPowered()
{
#ifdef CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE
    return true;
#else
    return false;
#endif
}

esp_err_t TriggerDisplayUpdate(bool is_partial, bool keep_powered)
{
    if (!g_epd_rail_powered) {
        PowerOnEpd();
    }
    if (!g_epd_powered) {
        esp_err_t ret = SendCommand(0x04);
        if (ret != ESP_OK) {
            DropEpdHotState(true, true);
            return ret;
        }
        ret = WaitBusyTimeout(is_partial ? kPartialRefreshBusyTimeoutMs : kBusyTimeoutMs);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "EPD power-on wait timed out; dropping hot refresh state");
            DropEpdHotState(true, true);
            return ret;
        }
        g_epd_powered = true;
    }
    esp_err_t ret = SendCommand(0x12);
    if (ret != ESP_OK) {
        DropEpdHotState(true, true);
        return ret;
    }
    ret = SendData(0x00);
    if (ret != ESP_OK) {
        DropEpdHotState(true, true);
        return ret;
    }
    const esp_err_t refresh_ret = WaitBusyTimeout(is_partial ? kPartialRefreshBusyTimeoutMs : kBusyTimeoutMs);
    if (refresh_ret != ESP_OK) {
        ESP_LOGW(kTag, "EPD %s refresh timed out; dropping hot refresh state", is_partial ? "partial" : "full");
        DropEpdHotState(true, true);
        return refresh_ret;
    }
    g_last_epd_refresh_us = esp_timer_get_time();
    g_hot_refresh_ok = keep_powered;
    if (!is_partial && !keep_powered) {
        ESP_RETURN_ON_ERROR(SendCommand(0x02), kTag, "EPD power off command");
        ESP_RETURN_ON_ERROR(SendData(0x00), kTag, "EPD power off data");
        ESP_RETURN_ON_ERROR(WaitBusy(), kTag, "wait EPD power off command");
        PowerOffEpd();
    }
    return ESP_OK;
}

void DrawPlaceholderGlyph(int x, int y, bool black)
{
    for (int yy = 0; yy < kTextGlyphHeight; ++yy) {
        for (int xx = 0; xx < kTextGlyphWidth; ++xx) {
            const bool edge = yy == 0 || yy == kTextGlyphHeight - 1 || xx == 0 || xx == kTextGlyphWidth - 1;
            if (edge) {
                DrawEpdPixel(x + xx, y + yy, black);
            }
        }
    }
}

[[maybe_unused]] void DrawPlaceholderCjkGlyph(int x, int y, bool black)
{
    for (int yy = 1; yy < kCjkFontHeight - 1; ++yy) {
        for (int xx = 1; xx < kCjkFallbackWidth - 1; ++xx) {
            const bool edge = yy == 1 || yy == kCjkFontHeight - 2 || xx == 1 || xx == kCjkFallbackWidth - 2;
            if (edge) {
                DrawEpdPixel(x + xx, y + yy, black);
            }
        }
    }
}

void DrawGlyph(int x, int y, unsigned char ch, bool black)
{
    if (ch < 0x20 || ch > 0x7E) {
        DrawPlaceholderGlyph(x, y, black);
        return;
    }

    const uint8_t* glyph = kFont5x7[ch - 0x20];
    for (int col = 0; col < kTextGlyphWidth; ++col) {
        for (int row = 0; row < kTextGlyphHeight; ++row) {
            if ((glyph[col] >> row) & 0x01) {
                DrawEpdPixel(x + col, y + row, black);
            }
        }
    }
}

const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(const lv_font_t* font, uint32_t codepoint)
{
    if (font == nullptr || codepoint == 0) {
        return nullptr;
    }
    const lv_font_fmt_txt_dsc_t* font_dsc = font->dsc;
    if (font_dsc == nullptr) {
        return nullptr;
    }

    for (uint16_t i = 0; i < font_dsc->cmap_num; ++i) {
        const lv_font_fmt_txt_cmap_t& cmap = font_dsc->cmaps[i];
        if (codepoint < cmap.range_start) {
            continue;
        }

        const uint32_t rcp = codepoint - cmap.range_start;
        if (rcp >= cmap.range_length) {
            continue;
        }

        uint32_t glyph_id = 0;
        switch (cmap.type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                glyph_id = cmap.glyph_id_start + rcp;
                break;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
                const uint8_t* offsets = static_cast<const uint8_t*>(cmap.glyph_id_ofs_list);
                if (offsets == nullptr) {
                    return nullptr;
                }
                if (offsets[rcp] == 0 && rcp != 0) {
                    return nullptr;
                }
                glyph_id = cmap.glyph_id_start + offsets[rcp];
                break;
            }
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
                const uint16_t key = static_cast<uint16_t>(rcp);
                const uint16_t* begin = cmap.unicode_list;
                const uint16_t* end = begin + cmap.list_length;
                if (begin == nullptr) {
                    return nullptr;
                }
                const uint16_t* found = std::lower_bound(begin, end, key);
                if (found == end || *found != key) {
                    return nullptr;
                }
                glyph_id = cmap.glyph_id_start + static_cast<uint32_t>(found - begin);
                break;
            }
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
                const uint16_t key = static_cast<uint16_t>(rcp);
                const uint16_t* begin = cmap.unicode_list;
                const uint16_t* end = begin + cmap.list_length;
                const uint16_t* offsets = static_cast<const uint16_t*>(cmap.glyph_id_ofs_list);
                if (begin == nullptr || offsets == nullptr) {
                    return nullptr;
                }
                const uint16_t* found = std::lower_bound(begin, end, key);
                if (found == end || *found != key) {
                    return nullptr;
                }
                glyph_id = cmap.glyph_id_start + offsets[found - begin];
                break;
            }
        }

        if (glyph_id == 0) {
            return nullptr;
        }
        return &font_dsc->glyph_dsc[glyph_id];
    }

    return nullptr;
}

bool DecodeUtf8(const char*& cursor, uint32_t* codepoint)
{
    if (cursor == nullptr || codepoint == nullptr || *cursor == '\0') {
        return false;
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(cursor);
    if (p[0] < 0x80) {
        *codepoint = p[0];
        cursor += 1;
        return true;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *codepoint = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        cursor += 2;
        return true;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *codepoint = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        cursor += 3;
        return true;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *codepoint = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        cursor += 4;
        return true;
    }

    *codepoint = '?';
    cursor += 1;
    return true;
}

// [font-fix] Map punctuation the slim font lacks a glyph for to an ASCII
// equivalent so it renders via the 5x7 ASCII path instead of a hollow box.
// Codepoints with no sensible ASCII equivalent (e.g. IPA phonetics) are left
// unchanged and skipped by DrawCjkGlyph/MeasureCodepointWidth.
uint32_t NormalizeCodepointForDisplay(uint32_t cp)
{
    switch (cp) {
        case 0x2018: case 0x2019: case 0xFF07: return 0x27;  // ' ' ＇ -> '
        case 0x201C: case 0x201D: return 0x22;               // " " -> "
        case 0x2013: case 0x2014: case 0x2212: return 0x2D;  // – — − -> -
        case 0x2026: return 0x2E;                            // … -> .
        case 0x00B7: case 0x2022: return 0x2E;               // · • -> .
        case 0x00A0: case 0x2007: case 0x202F: return 0x20;  // nbsp 等 -> 空格
        default: return cp;
    }
}

int MeasureGlyphWidthInFont(const lv_font_t* font, uint32_t codepoint)
{
    const lv_font_fmt_txt_glyph_dsc_t* glyph = FindGlyphInFont(font, codepoint);
    if (glyph == nullptr) {
        // [font-fix] No glyph (e.g. IPA phonetics): zero width, matches DrawGlyphFromFont.
        return 0;
    }
    return std::max<int>(1, (glyph->adv_w + 8) >> 4);
}

int MeasureCodepointWidth(uint32_t codepoint)
{
    if (codepoint == '\n' || codepoint == '\r') {
        return 0;
    }
    if (codepoint >= 0x20 && codepoint <= 0x7E) {
        return kTextCellWidth;
    }
    return MeasureGlyphWidthInFont(&SourceHanSansSC_Regular_slim, codepoint);
}

void DrawGlyphFromFont(int x, int y, const lv_font_t* font, uint32_t codepoint, bool black)
{
    if (font == nullptr) {
        return;
    }
    const lv_font_fmt_txt_dsc_t* font_dsc = font->dsc;
    const lv_font_fmt_txt_glyph_dsc_t* glyph = FindGlyphInFont(font, codepoint);
    if (font_dsc == nullptr || glyph == nullptr || glyph->box_w == 0 || glyph->box_h == 0) {
        // [font-fix] Missing glyph: draw nothing (skip) instead of a hollow box.
        return;
    }

    const uint8_t* bitmap = &font_dsc->glyph_bitmap[glyph->bitmap_index];
    const int draw_x = x + glyph->ofs_x;
    const int draw_y = y + (font->line_height - font->base_line) - glyph->box_h - glyph->ofs_y;
    int bit_index = 0;
    for (int row = 0; row < glyph->box_h; ++row) {
        for (int col = 0; col < glyph->box_w; ++col, ++bit_index) {
            const uint8_t byte = bitmap[bit_index >> 3];
            const uint8_t mask = static_cast<uint8_t>(0x80U >> (bit_index & 0x07));
            if ((byte & mask) != 0) {
                DrawEpdPixel(draw_x + col, draw_y + row, black);
            }
        }
    }
}

void DrawCjkGlyph(int x, int y, uint32_t codepoint, bool black)
{
    DrawGlyphFromFont(x, y, &SourceHanSansSC_Regular_slim, codepoint, black);
}

}  // namespace

esp_err_t InitEpdDisplay()
{
    EpdOperationGuard operation(portMAX_DELAY);
    ESP_RETURN_ON_FALSE(operation.locked(), ESP_ERR_NO_MEM, kTag, "create/take EPD operation mutex");
    if (g_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(InitGpio(), kTag, "init EPD GPIO");
    ESP_RETURN_ON_ERROR(InitSpiBus(false), kTag, "init EPD SPI");

    if (g_framebuffer == nullptr) {
        g_framebuffer = AllocateFramebuffer();
    }
    ESP_RETURN_ON_FALSE(g_framebuffer != nullptr, ESP_ERR_NO_MEM, kTag, "allocate EPD framebuffer");
    if (g_previous_framebuffer == nullptr) {
        g_previous_framebuffer = AllocateFramebuffer();
    }
    if (g_previous_framebuffer == nullptr) {
        FreeFramebuffer(g_framebuffer);
        ESP_LOGE(kTag, "previous EPD framebuffer allocation failed; released current framebuffer");
        return ESP_ERR_NO_MEM;
    }
    ClearEpdFramebuffer();
    std::memset(g_previous_framebuffer, 0xFF, kEpdFramebufferSize);
    g_previous_framebuffer_synced = false;
    g_partial_refreshes_since_full = 0;
    g_heavy_partials_since_full = 0;
    g_last_partial_was_full_frame = false;

    ESP_RETURN_ON_ERROR(InitPanelSequence(), kTag, "init EPD panel");
    g_initialized = true;
    return ESP_OK;
}

void ClearEpdFramebuffer(bool white)
{
    if (g_framebuffer == nullptr) {
        return;
    }
    std::memset(g_framebuffer, white ? 0xFF : 0x00, kEpdFramebufferSize);
}

void DrawEpdPixel(int x, int y, bool black)
{
    if (g_framebuffer == nullptr || x < 0 || x >= kEpdWidth || y < 0 || y >= kEpdHeight) {
        return;
    }

    const size_t index = static_cast<size_t>(y) * kEpdBytesPerRow + static_cast<size_t>(x / 8);
    const uint8_t mask = static_cast<uint8_t>(1U << (7 - (x & 0x07)));
    if (black) {
        g_framebuffer[index] &= static_cast<uint8_t>(~mask);
    } else {
        g_framebuffer[index] |= mask;
    }
}

esp_err_t DrawUtf8Text(int x, int y, const char* text, bool black)
{
    ESP_RETURN_ON_FALSE(g_framebuffer != nullptr, ESP_ERR_INVALID_STATE, kTag, "EPD framebuffer not allocated");
    ESP_RETURN_ON_FALSE(text != nullptr, ESP_ERR_INVALID_ARG, kTag, "text is null");

    int cursor_x = x;
    int cursor_y = y;
    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = 0;
        const char* before = cursor;
        if (!DecodeUtf8(cursor, &codepoint)) {
            break;
        }
        if (cursor == before) {
            break;
        }
        codepoint = NormalizeCodepointForDisplay(codepoint);
        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            cursor_y += kCjkLineHeight;
            cursor_x = x;
            continue;
        }

        const int glyph_width = MeasureCodepointWidth(codepoint);
        if (cursor_x + glyph_width > kEpdWidth) {
            break;
        }

        if (codepoint >= 0x20 && codepoint <= 0x7E) {
            DrawGlyph(cursor_x, cursor_y + kAsciiBaselineOffset, static_cast<unsigned char>(codepoint), black);
        } else {
            DrawCjkGlyph(cursor_x, cursor_y, codepoint, black);
        }
        cursor_x += glyph_width;
    }
    return ESP_OK;
}

int MeasureUtf8TextWidth(const char* text)
{
    if (text == nullptr) {
        return 0;
    }

    int max_width = 0;
    int current_width = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = 0;
        const char* before = cursor;
        if (!DecodeUtf8(cursor, &codepoint)) {
            break;
        }
        if (cursor == before) {
            break;
        }
        codepoint = NormalizeCodepointForDisplay(codepoint);
        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            max_width = std::max(max_width, current_width);
            current_width = 0;
            continue;
        }
        current_width += MeasureCodepointWidth(codepoint);
    }
    return std::max(max_width, current_width);
}

void DrawTextWithFont(int x, int y, const lv_font_t* font, const char* text, bool black)
{
    if (text == nullptr || font == nullptr) {
        return;
    }
    int cursor_x = x;
    int cursor_y = y;
    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = 0;
        const char* before = cursor;
        if (!DecodeUtf8(cursor, &codepoint)) {
            break;
        }
        if (cursor == before) {
            break;
        }
        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            cursor_y += font->line_height;
            cursor_x = x;
            continue;
        }

        const int glyph_width = MeasureGlyphWidthInFont(font, codepoint);
        if (cursor_x + glyph_width > kEpdWidth) {
            break;
        }
        DrawGlyphFromFont(cursor_x, cursor_y, font, codepoint, black);
        cursor_x += glyph_width;
    }
}

int MeasureTextWithFont(const lv_font_t* font, const char* text)
{
    if (font == nullptr || text == nullptr) {
        return 0;
    }
    int max_width = 0;
    int current_width = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = 0;
        const char* before = cursor;
        if (!DecodeUtf8(cursor, &codepoint)) {
            break;
        }
        if (cursor == before) {
            break;
        }
        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            max_width = std::max(max_width, current_width);
            current_width = 0;
            continue;
        }
        current_width += MeasureGlyphWidthInFont(font, codepoint);
    }
    return std::max(max_width, current_width);
}

void DrawTextWithFontCentered(int x, int y, int width, const lv_font_t* font, const char* text, bool black)
{
    if (font == nullptr || text == nullptr) {
        return;
    }
    const int text_width = MeasureTextWithFont(font, text);
    const int draw_x = x + std::max(0, (width - text_width) / 2);
    DrawTextWithFont(draw_x, y, font, text, black);
}

std::string TruncateUtf8TextToWidth(const std::string& text, int max_width_px)
{
    if (max_width_px <= 0 || MeasureUtf8TextWidth(text.c_str()) <= max_width_px) {
        return text;
    }

    constexpr const char* kEllipsis = "...";
    const int ellipsis_width = MeasureUtf8TextWidth(kEllipsis);
    const int text_width = std::max(0, max_width_px - ellipsis_width);
    std::string output;
    int current_width = 0;
    const char* cursor = text.c_str();
    while (*cursor != '\0') {
        const char* before = cursor;
        uint32_t codepoint = 0;
        if (!DecodeUtf8(cursor, &codepoint) || cursor == before) {
            break;
        }
        if (codepoint == '\r' || codepoint == '\n') {
            break;
        }

        const int glyph_width = MeasureCodepointWidth(codepoint);
        if (current_width + glyph_width > text_width) {
            break;
        }
        output.append(before, static_cast<size_t>(cursor - before));
        current_width += glyph_width;
    }
    output.append(kEllipsis);
    return output;
}

std::vector<std::string> WrapUtf8TextToWidth(const std::string& text, int max_width_px, size_t max_lines)
{
    std::vector<std::string> lines;
    if (max_width_px <= 0 || max_lines == 0) {
        return lines;
    }

    std::string line;
    int line_width = 0;
    const char* cursor = text.c_str();
    while (*cursor != '\0') {
        const char* before = cursor;
        uint32_t codepoint = 0;
        if (!DecodeUtf8(cursor, &codepoint) || cursor == before) {
            break;
        }

        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            lines.push_back(line);
            line.clear();
            line_width = 0;
            if (lines.size() >= max_lines) {
                break;
            }
            continue;
        }

        const int glyph_width = MeasureCodepointWidth(codepoint);
        if (!line.empty() && line_width + glyph_width > max_width_px) {
            lines.push_back(line);
            line.clear();
            line_width = 0;
            if (lines.size() >= max_lines) {
                break;
            }
        }

        if (glyph_width > max_width_px && line.empty()) {
            continue;
        }
        line.append(before, static_cast<size_t>(cursor - before));
        line_width += glyph_width;
    }

    if (!line.empty() && lines.size() < max_lines) {
        lines.push_back(line);
    }
    return lines;
}

namespace {

esp_err_t PreparePanelForFramebufferWrite()
{
    ESP_RETURN_ON_ERROR(SendCommand(0x50), kTag, "EPD display setting");
    ESP_RETURN_ON_ERROR(SendData(0x77), kTag, "EPD display setting data");
    ESP_RETURN_ON_ERROR(SendCommand(0x40), kTag, "EPD temperature command");
    ESP_RETURN_ON_ERROR(WaitBusy(), kTag, "wait EPD temperature command");

    uint8_t temperature = 0;
    ESP_RETURN_ON_ERROR(RecvData(&temperature), kTag, "read EPD temperature");
    const uint8_t vcom = TemperatureToVcom(temperature);
    ESP_LOGI(kTag, "EPD temperature raw=%u vcom=%u", temperature, vcom);

    ESP_RETURN_ON_ERROR(SendCommand(0xE0), kTag, "EPD active temperature");
    ESP_RETURN_ON_ERROR(SendData(0x02), kTag, "EPD active temperature data");
    ESP_RETURN_ON_ERROR(SendCommand(0xE6), kTag, "EPD vcom");
    ESP_RETURN_ON_ERROR(SendData(vcom), kTag, "EPD vcom data");
    ESP_RETURN_ON_ERROR(SendCommand(0xA5), kTag, "EPD booster");
    ESP_RETURN_ON_ERROR(WaitBusy(), kTag, "wait EPD booster");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t PreparePanelForHotFramebufferWrite()
{
    ESP_RETURN_ON_ERROR(SendCommand(0x10), kTag, "EPD hot DTM1 write");
    ESP_RETURN_ON_ERROR(WaitBusyTimeout(kPartialCommandBusyTimeoutMs), kTag, "wait EPD hot RAM command");
    return ESP_OK;
}

esp_err_t PreparePanelForLocalPartialWrite()
{
    ESP_RETURN_ON_ERROR(SendCommand(0x50), kTag, "EPD partial display setting");
    ESP_RETURN_ON_ERROR(SendData(0x77), kTag, "EPD partial display setting data");
    ESP_RETURN_ON_ERROR(WaitPartialCooldown(), kTag, "wait EPD local partial cooldown");
    return ESP_OK;
}

esp_err_t EnsurePanelReadyForWrite(bool allow_hot_reuse)
{
    if (!g_initialized) {
        ESP_RETURN_ON_ERROR(InitEpdDisplay(), kTag, "lazy init EPD");
    } else if (!allow_hot_reuse || !g_epd_rail_powered) {
        ESP_RETURN_ON_ERROR(InitPanelSequence(), kTag, "re-init EPD panel");
    }
    return ESP_OK;
}

esp_err_t SendFramebufferToPanel(bool partial, bool hot_update)
{
    ESP_RETURN_ON_ERROR(EnsurePanelReadyForWrite(partial && hot_update), kTag, "prepare EPD panel for framebuffer write");
    ESP_RETURN_ON_FALSE(g_framebuffer != nullptr, ESP_ERR_INVALID_STATE, kTag, "EPD framebuffer not allocated");
    if (partial) {
        ESP_RETURN_ON_FALSE(
            g_previous_framebuffer != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "previous EPD framebuffer not allocated");
    }

    if (partial) {
        ESP_RETURN_ON_ERROR(PreparePanelForHotFramebufferWrite(), kTag, "prepare EPD hot write");
    } else {
        ESP_RETURN_ON_ERROR(PreparePanelForFramebufferWrite(), kTag, "prepare EPD write");
        ESP_RETURN_ON_ERROR(SendCommand(0x10), kTag, "EPD DTM1 write");
    }

    uint8_t line[kEpdBytesPerRow * 2] = {};
    for (int y = 0; y < kEpdHeight; ++y) {
        const uint8_t* src = g_framebuffer + y * kEpdBytesPerRow;
        const uint8_t* previous = partial ? g_previous_framebuffer + y * kEpdBytesPerRow : nullptr;
        for (int xb = 0; xb < kEpdBytesPerRow; ++xb) {
            if (partial) {
                InterleavePreviousAndCurrent(previous[xb], src[xb], &line[xb * 2], &line[xb * 2 + 1]);
            } else {
                Pack1bppToSsd2683(src[xb], &line[xb * 2], &line[xb * 2 + 1]);
            }
        }
        ESP_RETURN_ON_ERROR(WriteBytes(line, sizeof(line)), kTag, "write EPD line");
        if ((y & 0x3F) == 0) {
            vTaskDelay(1);
        }
    }

    return TriggerDisplayUpdate(partial, partial && ShouldKeepEpdPowered());
}

esp_err_t SetPartialWindow(const DirtyRect& rect)
{
    ESP_RETURN_ON_FALSE(rect.valid, ESP_ERR_INVALID_ARG, kTag, "invalid EPD partial window");

    const uint16_t x_start = static_cast<uint16_t>(rect.x_start_byte * 8);
    const uint16_t x_end = static_cast<uint16_t>((rect.x_end_byte + 1) * 8 - 1);
    const uint16_t y_start = static_cast<uint16_t>(rect.y_start);
    const uint16_t y_end = static_cast<uint16_t>(rect.y_end);

    ESP_RETURN_ON_ERROR(SendCommand(0x83), kTag, "EPD partial window");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>((x_start >> 8) & 0x03)), kTag, "EPD partial x start high");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>(x_start & 0xFF)), kTag, "EPD partial x start low");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>((x_end >> 8) & 0x03)), kTag, "EPD partial x end high");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>(x_end & 0xFF)), kTag, "EPD partial x end low");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>((y_start >> 8) & 0x03)), kTag, "EPD partial y start high");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>(y_start & 0xFF)), kTag, "EPD partial y start low");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>((y_end >> 8) & 0x03)), kTag, "EPD partial y end high");
    ESP_RETURN_ON_ERROR(SendData(static_cast<uint8_t>(y_end & 0xFF)), kTag, "EPD partial y end low");
    ESP_RETURN_ON_ERROR(SendData(0x01), kTag, "EPD partial scan data");
    return ESP_OK;
}

esp_err_t SendDirtyRectToPanel(const DirtyRect& rect)
{
    const bool hot_ready = g_epd_rail_powered && g_epd_powered && g_hot_refresh_ok;
    ESP_RETURN_ON_ERROR(EnsurePanelReadyForWrite(hot_ready), kTag, "prepare EPD panel for local partial write");
    ESP_RETURN_ON_FALSE(g_framebuffer != nullptr, ESP_ERR_INVALID_STATE, kTag, "EPD framebuffer not allocated");
    ESP_RETURN_ON_FALSE(
        g_previous_framebuffer != nullptr,
        ESP_ERR_INVALID_STATE,
        kTag,
        "previous EPD framebuffer not allocated");
    ESP_RETURN_ON_FALSE(
        g_previous_framebuffer_synced,
        ESP_ERR_INVALID_STATE,
        kTag,
        "EPD local partial requires a synced previous framebuffer");

    DirtyRect aligned = ClampDirtyRect(AlignDirtyRect(rect));
    if (!aligned.valid) {
        ESP_LOGW(kTag, "EPD local partial: empty window after alignment/clamp, skipping");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(PreparePanelForLocalPartialWrite(), kTag, "prepare EPD local partial write");
    ESP_RETURN_ON_ERROR(WaitPanelStatusReady(3), kTag, "EPD panel status not ready for local partial");
    ESP_RETURN_ON_ERROR(SetPartialWindow(aligned), kTag, "set EPD partial window");
    ESP_RETURN_ON_ERROR(SendCommand(0x10), kTag, "EPD partial DTM1 write");
    ESP_RETURN_ON_ERROR(WaitBusyTimeout(kPartialCommandBusyTimeoutMs), kTag, "wait EPD partial RAM command");

    const int window_bytes = aligned.x_end_byte - aligned.x_start_byte + 1;
    uint8_t line[kEpdBytesPerRow * 2] = {};
    for (int y = aligned.y_start; y <= aligned.y_end; ++y) {
        const uint8_t* src = g_framebuffer + y * kEpdBytesPerRow + aligned.x_start_byte;
        const uint8_t* previous = g_previous_framebuffer + y * kEpdBytesPerRow + aligned.x_start_byte;
        for (int xb = 0; xb < window_bytes; ++xb) {
            InterleavePreviousAndCurrent(previous[xb], src[xb], &line[xb * 2], &line[xb * 2 + 1]);
        }
        ESP_RETURN_ON_ERROR(WriteBytes(line, static_cast<size_t>(window_bytes * 2)), kTag, "write EPD partial line");
        if ((y & 0x3F) == 0) {
            vTaskDelay(1);
        }
    }

    return TriggerDisplayUpdate(true, true);
}

}  // namespace

esp_err_t RefreshEpdFull(bool allow_local_partial, bool force_full_refresh)
{
    EpdOperationGuard operation(portMAX_DELAY);
    ESP_RETURN_ON_FALSE(operation.locked(), ESP_ERR_NO_MEM, kTag, "take EPD refresh operation mutex");
    ESP_LOGI(kTag, "EPD RefreshEpdFull: enter partial=%d full=%d fb=%p prev=%p synced=%d",
        allow_local_partial, force_full_refresh, g_framebuffer, g_previous_framebuffer, g_previous_framebuffer_synced);
    constexpr float kMaxLocalPartialAreaRatio = 0.45f;
    constexpr bool kEnableLocalPartialWindow =
#ifdef CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE
        true;
#else
        false;
#endif
    if (!g_initialized) {
        ESP_RETURN_ON_ERROR(InitEpdDisplay(), kTag, "lazy init EPD");
    }
    ESP_RETURN_ON_FALSE(g_framebuffer != nullptr, ESP_ERR_INVALID_STATE, kTag, "EPD framebuffer not allocated");
    ESP_RETURN_ON_FALSE(
        g_previous_framebuffer != nullptr,
        ESP_ERR_INVALID_STATE,
        kTag,
        "previous EPD framebuffer not allocated");

    // [power-fix] Compute CRC of what we want on screen.  If it matches the
    // CRC stored in RTC memory from the last real refresh, the panel already
    // shows this frame and we can skip the entire SPI transaction.
    const uint32_t current_crc = esp_crc32_le(~0U, g_framebuffer, kEpdFramebufferSize) ^ ~0U;
    if (!force_full_refresh && wqn::runtime::GetWakeContext().panel_cache_trusted &&
        g_rtc_last_frame_crc_valid && current_crc == g_rtc_last_frame_crc) {
        // Panel already shows this content -- restore RAM state to match and
        // skip the refresh to eliminate flicker and save ~200ms of CPU time.
        g_previous_framebuffer_synced = true;
        std::memcpy(g_previous_framebuffer, g_framebuffer, kEpdFramebufferSize);
        ESP_LOGI(kTag, "EPD refresh skipped: RTC CRC match (crc=0x%08x)", current_crc);
        return ESP_OK;
    }

    const size_t diff_bits = CountFramebufferDiffBits(g_previous_framebuffer, g_framebuffer);
    if (g_previous_framebuffer_synced && diff_bits == 0) {
        ESP_LOGI(kTag, "EPD refresh skipped: framebuffer unchanged");
        return ESP_OK;
    }

    const size_t total_bits = kEpdFramebufferSize * 8;
    const float diff_ratio = static_cast<float>(diff_bits) / static_cast<float>(total_bits);
    const DirtyRect dirty_rect = FindDirtyRect(g_previous_framebuffer, g_framebuffer);
    const float dirty_area_ratio = DirtyRectAreaRatio(dirty_rect);
    bool full_refresh = force_full_refresh || !g_previous_framebuffer_synced;
    if (!full_refresh &&
        (g_partial_refreshes_since_full >= kMaxPartialRefreshesBeforeFull ||
         g_heavy_partials_since_full >= kMaxHeavyPartialsBeforeFull)) {
        // [epd-stale-fix] Promote to a full refresh so accumulated partial-waveform
        // charge on the panel gets cleared before the next local-partial window
        // would risk a BUSY-pin timeout (EPD controller enters an unhealthy state
        // if it receives too many partial-update commands in a row). Without this
        // promotion the device gets stuck waiting for the BUSY pin to fall (1500 ms
        // probe timeout) followed by an automatic full-refresh recovery (~2 s),
        // which the user perceives as "卡顿".
        ESP_LOGI(kTag,
                 "EPD forcing full refresh: partial_since_full=%u heavy=%u limits=%u/%u",
                 static_cast<unsigned>(g_partial_refreshes_since_full),
                 static_cast<unsigned>(g_heavy_partials_since_full),
                 static_cast<unsigned>(kMaxPartialRefreshesBeforeFull),
                 static_cast<unsigned>(kMaxHeavyPartialsBeforeFull));
        full_refresh = true;
    }
    const bool hot_update = !full_refresh && g_epd_rail_powered && g_epd_powered && g_hot_refresh_ok;
    bool local_partial =
        allow_local_partial && kEnableLocalPartialWindow && !full_refresh && dirty_rect.valid &&
        dirty_area_ratio <= kMaxLocalPartialAreaRatio;
    if (local_partial && DirtyRectHeight(dirty_rect) > kLocalPartialMaxHeight) {
        local_partial = false;
    }
    if (local_partial && g_last_partial_was_full_frame) {
        // [epd-wedge-fix] Never chase a full-frame partial with a windowed
        // local partial; escalate to a clean full refresh instead. This both
        // avoids the 4 s BUSY wedge and clears the ghosting the full-frame
        // partial just built up.
        ESP_LOGI(kTag,
                 "EPD escalating to full refresh: windowed partial after full-frame partial");
        local_partial = false;
        full_refresh = true;
    }

    ESP_LOGI(
        kTag,
        "EPD refresh strategy: %s diff=%u/%u %.2f%% dirty=x%d..%d y%d..%d area=%.2f%% partial_since_full=%u",
        full_refresh ? "full" : (local_partial ? "local-partial" : "full-frame-partial"),
        static_cast<unsigned>(diff_bits),
        static_cast<unsigned>(total_bits),
        static_cast<double>(diff_ratio * 100.0f),
        dirty_rect.valid ? dirty_rect.x_start_byte * 8 : 0,
        dirty_rect.valid ? (dirty_rect.x_end_byte + 1) * 8 - 1 : 0,
        dirty_rect.valid ? dirty_rect.y_start : 0,
        dirty_rect.valid ? dirty_rect.y_end : 0,
        static_cast<double>(dirty_area_ratio * 100.0f),
        static_cast<unsigned>(g_partial_refreshes_since_full));

    esp_err_t refresh_ret =
        local_partial ? SendDirtyRectToPanel(dirty_rect) : SendFramebufferToPanel(!full_refresh, hot_update);
    if (refresh_ret != ESP_OK) {
        ESP_LOGW(
            kTag,
            "EPD %s failed: %s; clearing state and forcing full refresh recovery",
            local_partial ? "local partial" : "partial framebuffer",
            esp_err_to_name(refresh_ret));
        DropEpdHotState(true, true);
        if (!full_refresh) {
            ESP_LOGI(kTag, "EPD: attempting automatic full refresh recovery");
            refresh_ret = SendFramebufferToPanel(false, false);
            if (refresh_ret != ESP_OK) {
                ESP_LOGE(kTag, "EPD full refresh recovery failed: %s", esp_err_to_name(refresh_ret));
                DropEpdHotState(true, true);
                return refresh_ret;
            }
            ESP_LOGI(kTag, "EPD full refresh recovery succeeded");
            // The panel content now comes from a genuine full refresh: reset
            // the partial bookkeeping below accordingly.
            full_refresh = true;
        } else {
            return refresh_ret;
        }
    }
    ESP_RETURN_ON_ERROR(refresh_ret, kTag, "send EPD framebuffer");
    std::memcpy(g_previous_framebuffer, g_framebuffer, kEpdFramebufferSize);
    g_previous_framebuffer_synced = true;
    g_partial_refreshes_since_full = full_refresh ? 0 : g_partial_refreshes_since_full + 1;
    g_heavy_partials_since_full = full_refresh
        ? 0
        : g_heavy_partials_since_full + (diff_ratio >= kHeavyPartialDiffRatio ? 1 : 0);
    g_last_partial_was_full_frame = !full_refresh && !local_partial;
    // [power-fix] Record the CRC so deep-sleep wakeups can skip the next
    // refresh if the frame content hasn't changed.
    g_rtc_last_frame_crc = current_crc;
    g_rtc_last_frame_crc_valid = true;
    return ESP_OK;
}

static void PowerOffEpd()
{
    EpdOperationGuard operation(portMAX_DELAY);
    if (!operation.locked()) {
        ESP_LOGE(kTag, "EPD power-off skipped: operation mutex unavailable");
        return;
    }
    // [epd-leak-fix] Spec book §5.3 requires sending SSD1683 deep-sleep command
    // 0x07/0xA5 before cutting GPIO 6, otherwise the controller remains in an
    // active state and current backflows through protection diodes when the
    // rail drops. Best-effort: if SPI is busy or failed, fall through to the
    // rail cut so we never wedge the device on a power-down path.
    if (g_spi != nullptr && g_epd_rail_powered) {
        const esp_err_t deep_sleep_ret = SendCommand(0x07);
        if (deep_sleep_ret == ESP_OK) {
            if (SendData(0xA5) == ESP_OK) {
                // Do not poll BUSY here: once the SSD1683 enters deep sleep
                // it leaves BUSY asserted low (or floating) forever, so any
                // WaitBusyTimeout will always burn its full budget and emit
                // an "EPD BUSY wait timed out" warning. The SPI transmit is
                // synchronous, so by the time SendData returns the byte has
                // been clocked out; a 10ms delay is plenty for the controller
                // to latch the command before we cut the rail.
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            ESP_LOGW(kTag, "EPD deep-sleep command failed: %s", esp_err_to_name(deep_sleep_ret));
        }
    }

    DropEpdHotState(true, false);

    // [epd-leak-fix] Spec book §5.3 step 3: reconfigure SPI pins to low-output
    // or high-impedance after the rail drops, to prevent diode backflow into
    // the de-powered EPD module. gpio_hold_en keeps them there through deep sleep.
    constexpr gpio_num_t kSpiPins[] = {kEpdSck, kEpdMosi, kEpdCs, kEpdDc, kEpdReset};
    for (gpio_num_t pin : kSpiPins) {
        gpio_hold_dis(pin);
        gpio_set_level(pin, 0);
        gpio_hold_en(pin);
    }
}

static esp_err_t TryPowerOffEpd(uint32_t timeout_ms)
{
    EpdOperationGuard operation(pdMS_TO_TICKS(timeout_ms));
    if (!operation.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    // Recursive acquisition keeps every power-off path on the same lock.
    PowerOffEpd();
    return ESP_OK;
}

} // namespace wqn

void wqn::NoteEpdActivity()
{
    g_last_epd_activity_ms = esp_timer_get_time() / 1000;
    g_epd_idle_cut = false;
}

void wqn::PowerOffEpdAfterIdleIfNeeded()
{
    const int idle_ms = CONFIG_WQN_EPD_IDLE_POWER_OFF_MS;
    if (idle_ms <= 0 || g_epd_idle_cut || g_last_epd_activity_ms == 0) {
        return;
    }
    if ((esp_timer_get_time() / 1000 - g_last_epd_activity_ms) < idle_ms) {
        return;
    }
    // [epd-health] Deferred heavy-partial cleanup: forcing the full refresh
    // mid-scroll read as a 1.2 s freeze every few steps, so scrolling stays on
    // partials and the accumulated charge is cleared here instead, once the
    // user has stopped interacting and just before the rail drops.
    if (g_heavy_partials_since_full >= kIdleCleanupHeavyPartials &&
        g_initialized && g_framebuffer != nullptr) {
        EpdOperationGuard operation(0);
        if (operation.locked() &&
            g_heavy_partials_since_full >= kIdleCleanupHeavyPartials) {
            ESP_LOGI(kTag, "EPD idle cleanup full refresh: heavy=%u",
                     static_cast<unsigned>(g_heavy_partials_since_full));
            // Defeat the unchanged-framebuffer skip; the panel content is what
            // needs the clean waveform, not the pixels.
            g_previous_framebuffer_synced = false;
            const esp_err_t cleanup_ret = RefreshEpdFull(false, true);
            if (cleanup_ret != ESP_OK) {
                ESP_LOGW(kTag, "EPD idle cleanup full refresh failed: %s",
                         esp_err_to_name(cleanup_ret));
            }
        }
    }
    const esp_err_t result = TryPowerOffEpd(0);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "EPD idle power-off after %d ms", idle_ms);
        g_epd_idle_cut = true;
    } else if (result != ESP_ERR_TIMEOUT) {
        ESP_LOGW(kTag, "EPD idle power-off failed: %s", esp_err_to_name(result));
    }
}

esp_err_t wqn::PrepareDisplayForSleep(int64_t deadline_us)
{
    // PowerCoordinator may begin quiesce only when no SleepLease exists. Every
    // accepted UI frame owns kDisplay until its terminal result, so reaching
    // this service boundary proves there is no upstream frame left to drain.
    const int64_t remaining_us = deadline_us > 0
        ? deadline_us - esp_timer_get_time()
        : 0;
    if (deadline_us > 0 && remaining_us <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    const uint32_t timeout_ms = deadline_us > 0
        ? static_cast<uint32_t>((remaining_us + 999) / 1000)
        : 0;
    return TryPowerOffEpd(timeout_ms);
}

void wqn::RollbackDisplayAfterSleepAbort()
{
    // Power-off is a valid Ready state during ordinary runtime. The next
    // accepted DisplayIntent owns wake/re-init, so rollback must not touch
    // GPIO6 behind DisplayService's state machine.
}
