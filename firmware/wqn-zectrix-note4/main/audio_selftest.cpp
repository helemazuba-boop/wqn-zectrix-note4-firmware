#include "audio_selftest.h"

#if CONFIG_WQN_AUDIO_SELFTEST_ENABLE

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"

namespace {

constexpr char kTag[] = "wqn_audio_selftest";

constexpr gpio_num_t kAudioPower = GPIO_NUM_42;
constexpr gpio_num_t kAudioAmp = GPIO_NUM_46;

constexpr gpio_num_t kI2sMclk = GPIO_NUM_14;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_15;
constexpr gpio_num_t kI2sWs = GPIO_NUM_38;
constexpr gpio_num_t kI2sDin = GPIO_NUM_16;
constexpr gpio_num_t kI2sDout = GPIO_NUM_45;

constexpr gpio_num_t kCodecSda = GPIO_NUM_47;
constexpr gpio_num_t kCodecScl = GPIO_NUM_48;
constexpr i2c_port_num_t kCodecI2cPort = I2C_NUM_0;
constexpr uint8_t kEs8311Address = 0x18;

constexpr int kSampleRate = 16000;
constexpr int kChannels = 2;
constexpr int kCaptureSeconds = 2;
constexpr size_t kReadFrames = 240;
constexpr size_t kReadSamples = kReadFrames * kChannels;

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

struct ChannelStats {
    int64_t sum_abs = 0;
    int64_t sum_square = 0;
    int16_t peak = 0;
    size_t samples = 0;
};

void Accumulate(ChannelStats* stats, int16_t sample)
{
    if (stats == nullptr) {
        return;
    }
    const int value = sample;
    const int abs_value = std::abs(value);
    stats->sum_abs += abs_value;
    stats->sum_square += static_cast<int64_t>(value) * value;
    stats->peak = std::max<int16_t>(
        stats->peak,
        static_cast<int16_t>(std::min(abs_value, static_cast<int>(std::numeric_limits<int16_t>::max()))));
    stats->samples++;
}

int64_t IntegerSqrt(int64_t value)
{
    if (value <= 0) {
        return 0;
    }
    int64_t result = 0;
    int64_t bit = 1LL << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

void LogChannelStats(const char* name, const ChannelStats& stats)
{
    if (stats.samples == 0) {
        ESP_LOGW(kTag, "%s channel: no samples", name);
        return;
    }

    const int64_t mean_abs = stats.sum_abs / static_cast<int64_t>(stats.samples);
    const int64_t mean_square = stats.sum_square / static_cast<int64_t>(stats.samples);
    const int64_t rms = IntegerSqrt(mean_square);
    ESP_LOGI(
        kTag,
        "%s channel: samples=%u mean_abs=%lld rms=%lld peak=%d",
        name,
        static_cast<unsigned>(stats.samples),
        static_cast<long long>(mean_abs),
        static_cast<long long>(rms),
        static_cast<int>(stats.peak));
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
    i2c_master_bus_handle_t shared = wqn::GetSharedI2cBusHandle();
    if (shared != nullptr) {
        *bus = shared;
        return ESP_OK;
    }
    i2c_master_bus_config_t config = {};
    config.i2c_port = kCodecI2cPort;
    config.sda_io_num = kCodecSda;
    config.scl_io_num = kCodecScl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.intr_priority = 0;
    config.trans_queue_depth = 0;
    config.flags.enable_internal_pullup = 1;
    return i2c_new_master_bus(&config, bus);
}

esp_err_t ProbeEs8311(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kEs8311Address;
    config.scl_speed_hz = 100000;

    i2c_master_dev_handle_t dev = nullptr;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &dev), kTag, "add ES8311 I2C device");

    uint8_t reg = 0x00;
    uint8_t value = 0;
    const esp_err_t result = i2c_master_transmit_receive(dev, &reg, sizeof(reg), &value, sizeof(value), 100);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 I2C probe ok: addr=0x%02x reg00=0x%02x", kEs8311Address, value);
    } else {
        ESP_LOGW(kTag, "ES8311 I2C probe failed: addr=0x%02x err=%s", kEs8311Address, esp_err_to_name(result));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(dev));
    return result;
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

esp_err_t InitEs8311Adc(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = nullptr;
    ESP_RETURN_ON_ERROR(AddCodecDevice(bus, &dev), kTag, "add ES8311 init device");

    auto write = [&](uint8_t reg, uint8_t value) -> esp_err_t {
        const esp_err_t ret = WriteCodecReg(dev, reg, value);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "ES8311 write failed: reg=0x%02x value=0x%02x err=%s", reg, value, esp_err_to_name(ret));
        }
        return ret;
    };
    auto read = [&](uint8_t reg, uint8_t* value) -> esp_err_t {
        const esp_err_t ret = ReadCodecReg(dev, reg, value);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "ES8311 read failed: reg=0x%02x err=%s", reg, esp_err_to_name(ret));
        }
        return ret;
    };

    esp_err_t ret = ESP_OK;
    ret |= write(ES8311_GPIO_REG44, 0x08);
    ret |= write(ES8311_GPIO_REG44, 0x08);
    ret |= write(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= write(ES8311_ADC_REG16, 0x24);  // 30 dB MIC gain, matching official open-source wrapper.
    ret |= write(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= write(ES8311_SYSTEM_REG0B, 0x00);
    ret |= write(ES8311_SYSTEM_REG0C, 0x00);
    ret |= write(ES8311_SYSTEM_REG10, 0x1F);
    ret |= write(ES8311_SYSTEM_REG11, 0x7F);
    ret |= write(ES8311_RESET_REG00, 0x80);

    uint8_t reg00 = 0;
    if (read(ES8311_RESET_REG00, &reg00) == ESP_OK) {
        reg00 &= 0xBF;  // Slave mode; ESP32-S3 provides MCLK/BCLK/LRCK.
        ret |= write(ES8311_RESET_REG00, reg00);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_CLK_MANAGER_REG01, 0x3F);  // Use external MCLK, not inverted.
    uint8_t reg06 = 0;
    if (read(ES8311_CLK_MANAGER_REG06, &reg06) == ESP_OK) {
        reg06 &= ~0x20;  // SCLK not inverted.
        ret |= write(ES8311_CLK_MANAGER_REG06, reg06);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_SYSTEM_REG13, 0x10);
    ret |= write(ES8311_ADC_REG1B, 0x0A);
    ret |= write(ES8311_ADC_REG1C, 0x6A);
    ret |= write(ES8311_GPIO_REG44, 0x58);  // Internal reference signal enabled, as in esp_codec_dev default.

    // Equivalent to esp_codec_dev_open + enable for ADC path at 16 kHz / 16-bit / I2S normal.
    ret |= write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG06, 0x0F);  // BCLK divider for 16 kHz stereo 16-bit with 256x MCLK.
    ret |= write(ES8311_CLK_MANAGER_REG07, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG08, 0xFF);

    uint8_t adc_iface = 0;
    if (read(ES8311_SDPOUT_REG0A, &adc_iface) == ESP_OK) {
        adc_iface &= ~0x40;
        ret |= write(ES8311_SDPOUT_REG0A, adc_iface);
    } else {
        ret = ESP_FAIL;
    }
    uint8_t dac_iface = 0;
    if (read(ES8311_SDPIN_REG09, &dac_iface) == ESP_OK) {
        dac_iface &= ~0x40;
        ret |= write(ES8311_SDPIN_REG09, dac_iface);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_ADC_REG17, 0xBF);
    ret |= write(ES8311_SYSTEM_REG0E, 0x02);
    ret |= write(ES8311_SYSTEM_REG12, 0x00);
    ret |= write(ES8311_SYSTEM_REG14, 0x1A);
    uint8_t reg14 = 0;
    if (read(ES8311_SYSTEM_REG14, &reg14) == ESP_OK) {
        reg14 &= ~0x40;  // analog mic path, not digital mic.
        ret |= write(ES8311_SYSTEM_REG14, reg14);
    } else {
        ret = ESP_FAIL;
    }
    ret |= write(ES8311_SYSTEM_REG0D, 0x01);
    ret |= write(ES8311_ADC_REG15, 0x40);
    ret |= write(ES8311_DAC_REG37, 0x08);
    ret |= write(ES8311_GP_REG45, 0x00);

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(dev));
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 ADC init ok: input_gain=30db sample_rate=%d", kSampleRate);
    } else {
        ESP_LOGE(kTag, "ES8311 ADC init failed");
    }
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
    std_config.clk_cfg.sample_rate_hz = kSampleRate;
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

esp_err_t CaptureI2sStats()
{
    i2s_chan_handle_t rx_handle = nullptr;
    esp_err_t result = InitI2s(&rx_handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "I2S init failed: %s", esp_err_to_name(result));
        return result;
    }

    ChannelStats left;
    ChannelStats right;
    int16_t buffer[kReadSamples] = {};
    const int64_t target_frames = static_cast<int64_t>(kSampleRate) * kCaptureSeconds;
    int64_t captured_frames = 0;

    ESP_LOGI(
        kTag,
        "I2S capture start: sample_rate=%d channels=%d seconds=%d codec_init=es8311_adc_init amp=off",
        kSampleRate,
        kChannels,
        kCaptureSeconds);

    while (captured_frames < target_frames) {
        size_t bytes_read = 0;
        result = i2s_channel_read(rx_handle, buffer, sizeof(buffer), &bytes_read, pdMS_TO_TICKS(1000));
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "I2S read failed: %s", esp_err_to_name(result));
            break;
        }
        const size_t samples_read = bytes_read / sizeof(int16_t);
        for (size_t i = 0; i + 1 < samples_read; i += 2) {
            Accumulate(&left, buffer[i]);
            Accumulate(&right, buffer[i + 1]);
        }
        captured_frames += static_cast<int64_t>(samples_read / kChannels);
    }

    LogChannelStats("left", left);
    LogChannelStats("right", right);

    const char* selected = "left";
    if (right.sum_abs > left.sum_abs * 2 && right.peak > left.peak) {
        selected = "right";
    }
    ESP_LOGI(kTag, "selected mono channel candidate: %s", selected);

    if (rx_handle != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(rx_handle));
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(rx_handle));
    }
    return result;
}

esp_err_t RunProbeAndCapture()
{
    SetAudioPower(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_bus_handle_t i2c_bus = nullptr;
    esp_err_t result = InitI2c(&i2c_bus);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "I2C init failed: %s", esp_err_to_name(result));
        SetAudioPower(false);
        return result;
    }

    const esp_err_t probe_result = ProbeEs8311(i2c_bus);
    if (probe_result != ESP_OK) {
        ESP_LOGW(kTag, "continuing with raw I2S capture despite ES8311 probe failure");
    }

    const esp_err_t codec_result = InitEs8311Adc(i2c_bus);
    if (codec_result != ESP_OK) {
        ESP_LOGW(kTag, "continuing with raw I2S capture despite ES8311 ADC init failure");
    }

    result = CaptureI2sStats();

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_del_master_bus(i2c_bus));
    SetAudioPower(false);
    ESP_LOGI(kTag, "audio power off");
    if (result != ESP_OK) {
        return result;
    }
    if (codec_result != ESP_OK) {
        return codec_result;
    }
    return probe_result;
}

}  // namespace

namespace wqn {

esp_err_t RunAudioSelfTestIfEnabled()
{
    ESP_LOGI(kTag, "audio self-test enabled");
    return RunProbeAndCapture();
}

}  // namespace wqn

#else

namespace wqn {

esp_err_t RunAudioSelfTestIfEnabled()
{
    return ESP_OK;
}

}  // namespace wqn

#endif  // CONFIG_WQN_AUDIO_SELFTEST_ENABLE
