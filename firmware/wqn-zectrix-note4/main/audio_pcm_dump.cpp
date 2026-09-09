#include "audio_pcm_dump.h"

#if CONFIG_WQN_AI_PCM_DUMP_ENABLE

#include <cstdio>

#include "esp_log.h"

namespace wqn {
namespace {

constexpr char kTag[] = "wqn_pcm_dump";
// 2 s at 16 kHz. Long enough to hold a phrase, short enough that the dump does
// not stall the turn for half a minute.
constexpr size_t kMaxDumpSamples = 32000;
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

}  // namespace

void DumpCapturedPcmForAnalysis(const int16_t* samples, size_t sample_count,
                                int sample_rate)
{
    if (samples == nullptr || sample_count == 0) {
        ESP_LOGW(kTag, "dump skipped: empty capture");
        return;
    }
    const bool truncated = sample_count > kMaxDumpSamples;
    const size_t count = truncated ? kMaxDumpSamples : sample_count;

    ESP_LOGI(kTag,
             "PCM_DUMP_BEGIN rate=%d channels=1 bits=16 samples=%u "
             "truncated=%d bytes=%u",
             sample_rate, static_cast<unsigned>(count), truncated ? 1 : 0,
             static_cast<unsigned>(count * sizeof(int16_t)));

    char line[kLineChars + 1] = {};
    size_t offset = 0;
    while (offset < count) {
        const size_t remaining = count - offset;
        const size_t take = remaining < (kBytesPerLine / sizeof(int16_t))
                                ? remaining
                                : (kBytesPerLine / sizeof(int16_t));
        const size_t chars = EncodeChunk(
            reinterpret_cast<const uint8_t*>(samples + offset),
            take * sizeof(int16_t), line);
        line[chars] = '\0';
        ESP_LOGI(kTag, "%s", line);
        offset += take;
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
