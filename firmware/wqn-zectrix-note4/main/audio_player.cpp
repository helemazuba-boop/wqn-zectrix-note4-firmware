#include "audio_player.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"
#include "runtime/sleep_coordinator.h"
#include "storage.h"
#include "audio_volume.h"

namespace {

constexpr char kTag[] = "wqn_audio_player";

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

constexpr int kPlaybackSampleRate = 16000;
constexpr int kPlaybackChannels = 1;

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
constexpr uint8_t ES8311_DAC_REG37 = 0x37;
constexpr uint8_t ES8311_GPIO_REG44 = 0x44;
constexpr uint8_t ES8311_GP_REG45 = 0x45;

struct PlayerState {
    SemaphoreHandle_t mutex = nullptr;
    bool initialized = false;
    bool tx_enabled = false;
    bool powered = false;
    i2c_master_bus_handle_t i2c_bus = nullptr;
    i2s_chan_handle_t tx = nullptr;
};

PlayerState g_player;

void SetAudioPowerForPlayback(bool enabled)
{
    // [inflight-fix] GPIO42 (codec power) is boot-常通 - do not toggle.
    // Only manage the PA (GPIO46): on for playback, off otherwise.
    gpio_hold_dis(kAudioAmp);
    gpio_set_level(kAudioAmp, enabled ? 1 : 0);
    gpio_hold_en(kAudioAmp);
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
    if (*bus != nullptr) {
        return ESP_OK;
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

esp_err_t InitEs8311Dac(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = nullptr;
    ESP_RETURN_ON_ERROR(AddCodecDevice(bus, &dev), kTag, "add ES8311 device for DAC");

    auto write = [&](uint8_t reg, uint8_t value) -> esp_err_t {
        const esp_err_t ret = WriteCodecReg(dev, reg, value);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "ES8311 DAC write failed: reg=0x%02x val=0x%02x err=%s",
                     reg, value, esp_err_to_name(ret));
        }
        return ret;
    };

    esp_err_t ret = ESP_OK;
    ret |= write(ES8311_GPIO_REG44, 0x08);
    ret |= write(ES8311_GPIO_REG44, 0x08);

    ret |= write(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= write(ES8311_SYSTEM_REG0B, 0x00);
    ret |= write(ES8311_SYSTEM_REG0C, 0x00);
    ret |= write(ES8311_SYSTEM_REG10, 0x1F);
    ret |= write(ES8311_SYSTEM_REG11, 0x7F);
    ret |= write(ES8311_RESET_REG00, 0x80);

    uint8_t reg00 = 0;
    if (ReadCodecReg(dev, ES8311_RESET_REG00, &reg00) == ESP_OK) {
        reg00 &= 0xBF;
        ret |= write(ES8311_RESET_REG00, reg00);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_CLK_MANAGER_REG01, 0x3F);
    uint8_t reg06 = 0;
    if (ReadCodecReg(dev, ES8311_CLK_MANAGER_REG06, &reg06) == ESP_OK) {
        reg06 &= ~0x20;
        ret |= write(ES8311_CLK_MANAGER_REG06, reg06);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_SYSTEM_REG13, 0x10);
    ret |= write(ES8311_GPIO_REG44, 0x58);

    ret |= write(ES8311_DAC_REG37, 0x00);
    ret |= write(ES8311_SYSTEM_REG0D, 0x01);

    ret |= write(ES8311_SYSTEM_REG14, 0x1A);
    uint8_t reg14 = 0;
    if (ReadCodecReg(dev, ES8311_SYSTEM_REG14, &reg14) == ESP_OK) {
        reg14 &= ~0x40;
        ret |= write(ES8311_SYSTEM_REG14, reg14);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_GP_REG45, 0x00);

    ret |= write(ES8311_ADC_REG15, 0x00);

    ret |= write(ES8311_SYSTEM_REG0E, 0x02);
    ret |= write(ES8311_SYSTEM_REG12, 0x00);

    ret |= write(ES8311_DAC_REG37, 0x16);            // [dac-fix] DAC output enable (Gemini 0x08 -> TTS silent; reverted to 0x16)

    uint8_t dac_iface = 0;
    if (ReadCodecReg(dev, ES8311_SDPIN_REG09, &dac_iface) == ESP_OK) {
        dac_iface = (dac_iface & ~0x40) | 0x0C;  // [wordlen-fix] 16bit I2S (bit[4:2]=011, bit[1:0]=00) - reverted from Gemini 0x0D (LJ broke format)
        ret |= write(ES8311_SDPIN_REG09, dac_iface);
    } else {
        ret = ESP_FAIL;
    }
    uint8_t adc_iface = 0;
    if (ReadCodecReg(dev, ES8311_SDPOUT_REG0A, &adc_iface) == ESP_OK) {
        adc_iface = (adc_iface & ~0x40) | 0x0C;  // [wordlen-fix] 16bit I2S (bit[4:2]=011, bit[1:0]=00) - reverted from Gemini 0x0D (LJ broke format)
        ret |= write(ES8311_SDPOUT_REG0A, adc_iface);
    } else {
        ret = ESP_FAIL;
    }

    ret |= write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= write(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= write(ES8311_CLK_MANAGER_REG06, 0x0F);    // bclk_div = 16 (esp_codec_dev official; Gemini 0x03 broke clock - reverted)
    ret |= write(ES8311_CLK_MANAGER_REG07, 0x00);

    ret |= write(ES8311_SYSTEM_REG0B, 0x44);
    ret |= write(ES8311_SYSTEM_REG0C, 0x00);

    // [hw-volume] Apply persisted volume to ES8311 DAC registers (0x32/0x31)
    // before releasing the I2C device handle. Software PCM scaling was removed.
    wqn::SetEs8311Volume(dev, wqn::GetPlaybackVolumePercent());
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(dev));
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 DAC init ok: sample_rate=%d", kPlaybackSampleRate);
    } else {
        ESP_LOGE(kTag, "ES8311 DAC init failed");
    }
    return ret;
}

esp_err_t InitI2sTx(i2s_chan_handle_t* tx_handle)
{
    if (tx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*tx_handle != nullptr) {
        if (!g_player.tx_enabled) {
            ESP_RETURN_ON_ERROR(i2s_channel_enable(*tx_handle), kTag, "enable I2S TX");
            g_player.tx_enabled = true;
        }
        return ESP_OK;
    }

    i2s_chan_config_t channel_config = {};
    channel_config.id = I2S_NUM_0;
    channel_config.role = I2S_ROLE_MASTER;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 256;
    channel_config.auto_clear_after_cb = true;
    channel_config.intr_priority = 0;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, tx_handle, nullptr), kTag, "create I2S TX channel");

    i2s_std_config_t std_config = {};
    std_config.clk_cfg.sample_rate_hz = kPlaybackSampleRate;
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
    std_config.gpio_cfg.dout = kI2sDout;
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*tx_handle, &std_config), kTag, "init I2S TX std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*tx_handle), kTag, "enable I2S TX");
    g_player.tx_enabled = true;
    return ESP_OK;
}

esp_err_t EnsureMutex()
{
    if (g_player.mutex == nullptr) {
        g_player.mutex = xSemaphoreCreateMutex();
        if (g_player.mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t InitAudioPlayer()
{
    ESP_RETURN_ON_ERROR(EnsureMutex(), kTag, "create mutex");

    xSemaphoreTake(g_player.mutex, portMAX_DELAY);
    if (g_player.initialized) {
        xSemaphoreGive(g_player.mutex);
        return ESP_OK;
    }

    SetAudioPowerForPlayback(true);
    g_player.powered = true;

    esp_err_t result = InitI2c(&g_player.i2c_bus);
    if (result == ESP_OK) {
        result = InitEs8311Dac(g_player.i2c_bus);
    }
    if (result == ESP_OK) {
        result = InitI2sTx(&g_player.tx);
    }

    if (result == ESP_OK) {
        g_player.initialized = true;
        ESP_LOGI(kTag, "audio player initialized");
    } else {
        ESP_LOGE(kTag, "audio player init failed: %s", esp_err_to_name(result));
        SetAudioPowerForPlayback(false);
        g_player.powered = false;
    }

    xSemaphoreGive(g_player.mutex);
    return result;
}

esp_err_t PlayPcmSamples(const int16_t* samples, size_t count)
{
    if (samples == nullptr || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wqn::runtime::SleepLease sleep_lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kAudio, "audio-playback", __FILE__, __LINE__);
    if (!sleep_lease) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(InitAudioPlayer(), kTag, "init audio player");

    xSemaphoreTake(g_player.mutex, portMAX_DELAY);
    if (!g_player.initialized || g_player.tx == nullptr) {
        xSemaphoreGive(g_player.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    std::vector<int16_t> stereo(count * 2);
    // [hw-volume] PCM sent at 100% - volume is the ES8311 DAC register
    // (0x32/0x31) set during InitEs8311Dac, not software scaling.
    for (size_t i = 0; i < count; ++i) {
        const int16_t s = samples[i];
        stereo[i * 2] = s;
        stereo[i * 2 + 1] = s;
    }

    size_t bytes_written = 0;
    const esp_err_t result = i2s_channel_write(
        g_player.tx,
        stereo.data(),
        stereo.size() * sizeof(int16_t),
        &bytes_written,
        pdMS_TO_TICKS(1000));

    xSemaphoreGive(g_player.mutex);

    if (result != ESP_OK) {
        ESP_LOGW(kTag, "I2S write failed: %s", esp_err_to_name(result));
    }
    return result;
}

esp_err_t StopAudioPlayback()
{
    if (g_player.mutex == nullptr) {
        return ESP_OK;
    }
    xSemaphoreTake(g_player.mutex, portMAX_DELAY);
    if (g_player.tx != nullptr && g_player.tx_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(g_player.tx));
        g_player.tx_enabled = false;
    }
    if (g_player.tx != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(g_player.tx));
        g_player.tx = nullptr;
    }
    if (g_player.i2c_bus != nullptr) {
        if (g_player.i2c_bus != wqn::GetSharedI2cBusHandle()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_del_master_bus(g_player.i2c_bus));
        }
        g_player.i2c_bus = nullptr;
    }
    g_player.initialized = false;
    if (g_player.powered) {
        SetAudioPowerForPlayback(false);
        g_player.powered = false;
    }
    xSemaphoreGive(g_player.mutex);
    ESP_LOGI(kTag, "audio player stopped");
    return ESP_OK;
}

bool IsAudioPlayerPlaying()
{
    return false;
}

void DeinitAudioPlayer()
{
    StopAudioPlayback();
}

}  // namespace wqn
