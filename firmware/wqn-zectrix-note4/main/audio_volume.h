#pragma once

#include "services/audio_service.h"

namespace wqn {

// [hw-volume] Set ES8311 DAC output volume via hardware registers (0x32 volume,
// 0x31 mute). Replaces software PCM scaling, which ruined SNR (the DAC noise
// floor stayed constant while signal shrank), introduced quantization
// distortion at low levels (effective bit-depth dropped), and felt non-linear
// to the ear (linear scaling vs logarithmic perception).
//
//   percent 0     -> mute (0x31 bits6:5 = 1)
//   percent 1-100 -> 0x32 log-scaled: 0x00 = -95.5 dB, 0xBF = 0 dB,
//                    step +0.5 dB. Values above 0xBF add gain and are not used.
//
// Call after ES8311 DAC init, before the I2C device handle is released.
void SetEs8311Volume(
    const services::AudioSession& session,
    services::AudioCodecHandle dev,
    int percent);

}  // namespace wqn
