#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"

namespace wqn {

esp_err_t InitAudioPlayer();
esp_err_t PlayPcmSamples(const int16_t* samples, size_t count);
esp_err_t StopAudioPlayback();
bool IsAudioPlayerPlaying();
void DeinitAudioPlayer();

}  // namespace wqn
