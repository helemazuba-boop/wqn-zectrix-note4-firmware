#include "audio_capture.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

#include "esp_check.h"
#include "esp_heap_caps.h"
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
// [dma-footprint] The RX DMA pool is 6 descriptors x this frame count x 4 B.
// 256 frames asked for ~6 KiB of DMA-capable internal memory per capture;
// after the first AI turn fragments the heap (device-observed largest block
// dropping to 4352 B) those allocations fail and every later recording dies
// at init step=i2s. 128 frames keeps ~48 ms of buffering (the read cadence is
// 15 ms) while shrinking each descriptor request to ~0.5 KiB / ~3 KiB total,
// which stays placeable on a fragmented heap.
constexpr uint32_t kI2sDmaFrameNum = 128;
constexpr int kMaxConsecutiveReadTimeouts = 5;
constexpr int kI2sClockWarmupMs = 20;
constexpr int kCaptureInitWaitAttempts = 120;
constexpr int kCaptureInitWaitStepMs = 25;

struct AudioServiceState {
    SemaphoreHandle_t mutex = nullptr;
    bool running = false;
    bool stop_requested = false;
    bool initialized = false;
    bool rx_enabled = false;
    bool audio_powered = false;
    esp_err_t terminal_result = ESP_OK;
    TaskHandle_t task = nullptr;
    // True while the persistent capture worker is parked waiting for a start
    // notification (no live capture session). Mirrors the old "task == nullptr"
    // state that callers used to detect "nothing to stop".
    bool worker_parked = true;
    wqn::services::AudioBusHandle i2c_bus = nullptr;
    wqn::services::AudioChannelHandle rx = nullptr;
    int16_t* capture_buffer = nullptr;
    wqn::AudioCaptureChunk chunk;
    wqn::services::AudioSession session;
    wqn::AudioCaptureTapFn tap_cb = nullptr;
    void* tap_ctx = nullptr;
};

AudioServiceState g_audio;

// [capture-task-reserve] The capture worker's internal stack and TCB are
// pinned here for the process lifetime, mirroring the PSRAM PCM buffer
// reservation. After the first AI turn fragments the internal heap a
// transient xTaskCreate of this size can never succeed again; a
// statically-stored, permanently-parked worker makes capture start
// independent of heap layout.
// 6144 B: device-measured session peak is 4184 B (HWM 4008 incl. codec-retry
// logging bursts); this leaves ~1.9 KiB margin while returning 2 KiB of SRAM
// to the DMA-starved heap versus the original 8192.
constexpr uint32_t kCaptureTaskStackBytes = 6144;
StaticTask_t g_capture_task_tcb = {};
StackType_t g_capture_task_stack[kCaptureTaskStackBytes / sizeof(StackType_t)] = {};

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

esp_err_t EnsureCaptureBuffer()
{
    if (g_audio.capture_buffer != nullptr) {
        return ESP_OK;
    }
    // [ai-memory-fix] The 20 s PCM body is one fixed 640 KiB PSRAM allocation.
    // Keeping it for the process lifetime avoids allocator churn and removes
    // the old vector growth/copy chain that briefly kept several such blocks
    // alive. Never fall back to internal SRAM: task stacks and TLS need it.
    g_audio.capture_buffer = static_cast<int16_t*>(heap_caps_malloc(
        kMaxCaptureSamples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_audio.capture_buffer == nullptr) {
        ESP_LOGE(
            kTag,
            "capture PSRAM allocation failed: bytes=%u free=%u largest=%u internal_free=%u",
            static_cast<unsigned>(kMaxCaptureSamples * sizeof(int16_t)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        kTag,
        "capture PSRAM buffer ready: bytes=%u free=%u largest=%u",
        static_cast<unsigned>(kMaxCaptureSamples * sizeof(int16_t)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
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

esp_err_t InitEs8311Adc(wqn::services::AudioBusHandle bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = wqn::services::ConfigureAudioCodec(
        g_audio.session, wqn::services::AudioCodecProfile::kCapture);
    ESP_LOGI(kTag, "capture init codec: configure ES8311 result=%s (%d)",
             esp_err_to_name(result), static_cast<int>(result));
    return result;
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
        // [dma-attrib] Confirms whether the I2S create/delete cycle returns
        // its DMA pool (a falling dma_free here means the driver leaks).
        ESP_LOGI(kTag,
                 "[dma-attrib] after-i2s-delete dma_free=%u dma_largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
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

// One capture session: init codec/I2S, stream until stop_requested, clean up.
// Runs on the persistent CaptureTask worker; returns when the session is fully
// torn down and the state is published.
void CaptureSession()
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
        ESP_LOGI(kTag, "capture init step=i2s result=%s (%d)",
                 esp_err_to_name(result), static_cast<int>(result));
        // [dma-attrib] Snapshot right after the I2S DMA pool is taken or
        // refused, to separate I2S-cycle deltas from connect/teardown deltas.
        ESP_LOGI(kTag,
                 "[dma-attrib] after-i2s-create dma_free=%u dma_largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
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
        g_audio.worker_parked = true;
        g_audio.terminal_result = terminal_result;
        xSemaphoreGive(g_audio.mutex);
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
        g_audio.worker_parked = true;
        g_audio.terminal_result = terminal_result;
        xSemaphoreGive(g_audio.mutex);
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

        constexpr size_t kMaxMonoFrames = 240;
        int16_t mono_buf[kMaxMonoFrames];
        size_t mono_count = 0;

        const size_t samples_read = bytes_read / sizeof(int16_t);
        for (size_t i = 0; i + 1 < samples_read; i += 2) {
            if (g_audio.chunk.sample_count >= kMaxCaptureSamples) {
                sample_capacity_reached = true;
                break;
            }
            const int left = static_cast<int>(buffer[i]);
            const int right = static_cast<int>(buffer[i + 1]);
            const int16_t sample = buffer[i];
            g_audio.capture_buffer[g_audio.chunk.sample_count++] = sample;
            if (mono_count < kMaxMonoFrames) {
                mono_buf[mono_count++] = sample;
            }
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
        if (mono_count > 0 && g_audio.tap_cb != nullptr) {
            g_audio.tap_cb(mono_buf, mono_count, g_audio.tap_ctx);
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
    if (!g_audio.chunk.empty()) {
        g_audio.chunk.rms = static_cast<int>(IntegerSqrt(sum_squares / g_audio.chunk.sample_count));
        g_audio.chunk.duration_ms = static_cast<int>(
            (g_audio.chunk.sample_count * 1000U) /
            static_cast<size_t>(wqn::kAudioCaptureSampleRate));
    }
    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (read_failed && g_audio.chunk.empty()) {
        g_audio.chunk.duration_ms = 0;
    }
    const int logged_duration_ms = g_audio.chunk.duration_ms;
    const size_t logged_sample_count = g_audio.chunk.sample_count;
    const int16_t logged_peak = g_audio.chunk.peak;
    const int logged_rms = g_audio.chunk.rms;
    // Stack HWM of this session: evidence for any future stack-size decision.
    const unsigned logged_stack_hwm =
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
    g_audio.running = false;
    g_audio.worker_parked = true;
    g_audio.terminal_result = terminal_result;
    xSemaphoreGive(g_audio.mutex);
    ESP_LOGI(
        kTag,
        "capture stop: duration_ms=%d wall_duration_ms=%d mono_samples=%u peak=%d rms=%d left_peak=%d right_peak=%d left_mean_abs=%d right_mean_abs=%d stack_hwm=%u",
        logged_duration_ms,
        wall_capture_duration_ms,
        static_cast<unsigned>(logged_sample_count),
        static_cast<int>(logged_peak),
        logged_rms,
        left_peak,
        right_peak,
        stereo_frames == 0 ? 0 : static_cast<int>(left_abs_sum / static_cast<int64_t>(stereo_frames)),
        stereo_frames == 0 ? 0 : static_cast<int>(right_abs_sum / static_cast<int64_t>(stereo_frames)),
        logged_stack_hwm);
}

void CaptureTask(void*)
{
    for (;;) {
        // Parked between sessions; StartAudioCapture dispatches exactly one
        // capture per notification. The worker never deletes itself, so its
        // statically-stored stack/TCB are reused with no teardown race and no
        // heap churn between turns.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        CaptureSession();
    }
}

}  // namespace

namespace wqn {

esp_err_t InitAudioCaptureBuffer()
{
    return EnsureCaptureBuffer();
}

esp_err_t StartAudioCapture()
{
    ESP_RETURN_ON_ERROR(EnsureAudioService(), kTag, "init audio service");
    ESP_RETURN_ON_ERROR(EnsureCaptureBuffer(), kTag, "allocate capture PSRAM buffer");

    // [dma-footprint] The I2S RX channel allocates its descriptors with
    // MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA. When the DMA-capable sub-pool is
    // exhausted (WiFi dynamic buffers etc.) even a 0.5 KiB request fails while
    // generic internal memory still shows tens of KiB free; log both views at
    // every start so a failing init is attributable immediately.
    ESP_LOGI(kTag,
             "capture heap check: dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    if (g_audio.running) {
        xSemaphoreGive(g_audio.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    // [capture-task-reserve] Create the persistent worker once; its stack and
    // TCB are statically stored so internal-heap fragmentation can never block
    // a recording start again.
    if (g_audio.task == nullptr) {
        TaskHandle_t worker = xTaskCreateStatic(
            CaptureTask, "wqn_audio_cap", kCaptureTaskStackBytes, nullptr, 6,
            g_capture_task_stack, &g_capture_task_tcb);
        if (worker == nullptr) {
            // Unreachable with CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y;
            // kept for symmetry with the previous dynamic-creation rollback.
            g_audio.running = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                wqn::services::EndAudioActivity(&g_audio.session));
            xSemaphoreGive(g_audio.mutex);
            return ESP_ERR_NO_MEM;
        }
        g_audio.task = worker;
    }
    if (!g_audio.worker_parked) {
        // The previous session is still winding down on the worker context.
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
    g_audio.chunk = {};
    g_audio.chunk.samples = g_audio.capture_buffer;
    g_audio.worker_parked = false;
    xSemaphoreGive(g_audio.mutex);

    xTaskNotifyGive(g_audio.task);

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
    if (!g_audio.running && g_audio.worker_parked) {
        const bool has_samples = !g_audio.chunk.empty();
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
    const bool has_samples = stopped && !g_audio.chunk.empty();
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

void SetAudioCaptureTap(AudioCaptureTapFn cb, void* ctx)
{
    if (g_audio.mutex != nullptr) {
        xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
    }
    g_audio.tap_cb = cb;
    g_audio.tap_ctx = ctx;
    if (g_audio.mutex != nullptr) {
        xSemaphoreGive(g_audio.mutex);
    }
}

}  // namespace wqn

#else

namespace wqn {

esp_err_t InitAudioCaptureBuffer()
{
    return ESP_ERR_NOT_SUPPORTED;
}

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
