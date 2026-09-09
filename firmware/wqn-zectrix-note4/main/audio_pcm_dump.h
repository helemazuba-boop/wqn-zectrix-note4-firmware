#pragma once

#include <cstddef>
#include <cstdint>

namespace wqn {

// [diag] Emit a captured turn's PCM over the console as base64 so a clip can
// be replayed to the ASR provider at different gains offline.
//
// This exists to answer one question: when a whisper loses most of its words,
// is that because the clip reaches the provider ~20 dB quieter than normal
// speech (fixable on the device with gain/AGC), or because the model simply
// does not transcribe whispered speech (not fixable here)? Device logs only
// carry rms/peak, so the clip itself has to leave the board somehow, and the
// console is the only channel that needs no new plumbing.
//
// Compiled out unless CONFIG_WQN_AI_PCM_DUMP_ENABLE is set. When enabled the
// dump is capped by kMaxDumpSamples: at 115200 baud the console moves roughly
// 11 KiB/s, so an uncapped 6 s turn would block the AI worker for ~20 s.
void DumpCapturedPcmForAnalysis(const int16_t* samples, size_t sample_count,
                                int sample_rate);

}  // namespace wqn
