#include "audio_capture.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"

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
constexpr uint32_t kI2sDmaFrameNum = 256;
constexpr int kMaxConsecutiveReadTimeouts = 5;
constexpr int kAudioPowerWarmupMs = 250;
constexpr int64_t kAudioPowerIdleOffDelayUs = 15 * 1000 * 1000;

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

struct AudioServiceState {
    SemaphoreHandle_t mutex = nullptr;
    bool running = false;
    bool stop_requested = false;
    bool initialized = false;
    bool rx_enabled = false;
    bool audio_powered = false;
    bool power_off_pending = false;
    TaskHandle_t task = nullptr;
    i2c_master_bus_handle_t i2c_bus = nullptr;
    i2s_chan_handle_t rx = nullptr;
    esp_timer_handle_t power_timer = nullptr;
    wqn::AudioCaptureChunk chunk;
};

AudioServiceState g_audio;

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

void SetAudioPowerUnlocked(bool enabled)
{
    gpio_hold_dis(kAudioPower);
    gpio_set_level(kAudioPower, enabled ? 1 : 0);
    gpio_hold_en(kAudioPower);

    gpio_hold_dis(kAudioAmp);
    gpio_set_level(kAudioAmp, 0);
    gpio_hold_en(kAudioAmp);
}

void AudioPowerOffTimerCallback(void*)
{
    if (g_audio.mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (!g_audio.power_off_pending || g_audio.running) {
        xSemaphoreGive(g_audio.mutex);
        return;
    }
    g_audio.power_off_pending = false;
    g_audio.audio_powered = false;
    SetAudioPowerUnlocked(false);
    xSemaphoreGive(g_audio.mutex);
    ESP_LOGI(kTag, "audio power off after idle delay");
}

esp_err_t EnsureAudioService()
{
    if (g_audio.mutex == nullptr) {
        g_audio.mutex = xSemaphoreCreateMutex();
        if (g_audio.mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (g_audio.power_timer != nullptr) {
        return ESP_OK;
    }
    esp_timer_create_args_t args = {};
    args.callback = AudioPowerOffTimerCallback;
    args.arg = nullptr;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "wqn_audio_power";
    args.skip_unhandled_events = true;
    return esp_timer_create(&args, &g_audio.power_timer);
}

void CancelAudioPowerOffTimerUnlocked()
{
    if (g_audio.power_timer != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(g_audio.power_timer));
    }
}

void ScheduleAudioPowerOff()
{
    if (EnsureAudioService() != ESP_OK) {
        SetAudioPowerUnlocked(false);
        return;
    }
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    CancelAudioPowerOffTimerUnlocked();
    g_audio.power_off_pending = true;
    const esp_err_t result = esp_timer_start_once(g_audio.power_timer, kAudioPowerIdleOffDelayUs);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "schedule audio power off failed: %s", esp_err_to_name(result));
        g_audio.power_off_pending = false;
        g_audio.audio_powered = false;
        SetAudioPowerUnlocked(false);
    }
    xSemaphoreGive(g_audio.mutex);
}

esp_err_t PrepareAudioPowerForCapture()
{
    ESP_RETURN_ON_ERROR(EnsureAudioService(), kTag, "create audio service");

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    CancelAudioPowerOffTimerUnlocked();
    g_audio.power_off_pending = false;
    const bool was_powered = g_audio.audio_powered;
    g_audio.audio_powered = true;
    SetAudioPowerUnlocked(true);
    xSemaphoreGive(g_audio.mutex);

    if (!was_powered) {
        vTaskDelay(pdMS_TO_TICKS(kAudioPowerWarmupMs));
    }
    return ESP_OK;
}

esp_err_t InitI2c(i2c_master_bus_handle_t* bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*bus != nullptr) {
        return ESP_OK;
    }
    *bus = wqn::GetSharedI2cBusHandle();
    if (*bus == nullptr) {
        ESP_LOGW(kTag, "shared I2C bus not available, creating own");
        i2c_master_bus_config_t config = {};
        config.i2c_port = kCodecI2cPort;
        config.sda_io_num = kCodecSda;
        config.scl_io_num = kCodecScl;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = 1;
        return i2c_new_master_bus(&config, bus);
    }
    return ESP_OK;
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
    if (*rx_handle != nullptr) {
        if (!g_audio.rx_enabled) {
            ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx_handle), kTag, "enable I2S RX");
            g_audio.rx_enabled = true;
        }
        return ESP_OK;
    }
    i2s_chan_config_t channel_config = {};
    channel_config.id = I2S_NUM_0;
    channel_config.role = I2S_ROLE_MASTER;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = kI2sDmaFrameNum;
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
    std_config.slot_cfg.left_align = true;
    std_config.slot_cfg.big_endian = false;
    std_config.slot_cfg.bit_order_lsb = false;
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
    g_audio.rx_enabled = true;
    return ESP_OK;
}

void CleanupCaptureHardware(bool keep_power)
{
    if (g_audio.rx != nullptr && g_audio.rx_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(g_audio.rx));
        g_audio.rx_enabled = false;
    }
    if (!keep_power && g_audio.rx != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(g_audio.rx));
        g_audio.rx = nullptr;
    }
    if (!keep_power && g_audio.i2c_bus != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_del_master_bus(g_audio.i2c_bus));
        g_audio.i2c_bus = nullptr;
    }
    if (!keep_power) {
        if (EnsureAudioService() != ESP_OK) {
            SetAudioPowerUnlocked(false);
            return;
        }
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
        CancelAudioPowerOffTimerUnlocked();
        g_audio.power_off_pending = false;
        g_audio.audio_powered = false;
        SetAudioPowerUnlocked(false);
        xSemaphoreGive(g_audio.mutex);
    }
}

bool StopRequested()
{
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    const bool stop = g_audio.stop_requested;
    xSemaphoreGive(g_audio.mutex);
    return stop;
}

void CaptureTask(void*)
{
    esp_err_t result = PrepareAudioPowerForCapture();
    if (result == ESP_OK) {
        result = InitI2c(&g_audio.i2c_bus);
    }
    if (result == ESP_OK) {
        result = InitEs8311Adc(g_audio.i2c_bus);
    }
    if (result == ESP_OK) {
        result = InitI2s(&g_audio.rx);
    }

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    g_audio.initialized = result == ESP_OK;
    xSemaphoreGive(g_audio.mutex);

    if (result != ESP_OK) {
        ESP_LOGE(kTag, "audio capture init failed: %s", esp_err_to_name(result));
        CleanupCaptureHardware(false);
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
        g_audio.running = false;
        g_audio.task = nullptr;
        xSemaphoreGive(g_audio.mutex);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "capture start: 16kHz s16le mono from ES8311 stereo mix");
    int16_t buffer[kReadSamples] = {};
    const int64_t start_us = esp_timer_get_time();
    int64_t sum_squares = 0;
    int64_t left_abs_sum = 0;
    int64_t right_abs_sum = 0;
    int left_peak = 0;
    int right_peak = 0;
    size_t stereo_frames = 0;
    int consecutive_timeouts = 0;
    bool read_failed = false;
    while (!StopRequested()) {
        const int elapsed_ms = static_cast<int>((esp_timer_get_time() - start_us) / 1000);
        if (elapsed_ms >= kMaxCaptureMs) {
            ESP_LOGW(kTag, "capture auto-stop at max duration: %dms", elapsed_ms);
            break;
        }

        size_t bytes_read = 0;
        result = i2s_channel_read(g_audio.rx, buffer, sizeof(buffer), &bytes_read, pdMS_TO_TICKS(1000));
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
            const int left = static_cast<int>(buffer[i]);
            const int right = static_cast<int>(buffer[i + 1]);
            const int mixed = (left + right) / 2;
            const int16_t sample = static_cast<int16_t>(
                std::clamp(mixed,
                           static_cast<int>(std::numeric_limits<int16_t>::min()),
                           static_cast<int>(std::numeric_limits<int16_t>::max())));
            g_audio.chunk.samples.push_back(sample);
            const int abs_value = std::abs(static_cast<int>(sample));
            const int left_abs = std::abs(left);
            const int right_abs = std::abs(right);
            left_abs_sum += left_abs;
            right_abs_sum += right_abs;
            left_peak = std::max(left_peak, left_abs);
            right_peak = std::max(right_peak, right_abs);
            ++stereo_frames;
            sum_squares += static_cast<int64_t>(sample) * static_cast<int64_t>(sample);
            g_audio.chunk.peak = std::max<int16_t>(
                g_audio.chunk.peak,
                static_cast<int16_t>(std::min(abs_value, static_cast<int>(std::numeric_limits<int16_t>::max()))));
        }
        g_audio.chunk.duration_ms = static_cast<int>((esp_timer_get_time() - start_us) / 1000);
    }

    CleanupCaptureHardware(true);
    if (!g_audio.chunk.samples.empty()) {
        g_audio.chunk.rms = static_cast<int>(IntegerSqrt(sum_squares / g_audio.chunk.samples.size()));
    }
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (read_failed && g_audio.chunk.samples.empty()) {
        g_audio.chunk.duration_ms = 0;
    }
    const int logged_duration_ms = g_audio.chunk.duration_ms;
    const size_t logged_sample_count = g_audio.chunk.samples.size();
    const int16_t logged_peak = g_audio.chunk.peak;
    const int logged_rms = g_audio.chunk.rms;
    g_audio.running = false;
    g_audio.task = nullptr;
    xSemaphoreGive(g_audio.mutex);
    ESP_LOGI(
        kTag,
        "capture stop: duration_ms=%d mono_samples=%u peak=%d rms=%d left_peak=%d right_peak=%d left_mean_abs=%d right_mean_abs=%d",
        logged_duration_ms,
        static_cast<unsigned>(logged_sample_count),
        static_cast<int>(logged_peak),
        logged_rms,
        left_peak,
        right_peak,
        stereo_frames == 0 ? 0 : static_cast<int>(left_abs_sum / static_cast<int64_t>(stereo_frames)),
        stereo_frames == 0 ? 0 : static_cast<int>(right_abs_sum / static_cast<int64_t>(stereo_frames)));
    vTaskDelete(nullptr);
}

}  // namespace

namespace wqn {

esp_err_t StartAudioCapture()
{
    ESP_RETURN_ON_ERROR(EnsureAudioService(), kTag, "init audio service");
    AudioCaptureChunk next_chunk;
    next_chunk.samples.reserve(static_cast<size_t>(kAudioCaptureSampleRate) * (kMaxCaptureMs / 1000));

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (g_audio.running) {
        xSemaphoreGive(g_audio.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    g_audio.running = true;
    g_audio.stop_requested = false;
    g_audio.initialized = false;
    g_audio.chunk = std::move(next_chunk);
    xSemaphoreGive(g_audio.mutex);

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreate(CaptureTask, "wqn_audio_cap", 8192, nullptr, 6, &task);
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (created != pdPASS) {
        g_audio.running = false;
        g_audio.task = nullptr;
        xSemaphoreGive(g_audio.mutex);
        return ESP_ERR_NO_MEM;
    }
    g_audio.task = g_audio.running ? task : nullptr;
    xSemaphoreGive(g_audio.mutex);
    return ESP_OK;
}

esp_err_t StopAudioCapture(AudioCaptureChunk* chunk)
{
    ESP_RETURN_ON_ERROR(EnsureAudioService(), kTag, "init audio service");
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (!g_audio.running && g_audio.task == nullptr) {
        if (chunk != nullptr) {
            *chunk = g_audio.chunk;
        }
        xSemaphoreGive(g_audio.mutex);
        return ESP_OK;
    }
    g_audio.stop_requested = true;
    xSemaphoreGive(g_audio.mutex);

    for (int i = 0; i < 80; ++i) {
        if (!IsAudioCaptureRunning()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    const bool stopped = !g_audio.running;
    const bool has_samples = stopped && !g_audio.chunk.samples.empty();
    if (stopped && chunk != nullptr) {
        *chunk = g_audio.chunk;
    }
    xSemaphoreGive(g_audio.mutex);
    if (!stopped) {
        return ESP_ERR_TIMEOUT;
    }
    return has_samples ? ESP_OK : ESP_FAIL;
}

bool IsAudioCaptureRunning()
{
    if (g_audio.mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    const bool running = g_audio.running;
    xSemaphoreGive(g_audio.mutex);
    return running;
}

void ReleaseAudioCapturePower()
{
    ScheduleAudioPowerOff();
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
