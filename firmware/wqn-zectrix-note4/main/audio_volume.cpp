#include "audio_volume.h"

#include <cmath>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace wqn {

namespace {

constexpr char kTag[] = "wqn_audio_volume";
constexpr uint8_t kRegDacMute   = 0x31;
constexpr uint8_t kRegDacVolume = 0x32;
constexpr uint8_t kDacMuteBits  = 0x60;  // 0x31 bits6:5 = DSM + DEM mute
// ES8311 REG32 is +0.5 dB per step: 0x00=-95.5 dB, 0xBF=0 dB,
// 0xFF=+32 dB. Keep user volume at or below 0 dB.
constexpr uint8_t kVolRegMin = 0x00;
constexpr uint8_t kVolRegZeroDb = 0xBF;

esp_err_t WriteReg(
    const services::AudioSession& session,
    services::AudioCodecHandle dev,
    uint8_t reg,
    uint8_t val) {
    return services::WriteAudioCodecRegister(session, dev, reg, val);
}

esp_err_t ReadReg(
    const services::AudioSession& session,
    services::AudioCodecHandle dev,
    uint8_t reg,
    uint8_t* val) {
    return services::ReadAudioCodecRegister(session, dev, reg, val);
}

// Log map so a linear percent matches logarithmic ear perception:
//   dB = 20*log10(percent/100), reg = 0xBF + dB / 0.5dB
//   100% -> 0xBF (0 dB), 75% -> 0xBA (-2.5 dB), 50% -> 0xB3 (-6 dB),
//   25% -> 0xA7 (-12 dB), 1% -> 0x6F (-40 dB).
uint8_t PercentToVolReg(int percent) {
    if (percent >= 100) return kVolRegZeroDb;
    if (percent <= 0) return kVolRegMin;
    const double dB = 20.0 * std::log10(static_cast<double>(percent) / 100.0);  // <= 0
    int reg = static_cast<int>(std::lround(kVolRegZeroDb + dB / 0.5));
    if (reg < kVolRegMin) reg = kVolRegMin;
    if (reg > kVolRegZeroDb) reg = kVolRegZeroDb;
    return static_cast<uint8_t>(reg);
}

}  // namespace

void SetEs8311Volume(
    const services::AudioSession& session,
    services::AudioCodecHandle dev,
    int percent) {
    if (dev == nullptr) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // [reg31-rmw-fix] RMW REG31 to only toggle DSM/DEM mute (bit6:5),
    // preserving bit7/4/3/2/1/0 (DAC_INV, RAMCLR, dither, undocumented bits).
    // Prior direct write 0x00 cleared undocumented bits -> DAC path unstable ->
    // 味呲 hiss. Mirrors esp_codec_dev es8311_set_mute: reg31 &= 0x9F; mute|=0x60.
    uint8_t reg31 = 0;
    if (ReadReg(session, dev, kRegDacMute, &reg31) == ESP_OK) {
        reg31 &= 0x9F;  // clear bit6:5 (DSM/DEM mute) -> unmute
        if (percent == 0) {
            reg31 |= 0x60;  // mute both DSM + DEM
        }
        WriteReg(session, dev, kRegDacMute, reg31);
    } else {
        // read failed (I2C jitter) - fallback to direct write
        const uint8_t mute_reg = (percent == 0) ? kDacMuteBits : 0x00;
        WriteReg(session, dev, kRegDacMute, mute_reg);
    }
    const uint8_t vol_reg = PercentToVolReg(percent);
    WriteReg(session, dev, kRegDacVolume, vol_reg);

    uint8_t reg31_readback = 0xFF;
    uint8_t reg32_readback = 0xFF;
    const esp_err_t mute_read = ReadReg(
        session, dev, kRegDacMute, &reg31_readback);
    const esp_err_t volume_read = ReadReg(
        session, dev, kRegDacVolume, &reg32_readback);
    if (mute_read == ESP_OK && volume_read == ESP_OK) {
        ESP_LOGI(kTag, "ES8311 volume: percent=%d REG31=0x%02x REG32=0x%02x",
                 percent, reg31_readback, reg32_readback);
    } else {
        ESP_LOGW(kTag, "ES8311 volume readback failed: percent=%d REG31=%s REG32=%s",
                 percent, esp_err_to_name(mute_read), esp_err_to_name(volume_read));
    }
}

}  // namespace wqn
