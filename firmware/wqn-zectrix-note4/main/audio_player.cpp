#include "audio_player.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/audio_service.h"
#include "storage.h"
#include "audio_volume.h"

namespace {

constexpr char kTag[] = "wqn_audio_player";

constexpr int kPlaybackSampleRate = 16000;
constexpr int kPlaybackChannels = 1;
constexpr size_t kPlaybackChunkFrames = 512;

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
    wqn::services::AudioBusHandle i2c_bus = nullptr;
    wqn::services::AudioChannelHandle tx = nullptr;
    wqn::services::AudioSession session;
};

PlayerState g_player;

esp_err_t SetAudioPowerForPlayback(bool enabled)
{
    // [inflight-fix] GPIO42 (codec power) is boot-常通 - do not toggle.
    // Only manage the PA (GPIO46): on for playback, off otherwise.
    const esp_err_t result = wqn::services::SetAudioAmplifier(
        g_player.session, enabled);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "set amplifier=%d failed: %s",
                 enabled ? 1 : 0, esp_err_to_name(result));
    }
    return result;
}

void RecordFirstError(esp_err_t candidate, esp_err_t* result)
{
    if (result != nullptr && *result == ESP_OK && candidate != ESP_OK) {
        *result = candidate;
    }
}

esp_err_t InitI2c(wqn::services::AudioBusHandle* bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*bus != nullptr) {
        return ESP_OK;
    }
    return wqn::services::GetSharedAudioBus(g_player.session, bus);
}

esp_err_t AddCodecDevice(
    wqn::services::AudioBusHandle bus,
    wqn::services::AudioCodecHandle* dev)
{
    if (bus == nullptr || dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::services::AddAudioCodec(g_player.session, bus, dev);
}

esp_err_t WriteCodecReg(
    wqn::services::AudioCodecHandle dev, uint8_t reg, uint8_t value)
{
    return wqn::services::WriteAudioCodecRegister(
        g_player.session, dev, reg, value);
}

esp_err_t ReadCodecReg(
    wqn::services::AudioCodecHandle dev, uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::services::ReadAudioCodecRegister(
        g_player.session, dev, reg, value);
}

esp_err_t InitEs8311Dac(wqn::services::AudioBusHandle bus)
{
    wqn::services::AudioCodecHandle dev = nullptr;
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
    wqn::SetEs8311Volume(
        g_player.session, dev, wqn::GetPlaybackVolumePercent());
    const esp_err_t remove_result =
        wqn::services::RemoveAudioCodec(g_player.session, &dev);
    RecordFirstError(remove_result, &ret);
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 DAC init ok: sample_rate=%d", kPlaybackSampleRate);
    } else {
        ESP_LOGE(kTag, "ES8311 DAC init failed");
    }
    return ret;
}

esp_err_t InitI2sTx(wqn::services::AudioChannelHandle* tx_handle)
{
    if (tx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*tx_handle != nullptr) {
        if (!g_player.tx_enabled) {
            ESP_RETURN_ON_ERROR(
                wqn::services::EnableAudioChannel(g_player.session, *tx_handle),
                kTag, "enable I2S TX");
            g_player.tx_enabled = true;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        wqn::services::CreateAudioTxChannel(
            g_player.session, kPlaybackSampleRate, 256, true, tx_handle),
        kTag, "create I2S TX channel");
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

    const esp_err_t session_result = wqn::services::BeginAudioActivity(
        wqn::services::AudioActivity::kPlayback, &g_player.session);
    if (session_result != ESP_OK) {
        xSemaphoreGive(g_player.mutex);
        return session_result;
    }

    esp_err_t result = SetAudioPowerForPlayback(true);
    g_player.powered = result == ESP_OK;
    if (result == ESP_OK) {
        result = InitI2c(&g_player.i2c_bus);
    }
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
        const esp_err_t power_result = SetAudioPowerForPlayback(false);
        RecordFirstError(power_result, &result);
        if (power_result == ESP_OK) {
            g_player.powered = false;
        }
        const esp_err_t end_result =
            wqn::services::EndAudioActivity(&g_player.session);
        RecordFirstError(end_result, &result);
    }

    xSemaphoreGive(g_player.mutex);
    return result;
}

esp_err_t PlayPcmSamples(const int16_t* samples, size_t count)
{
    if (samples == nullptr || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(InitAudioPlayer(), kTag, "init audio player");

    xSemaphoreTake(g_player.mutex, portMAX_DELAY);
    if (!g_player.initialized || g_player.tx == nullptr) {
        xSemaphoreGive(g_player.mutex);
        StopAudioPlayback();
        return ESP_ERR_INVALID_STATE;
    }

    // Fixed scratch storage keeps playback bounded regardless of caller input.
    // [hw-volume] PCM is sent at 100%; ES8311 registers own volume.
    std::array<int16_t, kPlaybackChunkFrames * 2> stereo = {};
    esp_err_t result = ESP_OK;
    size_t offset = 0;
    while (offset < count) {
        const size_t frames = std::min(kPlaybackChunkFrames, count - offset);
        for (size_t i = 0; i < frames; ++i) {
            const int16_t sample = samples[offset + i];
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;
        }
        size_t bytes_written = 0;
        result = wqn::services::WriteAudioChannel(
            g_player.session, g_player.tx, stereo.data(),
            frames * 2 * sizeof(int16_t), &bytes_written,
            pdMS_TO_TICKS(1000));
        if (result != ESP_OK ||
            bytes_written != frames * 2 * sizeof(int16_t)) {
            if (result == ESP_OK) {
                result = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        offset += frames;
    }

    xSemaphoreGive(g_player.mutex);

    if (result != ESP_OK) {
        ESP_LOGW(kTag, "I2S write failed: %s", esp_err_to_name(result));
    }
    const esp_err_t stop_result = StopAudioPlayback();
    return result == ESP_OK ? stop_result : result;
}

esp_err_t StopAudioPlayback()
{
    if (g_player.mutex == nullptr) {
        return ESP_OK;
    }
    xSemaphoreTake(g_player.mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (g_player.tx != nullptr && g_player.tx_enabled) {
        const esp_err_t disable_result =
            wqn::services::DisableAudioChannel(g_player.session, g_player.tx);
        RecordFirstError(disable_result, &result);
        if (disable_result == ESP_OK) {
            g_player.tx_enabled = false;
        }
    }
    if (g_player.tx != nullptr) {
        const esp_err_t delete_result =
            wqn::services::DeleteAudioChannel(g_player.session, &g_player.tx);
        RecordFirstError(delete_result, &result);
        if (delete_result == ESP_OK) {
            g_player.tx_enabled = false;
        }
    }
    if (g_player.i2c_bus != nullptr) {
        g_player.i2c_bus = nullptr;
    }
    g_player.initialized = false;
    if (g_player.powered) {
        const esp_err_t power_result = SetAudioPowerForPlayback(false);
        RecordFirstError(power_result, &result);
        if (power_result == ESP_OK) {
            g_player.powered = false;
        }
    }
    if (g_player.tx == nullptr && !g_player.powered) {
        RecordFirstError(
            wqn::services::EndAudioActivity(&g_player.session), &result);
    }
    xSemaphoreGive(g_player.mutex);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "audio player stopped");
    } else {
        ESP_LOGE(kTag, "audio player teardown failed; session retained: %s",
                 esp_err_to_name(result));
    }
    return result;
}

bool IsAudioPlayerPlaying()
{
    return services::GetAudioSnapshot().state == services::AudioState::kPlaying;
}

void DeinitAudioPlayer()
{
    StopAudioPlayback();
}

}  // namespace wqn
