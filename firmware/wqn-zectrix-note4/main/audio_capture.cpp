#include "audio_capture.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/audio_service.h"

namespace {

constexpr char kTag[] = "wqn_audio_capture";

constexpr int kStereoChannels = 2;
constexpr int kMaxCaptureMs = 20000;
constexpr size_t kMaxCaptureSamples =
    static_cast<size_t>(wqn::kAudioCaptureSampleRate) *
    (kMaxCaptureMs / 1000);
constexpr size_t kReadFrames = 240;
constexpr size_t kReadSamples = kReadFrames * kStereoChannels;
constexpr int kAdcWarmupReadCount = 4;
constexpr uint32_t kI2sDmaFrameNum = 256;
constexpr int kMaxConsecutiveReadTimeouts = 5;
constexpr int kI2sClockWarmupMs = 5;
constexpr int kCodecResetSettleMs = 20;
constexpr int kCaptureInitWaitAttempts = 120;
constexpr int kCaptureInitWaitStepMs = 25;

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
    esp_err_t terminal_result = ESP_OK;
    TaskHandle_t task = nullptr;
    wqn::services::AudioBusHandle i2c_bus = nullptr;
    wqn::services::AudioChannelHandle rx = nullptr;
    wqn::AudioCaptureChunk chunk;
    wqn::services::AudioSession session;
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

esp_err_t SetAudioPowerUnlocked(bool /*enabled*/)
{
    // [inflight-fix] GPIO42 (codec power) is boot-常通 - do not toggle here
    // (was causing pop + cold-start recording garbage). Only manage the PA
    // (GPIO46): off during capture to avoid feedback.
    const esp_err_t result = wqn::services::SetAudioAmplifier(
        g_audio.session, false);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "disable amplifier failed: %s", esp_err_to_name(result));
    }
    return result;
}

void RecordFirstError(esp_err_t candidate, esp_err_t* result)
{
    if (result != nullptr && *result == ESP_OK && candidate != ESP_OK) {
        *result = candidate;
    }
}

esp_err_t EnsureAudioService()
{
    if (g_audio.mutex == nullptr) {
        g_audio.mutex = xSemaphoreCreateMutex();
        if (g_audio.mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t PrepareAudioPowerForCapture()
{
    ESP_RETURN_ON_ERROR(EnsureAudioService(), kTag, "create audio service");

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    const bool was_powered = g_audio.audio_powered;
    const esp_err_t power_result = SetAudioPowerUnlocked(true);
    ESP_LOGI(kTag, "capture init power: session=%lu was_powered=%d result=%s (%d)",
             static_cast<unsigned long>(g_audio.session.id),
             was_powered ? 1 : 0,
             esp_err_to_name(power_result),
             static_cast<int>(power_result));
    if (power_result == ESP_OK) {
        g_audio.audio_powered = true;
    }
    xSemaphoreGive(g_audio.mutex);

    if (power_result != ESP_OK) {
        return power_result;
    }

    // A failed init leaves ES8311 partially configured while GPIO42 remains
    // physically high. Recover the rail through AudioService when the logical
    // state says the previous turn did not leave a usable codec.
    if (!was_powered) {
        const esp_err_t recovery_result =
            wqn::services::RecoverAudioCodec(g_audio.session);
        ESP_LOGI(kTag, "capture init codec recovery: result=%s (%d)",
                 esp_err_to_name(recovery_result),
                 static_cast<int>(recovery_result));
        if (recovery_result != ESP_OK) {
            xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
            g_audio.audio_powered = false;
            xSemaphoreGive(g_audio.mutex);
            return recovery_result;
        }
    }
    return ESP_OK;
}

esp_err_t InitI2c(wqn::services::AudioBusHandle* bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // AudioBusHandle is a borrowed handle owned by the board/power service.
    // Refresh it for every capture instead of trusting a pointer retained from
    // the previous turn in case the shared bus was recreated meanwhile.
    const wqn::services::AudioBusHandle previous = *bus;
    const esp_err_t result =
        wqn::services::GetSharedAudioBus(g_audio.session, bus);
    ESP_LOGI(kTag,
             "capture init I2C: refresh shared bus previous=%p result=%s (%d) bus=%p",
             previous, esp_err_to_name(result), static_cast<int>(result), *bus);
    return result;
}

esp_err_t AddCodecDevice(
    wqn::services::AudioBusHandle bus,
    wqn::services::AudioCodecHandle* dev)
{
    if (bus == nullptr || dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result =
        wqn::services::AddAudioCodec(g_audio.session, bus, dev);
    ESP_LOGI(kTag, "capture init codec: add ES8311 result=%s (%d) dev=%p",
             esp_err_to_name(result), static_cast<int>(result), *dev);
    return result;
}

esp_err_t WriteCodecReg(
    wqn::services::AudioCodecHandle dev, uint8_t reg, uint8_t value)
{
    return wqn::services::WriteAudioCodecRegister(
        g_audio.session, dev, reg, value);
}

esp_err_t ReadCodecReg(
    wqn::services::AudioCodecHandle dev, uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::services::ReadAudioCodecRegister(
        g_audio.session, dev, reg, value);
}

esp_err_t InitEs8311Adc(wqn::services::AudioBusHandle bus)
{
    wqn::services::AudioCodecHandle dev = nullptr;
    ESP_RETURN_ON_ERROR(AddCodecDevice(bus, &dev), kTag, "add ES8311 device");

    esp_err_t first_error = ESP_OK;
    auto write = [&](uint8_t reg, uint8_t value) -> esp_err_t {
        if (first_error != ESP_OK) {
            return first_error;
        }
        const esp_err_t result = WriteCodecReg(dev, reg, value);
        if (result != ESP_OK) {
            first_error = result;
            ESP_LOGE(kTag, "ES8311 write failed: reg=0x%02x value=0x%02x result=%s (%d)",
                     reg, value, esp_err_to_name(result), static_cast<int>(result));
        }
        return result;
    };
    auto read = [&](uint8_t reg, uint8_t* value) -> esp_err_t {
        if (first_error != ESP_OK) {
            return first_error;
        }
        const esp_err_t result = ReadCodecReg(dev, reg, value);
        if (result != ESP_OK) {
            first_error = result;
            ESP_LOGE(kTag, "ES8311 read failed: reg=0x%02x result=%s (%d)",
                     reg, esp_err_to_name(result), static_cast<int>(result));
        }
        return result;
    };

    esp_err_t ret = ESP_OK;
    // Espressif's ES8311 stability fix deliberately writes REG44 twice at the
    // start of every open. The first write enables the codec-side I2C noise
    // filter; the second confirms the setting after the filter takes effect.
    // Keep the normal codec sequence intact: the only software reset is after
    // the clock/system preamble, as in the verified playback and flash paths.
    ret |= write(ES8311_GPIO_REG44, 0x08);
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
    const esp_err_t sequence_reset_result = write(ES8311_RESET_REG00, 0x80);
    ret |= sequence_reset_result;
    if (sequence_reset_result == ESP_OK) {
        // ES8311 briefly NACKs transactions while its software reset settles.
        // Wait before the read/modify/write operations that follow reset.
        vTaskDelay(pdMS_TO_TICKS(kCodecResetSettleMs));
    }

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
        ret |= write(ES8311_SDPOUT_REG0A, (reg & ~0x40) | 0x0C);  // [wordlen-fix] 16bit WL matches I2S 16bit
    } else {
        ret = ESP_FAIL;
    }
    if (read(ES8311_SDPIN_REG09, &reg) == ESP_OK) {
        ret |= write(ES8311_SDPIN_REG09, (reg & ~0x40) | 0x0C);  // [wordlen-fix] 16bit WL matches I2S 16bit
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

    if (first_error != ESP_OK) {
        // Preserve the transport error instead of converting a NACK into the
        // generic ESP_FAIL produced by skipped read/modify/write operations.
        ret = first_error;
    }
    RecordFirstError(
        wqn::services::RemoveAudioCodec(g_audio.session, &dev), &ret);
    ESP_LOGI(kTag, "capture init codec: configure ES8311 result=%s (%d)",
             esp_err_to_name(ret), static_cast<int>(ret));
    return ret;
}

esp_err_t InitI2s(wqn::services::AudioChannelHandle* rx_handle)
{
    if (rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*rx_handle != nullptr) {
        ESP_LOGI(kTag, "capture init I2S: existing RX=%p enabled=%d",
                 *rx_handle, g_audio.rx_enabled ? 1 : 0);
        if (!g_audio.rx_enabled) {
            const esp_err_t result = wqn::services::EnableAudioChannel(
                g_audio.session, *rx_handle);
            ESP_LOGI(kTag, "capture init I2S: enable RX result=%s (%d)",
                     esp_err_to_name(result), static_cast<int>(result));
            ESP_RETURN_ON_ERROR(result, kTag, "enable I2S RX");
            g_audio.rx_enabled = true;
        }
        return ESP_OK;
    }
    const esp_err_t result = wqn::services::CreateAudioRxChannel(
        g_audio.session, wqn::kAudioCaptureSampleRate,
        kI2sDmaFrameNum, true, rx_handle);
    ESP_LOGI(kTag, "capture init I2S: create RX result=%s (%d) RX=%p",
             esp_err_to_name(result), static_cast<int>(result), *rx_handle);
    ESP_RETURN_ON_ERROR(result, kTag, "create I2S RX channel");
    g_audio.rx_enabled = true;
    return ESP_OK;
}

esp_err_t CleanupCaptureHardware(bool keep_power)
{
    esp_err_t result = ESP_OK;
    ESP_LOGI(kTag,
             "capture cleanup begin: keep_power=%d session=%lu bus=%p rx=%p rx_enabled=%d",
             keep_power ? 1 : 0,
             static_cast<unsigned long>(g_audio.session.id),
             g_audio.i2c_bus, g_audio.rx, g_audio.rx_enabled ? 1 : 0);
    if (g_audio.rx != nullptr && g_audio.rx_enabled) {
        const esp_err_t disable_result =
            wqn::services::DisableAudioChannel(g_audio.session, g_audio.rx);
        ESP_LOGI(kTag, "capture cleanup disable RX: result=%s (%d)",
                 esp_err_to_name(disable_result), static_cast<int>(disable_result));
        RecordFirstError(disable_result, &result);
        if (disable_result == ESP_OK) {
            g_audio.rx_enabled = false;
        }
    }
    // [i2s-handoff] Always delete the RX channel (not just disable) so the
    // I2S_NUM_0 RX slot is released for Flash's duplex channels. A disabled
    // channel still holds the port slot (i2s_new_channel returns
    // ESP_ERR_NOT_FOUND/"no available channel"), which is what blocked Flash
    // after a STD recording. Mirrors audio_player.cpp StopAudioPlayback (which
    // deletes g_player.tx). InitI2s recreates the channel on next start.
    if (g_audio.rx != nullptr) {
        const esp_err_t delete_result =
            wqn::services::DeleteAudioChannel(g_audio.session, &g_audio.rx);
        ESP_LOGI(kTag, "capture cleanup delete RX: result=%s (%d) rx=%p",
                 esp_err_to_name(delete_result), static_cast<int>(delete_result),
                 g_audio.rx);
        RecordFirstError(delete_result, &result);
        if (delete_result == ESP_OK) {
            g_audio.rx_enabled = false;
        }
    }
    if (g_audio.i2c_bus != nullptr) {
        // The bus is owned by power_manager and must not be deleted here.
        // Drop this borrowed pointer on every turn so the next init refreshes
        // it from GetSharedI2cBusHandle().
        g_audio.i2c_bus = nullptr;
    }
    if (!keep_power) {
        if (EnsureAudioService() != ESP_OK) {
            RecordFirstError(SetAudioPowerUnlocked(false), &result);
            return result;
        }
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
        const esp_err_t power_result = SetAudioPowerUnlocked(false);
        RecordFirstError(power_result, &result);
        if (power_result == ESP_OK) {
            g_audio.audio_powered = false;
        }
        xSemaphoreGive(g_audio.mutex);
        ESP_LOGI(kTag, "capture cleanup power off: result=%s (%d)",
                 esp_err_to_name(power_result), static_cast<int>(power_result));
    }
    ESP_LOGI(kTag, "capture cleanup end: result=%s (%d) bus=%p rx=%p",
             esp_err_to_name(result), static_cast<int>(result),
             g_audio.i2c_bus, g_audio.rx);
    return result;
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
    const auto snapshot = wqn::services::GetAudioSnapshot();
    ESP_LOGI(kTag,
             "capture init begin: session=%lu service_session=%lu state=%s bus=%p rx=%p rx_enabled=%d",
             static_cast<unsigned long>(g_audio.session.id),
             static_cast<unsigned long>(snapshot.session_id),
             wqn::services::AudioStateName(snapshot.state),
             g_audio.i2c_bus,
             g_audio.rx,
             g_audio.rx_enabled ? 1 : 0);
    esp_err_t result = PrepareAudioPowerForCapture();
    ESP_LOGI(kTag, "capture init step=power result=%s (%d)",
             esp_err_to_name(result), static_cast<int>(result));
    if (result == ESP_OK) {
        result = InitI2c(&g_audio.i2c_bus);
        ESP_LOGI(kTag, "capture init step=i2c result=%s (%d)",
                 esp_err_to_name(result), static_cast<int>(result));
    }
    if (result == ESP_OK) {
        result = InitI2s(&g_audio.rx);
        ESP_LOGI(kTag, "capture init step=i2s result=%s (%d) RX=%p",
                 esp_err_to_name(result), static_cast<int>(result), g_audio.rx);
    }
    if (result == ESP_OK) {
        // REG01..REG03 select the external audio clock path. Keep MCLK/BCLK/WS
        // running before those writes; without them this board ACKs the first
        // few registers and then consistently NACKs at REG16.
        vTaskDelay(pdMS_TO_TICKS(kI2sClockWarmupMs));
        result = InitEs8311Adc(g_audio.i2c_bus);
        ESP_LOGI(kTag, "capture init step=es8311 result=%s (%d)",
                 esp_err_to_name(result), static_cast<int>(result));
    }

    if (result != ESP_OK) {
        ESP_LOGE(kTag,
                 "audio capture init failed: %s (%d), session=%lu bus=%p rx=%p rx_enabled=%d",
                 esp_err_to_name(result), static_cast<int>(result),
                 static_cast<unsigned long>(g_audio.session.id),
                 g_audio.i2c_bus, g_audio.rx, g_audio.rx_enabled ? 1 : 0);
        esp_err_t terminal_result = result;
        const esp_err_t cleanup_result = CleanupCaptureHardware(false);
        ESP_LOGI(kTag, "capture init cleanup result=%s (%d), bus=%p rx=%p",
                 esp_err_to_name(cleanup_result), static_cast<int>(cleanup_result),
                 g_audio.i2c_bus, g_audio.rx);
        RecordFirstError(cleanup_result, &terminal_result);
        if (g_audio.rx == nullptr) {
            const esp_err_t end_result =
                wqn::services::EndAudioActivity(&g_audio.session);
            ESP_LOGI(kTag, "capture init end activity result=%s (%d)",
                     esp_err_to_name(end_result), static_cast<int>(end_result));
            RecordFirstError(end_result, &terminal_result);
        }
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
        g_audio.running = false;
        g_audio.task = nullptr;
        g_audio.terminal_result = terminal_result;
        xSemaphoreGive(g_audio.mutex);
        vTaskDelete(nullptr);
        return;
    }

    int16_t buffer[kReadSamples] = {};
    size_t warmup_bytes = 0;
    for (int warmup_read = 0; warmup_read < kAdcWarmupReadCount; ++warmup_read) {
        size_t bytes_read = 0;
        const esp_err_t warmup_result = wqn::services::ReadAudioChannel(
            g_audio.session, g_audio.rx, buffer, sizeof(buffer),
            &bytes_read, pdMS_TO_TICKS(1000));
        if (warmup_result != ESP_OK) {
            ESP_LOGW(kTag,
                     "ADC warmup discard failed: read=%d/%d result=%s (%d)",
                     warmup_read + 1, kAdcWarmupReadCount,
                     esp_err_to_name(warmup_result),
                     static_cast<int>(warmup_result));
            result = warmup_result;
            break;
        }
        warmup_bytes += bytes_read;
    }
    if (result != ESP_OK) {
        esp_err_t terminal_result = result;
        RecordFirstError(CleanupCaptureHardware(false), &terminal_result);
        if (g_audio.rx == nullptr) {
            RecordFirstError(
                wqn::services::EndAudioActivity(&g_audio.session),
                &terminal_result);
        }
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
        g_audio.running = false;
        g_audio.task = nullptr;
        g_audio.terminal_result = terminal_result;
        xSemaphoreGive(g_audio.mutex);
        vTaskDelete(nullptr);
        return;
    }
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    g_audio.initialized = true;
    xSemaphoreGive(g_audio.mutex);
    ESP_LOGI(kTag,
             "capture start: 16kHz s16le mono from ES8311 left channel; ADC warmup discarded reads=%d bytes=%u",
             kAdcWarmupReadCount, static_cast<unsigned>(warmup_bytes));
    const int64_t start_us = esp_timer_get_time();
    int64_t sum_squares = 0;
    int64_t left_abs_sum = 0;
    int64_t right_abs_sum = 0;
    int left_peak = 0;
    int right_peak = 0;
    size_t stereo_frames = 0;
    int consecutive_timeouts = 0;
    bool read_failed = false;
    bool sample_capacity_reached = false;
    while (!StopRequested()) {
        const int elapsed_ms = static_cast<int>((esp_timer_get_time() - start_us) / 1000);
        if (elapsed_ms >= kMaxCaptureMs) {
            ESP_LOGW(kTag, "capture auto-stop at max duration: %dms", elapsed_ms);
            break;
        }

        size_t bytes_read = 0;
        result = wqn::services::ReadAudioChannel(
            g_audio.session, g_audio.rx, buffer, sizeof(buffer),
            &bytes_read, pdMS_TO_TICKS(1000));
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
            if (g_audio.chunk.samples.size() >= kMaxCaptureSamples) {
                sample_capacity_reached = true;
                break;
            }
            const int left = static_cast<int>(buffer[i]);
            const int right = static_cast<int>(buffer[i + 1]);
            const int16_t sample = buffer[i];
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
        if (sample_capacity_reached) {
            ESP_LOGI(kTag, "capture reached fixed sample capacity: %u",
                     static_cast<unsigned>(kMaxCaptureSamples));
            break;
        }
    }
    const int wall_capture_duration_ms =
        static_cast<int>((esp_timer_get_time() - start_us) / 1000);

    esp_err_t terminal_result = read_failed ? result : ESP_OK;
    RecordFirstError(CleanupCaptureHardware(true), &terminal_result);
    if (g_audio.rx == nullptr) {
        RecordFirstError(
            wqn::services::EndAudioActivity(&g_audio.session),
            &terminal_result);
    }
    if (!g_audio.chunk.samples.empty()) {
        g_audio.chunk.rms = static_cast<int>(IntegerSqrt(sum_squares / g_audio.chunk.samples.size()));
        g_audio.chunk.duration_ms = static_cast<int>(
            (g_audio.chunk.samples.size() * 1000U) /
            static_cast<size_t>(wqn::kAudioCaptureSampleRate));
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
    g_audio.terminal_result = terminal_result;
    xSemaphoreGive(g_audio.mutex);
    ESP_LOGI(
        kTag,
        "capture stop: duration_ms=%d wall_duration_ms=%d mono_samples=%u peak=%d rms=%d left_peak=%d right_peak=%d left_mean_abs=%d right_mean_abs=%d",
        logged_duration_ms,
        wall_capture_duration_ms,
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
    next_chunk.samples.reserve(kMaxCaptureSamples);

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (g_audio.running) {
        xSemaphoreGive(g_audio.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t session_result = wqn::services::BeginAudioActivity(
        wqn::services::AudioActivity::kCapture, &g_audio.session);
    if (session_result != ESP_OK) {
        xSemaphoreGive(g_audio.mutex);
        return session_result;
    }
    g_audio.running = true;
    g_audio.stop_requested = false;
    g_audio.initialized = false;
    g_audio.terminal_result = ESP_OK;
    g_audio.chunk = std::move(next_chunk);
    xSemaphoreGive(g_audio.mutex);

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreate(CaptureTask, "wqn_audio_cap", 8192, nullptr, 6, &task);
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (created != pdPASS) {
        g_audio.running = false;
        g_audio.task = nullptr;
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            wqn::services::EndAudioActivity(&g_audio.session));
        xSemaphoreGive(g_audio.mutex);
        return ESP_ERR_NO_MEM;
    }
    g_audio.task = g_audio.running ? task : nullptr;
    xSemaphoreGive(g_audio.mutex);

    // Do not report "recording started" to AiSession until the asynchronous
    // capture task has actually completed codec + I2S initialization. This
    // also surfaces terminal init failures immediately instead of leaving the
    // UI in kListening until the user releases the button.
    for (int i = 0; i < kCaptureInitWaitAttempts; ++i) {
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
        const bool initialized = g_audio.initialized;
        const bool running = g_audio.running;
        const esp_err_t terminal_result = g_audio.terminal_result;
        xSemaphoreGive(g_audio.mutex);
        if (initialized) {
            return ESP_OK;
        }
        if (!running) {
            return terminal_result == ESP_OK ? ESP_FAIL : terminal_result;
        }
        vTaskDelay(pdMS_TO_TICKS(kCaptureInitWaitStepMs));
    }

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (g_audio.running) {
        g_audio.stop_requested = true;
    }
    xSemaphoreGive(g_audio.mutex);
    ESP_LOGE(kTag, "audio capture init handshake timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t StopAudioCapture(AudioCaptureChunk* chunk)
{
    ESP_RETURN_ON_ERROR(EnsureAudioService(), kTag, "init audio service");
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (!g_audio.running && g_audio.task == nullptr) {
        const bool has_samples = !g_audio.chunk.samples.empty();
        const esp_err_t terminal_result = g_audio.terminal_result;
        if (chunk != nullptr) {
            *chunk = g_audio.chunk;
        }
        xSemaphoreGive(g_audio.mutex);
        // [handshake-guard] No active capture: CaptureTask already exited.
        // If the chunk is empty this is a stale-listening state (CaptureTask
        // init failed but AI status stayed kListening) - returning ESP_OK
        // would submit a 0-duration clip as success. Surface it as an error
        // so the caller reports 录音太短/失败 instead of a silent 0/0 submit.
        if (terminal_result != ESP_OK) {
            return terminal_result;
        }
        return has_samples ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    const esp_err_t terminal_result = g_audio.terminal_result;
    if (stopped && chunk != nullptr) {
        *chunk = g_audio.chunk;
    }
    xSemaphoreGive(g_audio.mutex);
    if (!stopped) {
        return ESP_ERR_TIMEOUT;
    }
    if (terminal_result != ESP_OK) {
        return terminal_result;
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
    // GPIO42 remains warm during runtime and capture already keeps the PA
    // disabled. Do not arm a stale timer: it could otherwise fire after Flash
    // acquires the next audio session and cut that session's amplifier.
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
