#include "audio_capture.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "wqn_audio_capture";

constexpr gpio_num_t kAudioPower = GPIO_NUM_42;
constexpr gpio_num_t kAudioAmp = GPIO_NUM_46;

constexpr gpio_num_t kI2sMclk = GPIO_NUM_14;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_15;
constexpr gpio_num_t kI2sWs = GPIO_NUM_38;
constexpr gpio_num_t kI2sDin = GPIO_NUM_16;

constexpr gpio_num_t kCodecSda = GPIO_NUM_47;
constexpr gpio_num_t kCodecScl = GPIO_NUM_48;
constexpr i2c_port_num_t kCodecI2cPort = I2C_NUM_0;
constexpr uint8_t kEs8311Address = 0x18;

constexpr int kStereoChannels = 2;
constexpr int kMaxCaptureMs = 20000;
constexpr size_t kReadFrames = 240;
constexpr size_t kReadSamples = kReadFrames * kStereoChannels;
constexpr int kMaxConsecutiveReadTimeouts = 5;

constexpr uint8_t ES8311_RESET_REG00 = 0x00;
constexpr uint8_t ES8311_CLK_MANAGER_REG01 = 0x01;
constexpr uint8_t ES8311_CLK_MANAGER_REG02 = 0x02;
constexpr uint8_t ES8311_CLK_MANAGER_REG03 = 0x03;
constexpr uint8_t ES8311_CLK_MANAGER_REG04 = 0x04;
constexpr uint8_t ES8311_CLK_MANAGER_REG05 = 0x05;
constexpr uint8_t ES8311_CLK_MANAGER_REG06 = 0x06;
constexpr uint8_t ES8311_CLK_MANAGER_REG07 = 0x07;
constexpr uint8_t ES8311_CLK_MANAGER_REG08 = 0x08;
constexpr uint8_t ES8311_SDPIN_REG09 = 0x09;
constexpr uint8_t ES8311_SDPOUT_REG0A = 0x0A;
constexpr uint8_t ES8311_SYSTEM_REG0B = 0x0B;
constexpr uint8_t ES8311_SYSTEM_REG0C = 0x0C;
constexpr uint8_t ES8311_SYSTEM_REG0D = 0x0D;
constexpr uint8_t ES8311_SYSTEM_REG0E = 0x0E;
constexpr uint8_t ES8311_SYSTEM_REG10 = 0x10;
constexpr uint8_t ES8311_SYSTEM_REG11 = 0x11;
constexpr uint8_t ES8311_SYSTEM_REG12 = 0x12;
constexpr uint8_t ES8311_SYSTEM_REG13 = 0x13;
constexpr uint8_t ES8311_SYSTEM_REG14 = 0x14;
constexpr uint8_t ES8311_ADC_REG15 = 0x15;
constexpr uint8_t ES8311_ADC_REG16 = 0x16;
constexpr uint8_t ES8311_ADC_REG17 = 0x17;
constexpr uint8_t ES8311_ADC_REG1B = 0x1B;
constexpr uint8_t ES8311_ADC_REG1C = 0x1C;
constexpr uint8_t ES8311_DAC_REG37 = 0x37;
constexpr uint8_t ES8311_GPIO_REG44 = 0x44;
constexpr uint8_t ES8311_GP_REG45 = 0x45;

struct CaptureState {
    bool running = false;
    bool stop_requested = false;
    bool initialized = false;
    TaskHandle_t task = nullptr;
    i2c_master_bus_handle_t i2c_bus = nullptr;
    i2s_chan_handle_t rx = nullptr;
    wqn::AudioCaptureChunk chunk;
};

CaptureState g_capture;
portMUX_TYPE g_capture_lock = portMUX_INITIALIZER_UNLOCKED;

int64_t IntegerSqrt(int64_t value)
{
    if (value <= 0) {
        return 0;
    }
    int64_t result = value;
    int64_t candidate = (result + 1) / 2;
    while (candidate < result) {
        result = candidate;
        candidate = (result + value / result) / 2;
    }
    return result;
}

void SetAudioPower(bool enabled)
{
    gpio_hold_dis(kAudioPower);
    gpio_set_level(kAudioPower, enabled ? 1 : 0);
    gpio_hold_en(kAudioPower);

    gpio_hold_dis(kAudioAmp);
    gpio_set_level(kAudioAmp, 0);
    gpio_hold_en(kAudioAmp);
}

esp_err_t InitI2c(i2c_master_bus_handle_t* bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_config_t config = {};
    config.i2c_port = kCodecI2cPort;
    config.sda_io_num = kCodecSda;
    config.scl_io_num = kCodecScl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = 1;
    return i2c_new_master_bus(&config, bus);
}

esp_err_t AddCodecDevice(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t* dev)
{
    if (bus == nullptr || dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kEs8311Address;
    config.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &config, dev);
}

esp_err_t WriteCodecReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), 100);
}

esp_err_t ReadCodecReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(dev, &reg, sizeof(reg), value, sizeof(*value), 100);
}

esp_err_t InitEs8311Adc(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = nullptr;
    ESP_RETURN_ON_ERROR(AddCodecDevice(bus, &dev), kTag, "add ES8311 device");

    auto write = [&](uint8_t reg, uint8_t value) -> esp_err_t {
        return WriteCodecReg(dev, reg, value);
    };
    auto read = [&](uint8_t reg, uint8_t* value) -> esp_err_t {
        return ReadCodecReg(dev, reg, value);
    };

    esp_err_t ret = ESP_OK;
    ret |= write(ES8311_GPIO_REG44, 0x08);
    ret |= write(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= write(ES8311_ADC_REG16, 0x24);
    ret |= write(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= write(ES8311_SYSTEM_REG0B, 0x00);
    ret |= write(ES8311_SYSTEM_REG0C, 0x00);
    ret |= write(ES8311_SYSTEM_REG10, 0x1F);
    ret |= write(ES8311_SYSTEM_REG11, 0x7F);
    ret |= write(ES8311_RESET_REG00, 0x80);

    uint8_t reg = 0;
    if (read(ES8311_RESET_REG00, &reg) == ESP_OK) {
        ret |= write(ES8311_RESET_REG00, reg & 0xBF);
    } else {
        ret = ESP_FAIL;
    }
    ret |= write(ES8311_CLK_MANAGER_REG01, 0x3F);
    if (read(ES8311_CLK_MANAGER_REG06, &reg) == ESP_OK) {
        ret |= write(ES8311_CLK_MANAGER_REG06, reg & ~0x20);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_SYSTEM_REG13, 0x10);
    ret |= write(ES8311_ADC_REG1B, 0x0A);
    ret |= write(ES8311_ADC_REG1C, 0x6A);
    ret |= write(ES8311_GPIO_REG44, 0x58);
    ret |= write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG06, 0x0F);
    ret |= write(ES8311_CLK_MANAGER_REG07, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG08, 0xFF);

    if (read(ES8311_SDPOUT_REG0A, &reg) == ESP_OK) {
        ret |= write(ES8311_SDPOUT_REG0A, reg & ~0x40);
    } else {
        ret = ESP_FAIL;
    }
    if (read(ES8311_SDPIN_REG09, &reg) == ESP_OK) {
        ret |= write(ES8311_SDPIN_REG09, reg & ~0x40);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_ADC_REG17, 0xBF);
    ret |= write(ES8311_SYSTEM_REG0E, 0x02);
    ret |= write(ES8311_SYSTEM_REG12, 0x00);
    ret |= write(ES8311_SYSTEM_REG14, 0x1A);
    if (read(ES8311_SYSTEM_REG14, &reg) == ESP_OK) {
        ret |= write(ES8311_SYSTEM_REG14, reg & ~0x40);
    } else {
        ret = ESP_FAIL;
    }
    ret |= write(ES8311_SYSTEM_REG0D, 0x01);
    ret |= write(ES8311_ADC_REG15, 0x40);
    ret |= write(ES8311_DAC_REG37, 0x08);
    ret |= write(ES8311_GP_REG45, 0x00);

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(dev));
    return ret;
}

esp_err_t InitI2s(i2s_chan_handle_t* rx_handle)
{
    if (rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2s_chan_config_t channel_config = {};
    channel_config.id = I2S_NUM_0;
    channel_config.role = I2S_ROLE_MASTER;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 240;
    channel_config.auto_clear_after_cb = true;
    channel_config.auto_clear_before_cb = false;
    channel_config.intr_priority = 0;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, nullptr, rx_handle), kTag, "create I2S RX channel");

    i2s_std_config_t std_config = {};
    std_config.clk_cfg.sample_rate_hz = wqn::kAudioCaptureSampleRate;
    std_config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_config.slot_cfg.ws_pol = false;
    std_config.slot_cfg.bit_shift = true;
    std_config.gpio_cfg.mclk = kI2sMclk;
    std_config.gpio_cfg.bclk = kI2sBclk;
    std_config.gpio_cfg.ws = kI2sWs;
    std_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.din = kI2sDin;
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*rx_handle, &std_config), kTag, "init I2S RX std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx_handle), kTag, "enable I2S RX");
    return ESP_OK;
}

void CleanupCaptureHardware(bool keep_power)
{
    if (g_capture.rx != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(g_capture.rx));
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(g_capture.rx));
        g_capture.rx = nullptr;
    }
    if (g_capture.i2c_bus != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_del_master_bus(g_capture.i2c_bus));
        g_capture.i2c_bus = nullptr;
    }
    if (!keep_power) {
        SetAudioPower(false);
    }
}

bool StopRequested()
{
    portENTER_CRITICAL(&g_capture_lock);
    const bool stop = g_capture.stop_requested;
    portEXIT_CRITICAL(&g_capture_lock);
    return stop;
}

void CaptureTask(void*)
{
    SetAudioPower(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t result = InitI2c(&g_capture.i2c_bus);
    if (result == ESP_OK) {
        result = InitEs8311Adc(g_capture.i2c_bus);
    }
    if (result == ESP_OK) {
        result = InitI2s(&g_capture.rx);
    }

    portENTER_CRITICAL(&g_capture_lock);
    g_capture.initialized = result == ESP_OK;
    portEXIT_CRITICAL(&g_capture_lock);

    if (result != ESP_OK) {
        ESP_LOGE(kTag, "audio capture init failed: %s", esp_err_to_name(result));
        CleanupCaptureHardware(false);
        portENTER_CRITICAL(&g_capture_lock);
        g_capture.running = false;
        g_capture.task = nullptr;
        portEXIT_CRITICAL(&g_capture_lock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "capture start: 16kHz s16le mono from ES8311 left slot");
    int16_t buffer[kReadSamples] = {};
    const int64_t start_us = esp_timer_get_time();
    int64_t sum_squares = 0;
    int consecutive_timeouts = 0;
    bool read_failed = false;
    while (!StopRequested()) {
        const int elapsed_ms = static_cast<int>((esp_timer_get_time() - start_us) / 1000);
        if (elapsed_ms >= kMaxCaptureMs) {
            ESP_LOGW(kTag, "capture auto-stop at max duration: %dms", elapsed_ms);
            break;
        }

        size_t bytes_read = 0;
        result = i2s_channel_read(g_capture.rx, buffer, sizeof(buffer), &bytes_read, pdMS_TO_TICKS(1000));
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "I2S read failed: %s", esp_err_to_name(result));
            if (++consecutive_timeouts >= kMaxConsecutiveReadTimeouts) {
                read_failed = true;
                break;
            }
            continue;
        }
        consecutive_timeouts = 0;

        const size_t samples_read = bytes_read / sizeof(int16_t);
        for (size_t i = 0; i + 1 < samples_read; i += 2) {
            const int16_t sample = buffer[i];
            g_capture.chunk.samples.push_back(sample);
            const int abs_value = std::abs(static_cast<int>(sample));
            sum_squares += static_cast<int64_t>(sample) * static_cast<int64_t>(sample);
            g_capture.chunk.peak = std::max<int16_t>(
                g_capture.chunk.peak,
                static_cast<int16_t>(std::min(abs_value, static_cast<int>(std::numeric_limits<int16_t>::max()))));
        }
        g_capture.chunk.duration_ms = static_cast<int>((esp_timer_get_time() - start_us) / 1000);
    }

    CleanupCaptureHardware(true);
    if (!g_capture.chunk.samples.empty()) {
        g_capture.chunk.rms = static_cast<int>(IntegerSqrt(sum_squares / g_capture.chunk.samples.size()));
    }
    portENTER_CRITICAL(&g_capture_lock);
    if (read_failed && g_capture.chunk.samples.empty()) {
        g_capture.chunk.duration_ms = 0;
    }
    g_capture.running = false;
    g_capture.task = nullptr;
    portEXIT_CRITICAL(&g_capture_lock);
    ESP_LOGI(
        kTag,
        "capture stop: duration_ms=%d mono_samples=%u peak=%d rms=%d",
        g_capture.chunk.duration_ms,
        static_cast<unsigned>(g_capture.chunk.samples.size()),
        static_cast<int>(g_capture.chunk.peak),
        g_capture.chunk.rms);
    vTaskDelete(nullptr);
}

}  // namespace

namespace wqn {

esp_err_t StartAudioCapture()
{
    portENTER_CRITICAL(&g_capture_lock);
    if (g_capture.running) {
        portEXIT_CRITICAL(&g_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_capture.running = true;
    g_capture.stop_requested = false;
    g_capture.initialized = false;
    g_capture.chunk = AudioCaptureChunk{};
    g_capture.chunk.samples.reserve(static_cast<size_t>(kAudioCaptureSampleRate) * (kMaxCaptureMs / 1000));
    portEXIT_CRITICAL(&g_capture_lock);

    const BaseType_t created = xTaskCreate(CaptureTask, "wqn_audio_cap", 8192, nullptr, 6, &g_capture.task);
    if (created != pdPASS) {
        portENTER_CRITICAL(&g_capture_lock);
        g_capture.running = false;
        g_capture.task = nullptr;
        portEXIT_CRITICAL(&g_capture_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t StopAudioCapture(AudioCaptureChunk* chunk)
{
    portENTER_CRITICAL(&g_capture_lock);
    if (!g_capture.running && g_capture.task == nullptr) {
        portEXIT_CRITICAL(&g_capture_lock);
        if (chunk != nullptr) {
            *chunk = g_capture.chunk;
        }
        return ESP_OK;
    }
    g_capture.stop_requested = true;
    portEXIT_CRITICAL(&g_capture_lock);

    for (int i = 0; i < 80; ++i) {
        if (!IsAudioCaptureRunning()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (chunk != nullptr) {
        *chunk = g_capture.chunk;
    }
    portENTER_CRITICAL(&g_capture_lock);
    const bool stopped = !g_capture.running;
    const bool has_samples = !g_capture.chunk.samples.empty();
    portEXIT_CRITICAL(&g_capture_lock);
    if (!stopped) {
        return ESP_ERR_TIMEOUT;
    }
    return has_samples ? ESP_OK : ESP_FAIL;
}

bool IsAudioCaptureRunning()
{
    portENTER_CRITICAL(&g_capture_lock);
    const bool running = g_capture.running;
    portEXIT_CRITICAL(&g_capture_lock);
    return running;
}

void ReleaseAudioCapturePower()
{
    SetAudioPower(false);
}

}  // namespace wqn

#else

namespace wqn {

esp_err_t StartAudioCapture()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t StopAudioCapture(AudioCaptureChunk*)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ReleaseAudioCapturePower() {}

bool IsAudioCaptureRunning()
{
    return false;
}

}  // namespace wqn

#endif
