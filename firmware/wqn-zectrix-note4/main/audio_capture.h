#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace wqn {

constexpr int kAudioCaptureSampleRate = 16000;
constexpr int kAudioCaptureChannels = 1;
constexpr const char* kAudioCaptureSampleFormat = "s16le";

struct AudioCaptureChunk {
    // Non-owning view into AudioCapture's fixed PSRAM buffer. It remains valid
    // until the next successful StartAudioCapture() call. AiSession serializes
    // capture and upload, so the synchronous uploader consumes it before then.
    const int16_t* samples = nullptr;
    size_t sample_count = 0;
    int duration_ms = 0;
    int16_t peak = 0;
    int rms = 0;

    bool empty() const { return samples == nullptr || sample_count == 0; }
};

// Reserves the fixed 20-second PCM buffer in PSRAM. Called during AI service
// boot so normal runtime heap churn cannot fragment away the required block.
esp_err_t InitAudioCaptureBuffer();
esp_err_t StartAudioCapture();
esp_err_t StopAudioCapture(AudioCaptureChunk* chunk);
void ReleaseAudioCapturePower();
bool IsAudioCaptureRunning();

}  // namespace wqn
