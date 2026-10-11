#include "audio_pcm_dump.h"

// Must precede the check below: nothing else in this translation unit has
// pulled sdkconfig.h in yet, so without it the #if silently sees an undefined
// macro and compiles the no-op branch even when the option is enabled.
#include "sdkconfig.h"

#if CONFIG_WQN_AI_PCM_DUMP_ENABLE

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "esp_log.h"

namespace wqn {
namespace {

constexpr char kTag[] = "wqn_pcm_dump";
// 2 s at 16 kHz. At 115200 baud the console moves roughly 11 KiB/s, so an
// uncapped 6 s turn would block the AI worker for ~20 s.
constexpr size_t kMaxDumpSamples = 32000;
// Window search granularity: 250 ms blocks, 8 of them make the 2 s window,
// and 96 blocks cover 24 s of capture. The block table is 768 B on the stack.
constexpr size_t kBlockSamples = 4000;
constexpr size_t kWindowBlocks = 8;
constexpr size_t kMaxBlocks = 96;
constexpr size_t kBytesPerLine = 72;  // -> 96 base64 chars
constexpr size_t kLineChars = (kBytesPerLine / 3) * 4;

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Local encoder rather than mbedtls_base64_encode: this diagnostic must not
// depend on a cipher suite option being enabled.
size_t EncodeChunk(const uint8_t* in, size_t in_len, char* out)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(in[i]) << 16) |
                               ((i + 1 < in_len ? static_cast<uint32_t>(in[i + 1]) : 0U) << 8) |
                               (i + 2 < in_len ? static_cast<uint32_t>(in[i + 2]) : 0U);
        out[o++] = kAlphabet[(chunk >> 18) & 0x3F];
        out[o++] = kAlphabet[(chunk >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? kAlphabet[(chunk >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? kAlphabet[chunk & 0x3F] : '=';
    }
    return o;
}

int WindowRms(const int16_t* samples, size_t count)
{
    if (samples == nullptr || count == 0) {
        return 0;
    }
    double acc = 0.0;
    for (size_t i = 0; i < count; ++i) {
        acc += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return static_cast<int>(std::sqrt(acc / static_cast<double>(count)));
}

// Sample offset of the loudest kMaxDumpSamples window.
//
// The first version of this dump always took the head of the clip, and every
// clip came back as noise floor: these recordings begin with a pause, so the
// head is silence even though the whole-clip rms is 20-30 dB higher. Picking
// the loudest window makes the dump independent of how quickly the speaker
// starts, which is the one thing that cannot be controlled when reproducing a
// whisper by hand.
size_t PickLoudestWindow(const int16_t* samples, size_t count)
{
    if (samples == nullptr || count <= kMaxDumpSamples) {
        return 0;
    }
    const size_t blocks = std::min(count / kBlockSamples, kMaxBlocks);
    if (blocks <= kWindowBlocks) {
        return 0;
    }

    int64_t block_square[kMaxBlocks] = {};
    for (size_t b = 0; b < blocks; ++b) {
        const int16_t* p = samples + b * kBlockSamples;
        int64_t acc = 0;
        for (size_t i = 0; i < kBlockSamples; ++i) {
            acc += static_cast<int64_t>(p[i]) * static_cast<int64_t>(p[i]);
        }
        block_square[b] = acc;
    }

    int64_t running = 0;
    for (size_t b = 0; b < kWindowBlocks; ++b) {
        running += block_square[b];
    }
    int64_t best = running;
    size_t best_block = 0;
    for (size_t b = kWindowBlocks; b < blocks; ++b) {
        running += block_square[b] - block_square[b - kWindowBlocks];
        if (running > best) {
            best = running;
            best_block = b - kWindowBlocks + 1;
        }
    }

    size_t offset = best_block * kBlockSamples;
    if (offset + kMaxDumpSamples > count) {
        offset = count - kMaxDumpSamples;
    }
    return offset;
}

}  // namespace

void DumpCapturedPcmForAnalysis(const int16_t* samples, size_t sample_count,
                                int sample_rate)
{
    if (samples == nullptr || sample_count == 0) {
        ESP_LOGW(kTag, "dump skipped: empty capture");
        return;
    }

    const size_t offset = PickLoudestWindow(samples, sample_count);
    const int16_t* window = samples + offset;
    const size_t count = std::min(sample_count - offset, kMaxDumpSamples);
    const int rate = sample_rate > 0 ? sample_rate : 16000;

    ESP_LOGI(kTag,
             "PCM_DUMP_BEGIN rate=%d channels=1 bits=16 samples=%u "
             "offset_ms=%u window_rms=%d truncated=%d",
             rate,
             static_cast<unsigned>(count),
             static_cast<unsigned>((offset * 1000U) / static_cast<size_t>(rate)),
             WindowRms(window, count),
             sample_count > kMaxDumpSamples ? 1 : 0);

    char line[kLineChars + 1] = {};
    size_t done = 0;
    while (done < count) {
        const size_t take =
            std::min(kBytesPerLine / sizeof(int16_t), count - done);
        const size_t chars = EncodeChunk(
            reinterpret_cast<const uint8_t*>(window + done),
            take * sizeof(int16_t), line);
        line[chars] = '\0';
        ESP_LOGI(kTag, "%s", line);
        done += take;
    }

    ESP_LOGI(kTag, "PCM_DUMP_END samples=%u", static_cast<unsigned>(count));
}

}  // namespace wqn

#else  // !CONFIG_WQN_AI_PCM_DUMP_ENABLE

namespace wqn {

void DumpCapturedPcmForAnalysis(const int16_t* samples, size_t sample_count,
                                int sample_rate)
{
    (void)samples;
    (void)sample_count;
    (void)sample_rate;
}

}  // namespace wqn

#endif  // CONFIG_WQN_AI_PCM_DUMP_ENABLE
