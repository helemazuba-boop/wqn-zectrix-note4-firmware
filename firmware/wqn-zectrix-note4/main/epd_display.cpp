#include "epd_display.h"

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
#include "freertos/task.h"
#include "lvgl/lvgl.h"
#include "sdkconfig.h"

namespace wqn {
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
constexpr int kPartialRefreshBusyTimeoutMs = 1500;
constexpr int kPartialCommandBusyTimeoutMs = 1500;
constexpr int kLocalPartialMaxHeight = 170;
constexpr uint32_t kMaxPartialRefreshesBeforeFull = 20;
constexpr int kTextGlyphWidth = 5;
constexpr int kTextGlyphHeight = 7;
constexpr int kTextCellWidth = 6;
constexpr int kTextLineHeight = 9;
constexpr int kCjkLineHeight = 18;
constexpr int kCjkFallbackWidth = 16;
constexpr int kCjkFontHeight = 16;
constexpr int kCjkFontBaseLine = 2;

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
int64_t g_last_epd_refresh_us = 0;

// [power-fix] Persisted across deep-sleep resets so the EPD refresh task
// can skip redundant panel updates after an RTC-timer wakeup.  Without this,
// RAM state is lost on every deep sleep and the driver always forces a full
// refresh, causing visible flicker on every clock tick.
RTC_DATA_ATTR uint32_t g_rtc_last_frame_crc = 0;
RTC_DATA_ATTR bool g_rtc_last_frame_crc_valid = false;

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
    while (gpio_get_level(kEpdBusy) == 0) {
        if ((xTaskGetTickCount() - start) > timeout) {
            ESP_LOGW(kTag, "EPD BUSY wait timed out: %d ms elapsed, gpio8=%d",
                     timeout_ms, static_cast<int>(gpio_get_level(kEpdBusy)));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
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

void DrawPlaceholderCjkGlyph(int x, int y, bool black)
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

const lv_font_fmt_txt_glyph_dsc_t* FindCjkGlyph(uint32_t codepoint)
{
    const lv_font_fmt_txt_dsc_t* font_dsc = SourceHanSansSC_Regular_slim.dsc;
    if (font_dsc == nullptr || codepoint == 0) {
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

int MeasureCodepointWidth(uint32_t codepoint)
{
    if (codepoint == '\n' || codepoint == '\r') {
        return 0;
    }
    if (codepoint >= 0x20 && codepoint <= 0x7E) {
        return kTextCellWidth;
    }

    const lv_font_fmt_txt_glyph_dsc_t* glyph = FindCjkGlyph(codepoint);
    if (glyph == nullptr) {
        return kCjkFallbackWidth;
    }
    return std::max<int>(1, (glyph->adv_w + 8) >> 4);
}

void DrawCjkGlyph(int x, int y, uint32_t codepoint, bool black)
{
    const lv_font_fmt_txt_dsc_t* font_dsc = SourceHanSansSC_Regular_slim.dsc;
    const lv_font_fmt_txt_glyph_dsc_t* glyph = FindCjkGlyph(codepoint);
    if (font_dsc == nullptr || glyph == nullptr || glyph->box_w == 0 || glyph->box_h == 0) {
        DrawPlaceholderCjkGlyph(x, y, black);
        return;
    }

    const uint8_t* bitmap = &font_dsc->glyph_bitmap[glyph->bitmap_index];
    const int draw_x = x + glyph->ofs_x;
    const int draw_y = y + (kCjkFontHeight - kCjkFontBaseLine) - glyph->box_h - glyph->ofs_y;
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

}  // namespace

esp_err_t InitEpdDisplay()
{
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

    ESP_RETURN_ON_ERROR(InitPanelSequence(), kTag, "init EPD panel");
    g_initialized = true;
    return ESP_OK;
}

uint8_t* GetEpdFramebuffer()
{
    return g_framebuffer;
}

const uint8_t* GetEpdFramebufferConst()
{
    return g_framebuffer;
}

size_t GetEpdFramebufferSize()
{
    return kEpdFramebufferSize;
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

esp_err_t DrawAsciiText(int x, int y, const char* text, bool black)
{
    ESP_RETURN_ON_FALSE(g_framebuffer != nullptr, ESP_ERR_INVALID_STATE, kTag, "EPD framebuffer not allocated");
    ESP_RETURN_ON_FALSE(text != nullptr, ESP_ERR_INVALID_ARG, kTag, "text is null");

    int cursor_x = x;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != '\0'; ++p) {
        if (*p == '\n') {
            y += kTextLineHeight;
            cursor_x = x;
            continue;
        }
        DrawGlyph(cursor_x, y, *p, black);
        cursor_x += kTextCellWidth;
        if (cursor_x >= kEpdWidth) {
            break;
        }
    }
    return ESP_OK;
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
            DrawGlyph(cursor_x, cursor_y + 5, static_cast<unsigned char>(codepoint), black);
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

esp_err_t DrawSimpleText(int x, int y, const char* text)
{
    return DrawUtf8Text(x, y, text, true);
}

esp_err_t DrawTextLine(int line, const char* text, bool black)
{
    ESP_RETURN_ON_FALSE(line >= 0, ESP_ERR_INVALID_ARG, kTag, "negative text line");
    return DrawUtf8Text(0, line * kCjkLineHeight, text, black);
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
        if ((y & 0x0F) == 0) {
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
        if ((y & 0x0F) == 0) {
            vTaskDelay(1);
        }
    }

    return TriggerDisplayUpdate(true, true);
}

}  // namespace

esp_err_t RefreshEpdFull(bool allow_local_partial, bool force_full_refresh)
{
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
    if (!force_full_refresh && g_rtc_last_frame_crc_valid && current_crc == g_rtc_last_frame_crc) {
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
    if (!full_refresh && g_partial_refreshes_since_full >= kMaxPartialRefreshesBeforeFull) {
        ESP_LOGI(kTag, "EPD full refresh deferred: partial_since_full=%u", static_cast<unsigned>(g_partial_refreshes_since_full));
    }
    const bool hot_update = !full_refresh && g_epd_rail_powered && g_epd_powered && g_hot_refresh_ok;
    bool local_partial =
        allow_local_partial && kEnableLocalPartialWindow && !full_refresh && dirty_rect.valid &&
        dirty_area_ratio <= kMaxLocalPartialAreaRatio;
    if (local_partial && DirtyRectHeight(dirty_rect) > kLocalPartialMaxHeight) {
        local_partial = false;
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
        } else {
            return refresh_ret;
        }
    }
    ESP_RETURN_ON_ERROR(refresh_ret, kTag, "send EPD framebuffer");
    std::memcpy(g_previous_framebuffer, g_framebuffer, kEpdFramebufferSize);
    g_previous_framebuffer_synced = true;
    g_partial_refreshes_since_full = full_refresh ? 0 : g_partial_refreshes_since_full + 1;
    // [power-fix] Record the CRC so deep-sleep wakeups can skip the next
    // refresh if the frame content hasn't changed.
    g_rtc_last_frame_crc = current_crc;
    g_rtc_last_frame_crc_valid = true;
    return ESP_OK;
}

void PowerOffEpd()
{
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

}  // namespace wqn
