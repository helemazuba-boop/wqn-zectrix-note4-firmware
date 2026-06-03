#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"

namespace wqn {

constexpr int kAudioCaptureSampleRate = 16000;
constexpr int kAudioCaptureChannels = 1;
constexpr const char* kAudioCaptureSampleFormat = "s16le";

struct AudioCaptureChunk {
    std::vector<int16_t> samples;
    int duration_ms = 0;
    int16_t peak = 0;
    int rms = 0;
};

esp_err_t StartAudioCapture();
esp_err_t StopAudioCapture(AudioCaptureChunk* chunk);
void ReleaseAudioCapturePower();
bool IsAudioCaptureRunning();

}  // namespace wqn
