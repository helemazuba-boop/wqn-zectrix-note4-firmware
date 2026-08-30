#include "audio_selftest.h"

#if CONFIG_WQN_AI_AUDIO_SELFTEST_ENABLE

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/audio_service.h"

namespace {

constexpr char kTag[] = "wqn_audio_selftest";
wqn::services::AudioSession g_selftest_session;

constexpr int kSampleRate = 24000;
constexpr int kChannels = 2;
constexpr int kCaptureSeconds = 2;
constexpr size_t kReadFrames = 360;
constexpr size_t kReadSamples = kReadFrames * kChannels;
constexpr TickType_t kI2sClockWarmup = pdMS_TO_TICKS(20);

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

esp_err_t SetAudioPower(bool /*enabled*/)
{
    // [inflight-fix] GPIO42 (codec power) is boot-常通 - do not toggle.
    // Only manage the PA (GPIO46): off (selftest keeps amp off).
    const esp_err_t result = wqn::services::SetAudioAmplifier(
        g_selftest_session, false);
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

esp_err_t InitI2c(wqn::services::AudioBusHandle* bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::services::GetSharedAudioBus(g_selftest_session, bus);
}

esp_err_t ProbeEs8311(wqn::services::AudioBusHandle bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    wqn::services::AudioCodecHandle dev = nullptr;
    ESP_RETURN_ON_ERROR(
        wqn::services::AddAudioCodec(g_selftest_session, bus, &dev),
        kTag, "add ES8311 I2C device");

    uint8_t reg = 0x00;
    uint8_t value = 0;
    esp_err_t result = wqn::services::ReadAudioCodecRegister(
        g_selftest_session, dev, reg, &value);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 I2C probe ok: reg00=0x%02x", value);
    } else {
        ESP_LOGW(kTag, "ES8311 I2C probe failed: %s", esp_err_to_name(result));
    }

    RecordFirstError(
        wqn::services::RemoveAudioCodec(g_selftest_session, &dev), &result);
    return result;
}

esp_err_t InitEs8311Adc(wqn::services::AudioBusHandle bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = wqn::services::ConfigureAudioCodec(
        g_selftest_session, wqn::services::AudioCodecProfile::kCapture);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 ADC init ok: input_gain=30db sample_rate=%d", kSampleRate);
    } else {
        ESP_LOGE(kTag, "ES8311 ADC init failed: %s",
                 esp_err_to_name(result));
    }
    return result;
}

esp_err_t InitI2s(wqn::services::AudioChannelHandle* rx_handle)
{
    if (rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    return wqn::services::CreateAudioRxChannel(
        g_selftest_session, kSampleRate, 240, false, rx_handle);
}

esp_err_t CaptureI2sStats(
    wqn::services::AudioChannelHandle* rx_handle)
{
    if (rx_handle == nullptr || *rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;

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
        result = wqn::services::ReadAudioChannel(
            g_selftest_session, *rx_handle, buffer, sizeof(buffer),
            &bytes_read, pdMS_TO_TICKS(1000));
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

    if (*rx_handle != nullptr) {
        RecordFirstError(
            wqn::services::DisableAudioChannel(
                g_selftest_session, *rx_handle),
            &result);
        RecordFirstError(
            wqn::services::DeleteAudioChannel(
                g_selftest_session, rx_handle),
            &result);
    }
    return result;
}

esp_err_t RunProbeAndCapture()
{
    ESP_RETURN_ON_ERROR(SetAudioPower(true), kTag, "disable self-test PA");
    vTaskDelay(pdMS_TO_TICKS(100));

    wqn::services::AudioBusHandle i2c_bus = nullptr;
    esp_err_t result = InitI2c(&i2c_bus);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "I2C init failed: %s", esp_err_to_name(result));
        ESP_ERROR_CHECK_WITHOUT_ABORT(SetAudioPower(false));
        return result;
    }

    const esp_err_t probe_result = ProbeEs8311(i2c_bus);
    if (probe_result != ESP_OK) {
        ESP_LOGW(kTag, "continuing with raw I2S capture despite ES8311 probe failure");
    }

    wqn::services::AudioChannelHandle rx_handle = nullptr;
    result = InitI2s(&rx_handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "I2S init failed: %s", esp_err_to_name(result));
        ESP_ERROR_CHECK_WITHOUT_ABORT(SetAudioPower(false));
        return result;
    }
    vTaskDelay(kI2sClockWarmup);

    const esp_err_t codec_result = InitEs8311Adc(i2c_bus);
    if (codec_result != ESP_OK) {
        ESP_LOGW(kTag, "continuing with raw I2S capture despite ES8311 ADC init failure");
    }

    result = CaptureI2sStats(&rx_handle);

    // The board owns the shared bus for the process lifetime.
    RecordFirstError(SetAudioPower(false), &result);
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
    const esp_err_t begin_result = services::BeginAudioActivity(
        services::AudioActivity::kSelfTest, &g_selftest_session);
    if (begin_result != ESP_OK) {
        return begin_result;
    }
    const esp_err_t result = RunProbeAndCapture();
    const esp_err_t end_result = services::EndAudioActivity(&g_selftest_session);
    return result == ESP_OK ? end_result : result;
}

}  // namespace wqn

#else

namespace wqn {

esp_err_t RunAudioSelfTestIfEnabled()
{
    return ESP_OK;
}

}  // namespace wqn

#endif  // CONFIG_WQN_AI_AUDIO_SELFTEST_ENABLE
