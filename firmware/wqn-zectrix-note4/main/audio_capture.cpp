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
constexpr uint32_t kI2sDmaFrameNum = 256;
constexpr int kMaxConsecutiveReadTimeouts = 5;
constexpr int kAudioPowerWarmupMs = 250;

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
    if (power_result == ESP_OK) {
        g_audio.audio_powered = true;
    }
    xSemaphoreGive(g_audio.mutex);

    if (power_result != ESP_OK) {
        return power_result;
    }

    if (!was_powered) {
        vTaskDelay(pdMS_TO_TICKS(kAudioPowerWarmupMs));
    }
    return ESP_OK;
}

esp_err_t InitI2c(wqn::services::AudioBusHandle* bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*bus != nullptr) {
        return ESP_OK;
    }
    return wqn::services::GetSharedAudioBus(g_audio.session, bus);
}

esp_err_t AddCodecDevice(
    wqn::services::AudioBusHandle bus,
    wqn::services::AudioCodecHandle* dev)
{
    if (bus == nullptr || dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::services::AddAudioCodec(g_audio.session, bus, dev);
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

    RecordFirstError(
        wqn::services::RemoveAudioCodec(g_audio.session, &dev), &ret);
    return ret;
}

esp_err_t InitI2s(wqn::services::AudioChannelHandle* rx_handle)
{
    if (rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*rx_handle != nullptr) {
        if (!g_audio.rx_enabled) {
            ESP_RETURN_ON_ERROR(
                wqn::services::EnableAudioChannel(g_audio.session, *rx_handle),
                kTag, "enable I2S RX");
            g_audio.rx_enabled = true;
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        wqn::services::CreateAudioRxChannel(
            g_audio.session, wqn::kAudioCaptureSampleRate,
            kI2sDmaFrameNum, true, rx_handle),
        kTag, "create I2S RX channel");
    g_audio.rx_enabled = true;
    return ESP_OK;
}

esp_err_t CleanupCaptureHardware(bool keep_power)
{
    esp_err_t result = ESP_OK;
    if (g_audio.rx != nullptr && g_audio.rx_enabled) {
        const esp_err_t disable_result =
            wqn::services::DisableAudioChannel(g_audio.session, g_audio.rx);
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
        RecordFirstError(delete_result, &result);
        if (delete_result == ESP_OK) {
            g_audio.rx_enabled = false;
        }
    }
    if (!keep_power && g_audio.i2c_bus != nullptr) {
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
    }
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
        esp_err_t terminal_result = result;
        RecordFirstError(
            CleanupCaptureHardware(false), &terminal_result);
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
        if (sample_capacity_reached) {
            ESP_LOGI(kTag, "capture reached fixed sample capacity: %u",
                     static_cast<unsigned>(kMaxCaptureSamples));
            break;
        }
    }

    esp_err_t terminal_result = read_failed ? result : ESP_OK;
    RecordFirstError(CleanupCaptureHardware(true), &terminal_result);
    if (g_audio.rx == nullptr) {
        RecordFirstError(
            wqn::services::EndAudioActivity(&g_audio.session),
            &terminal_result);
    }
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
    g_audio.terminal_result = terminal_result;
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
    return ESP_OK;
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
