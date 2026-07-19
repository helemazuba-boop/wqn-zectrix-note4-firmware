#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "power/sleep_protocol.h"

namespace wqn::services {

enum class AudioActivity : uint8_t {
    kCapture,
    kPlayback,
    kFlash,
    kSelfTest,
};

enum class AudioState : uint8_t {
    kIdle,
    kCapturing,
    kPlaying,
    kFlash,
    kSelfTest,
    kQuiescing,
};

// Fixed-size capability returned by AudioService. A token is valid until the
// matching EndAudioActivity call succeeds. It deliberately owns no heap data
// and can be embedded in a long-lived capture/Flash state object.
struct AudioSession {
    uint32_t id = 0;
    AudioActivity activity = AudioActivity::kCapture;

    explicit operator bool() const { return id != 0; }
};

struct AudioSnapshot {
    AudioState state = AudioState::kIdle;
    uint32_t session_id = 0;
    bool codec_powered = true;
    bool amplifier_enabled = false;
};

// Opaque Note4 audio driver handles. Keeping ESP-IDF driver types out of
// feature modules makes AudioService the only runtime hardware entry point.
using AudioBusHandle = void*;
using AudioCodecHandle = void*;
using AudioChannelHandle = void*;

esp_err_t StartAudioService();

// Capture, playback, Flash and self-test are mutually exclusive. Every begin
// receives either a session token or an explicit error result; queue pressure
// is reported as ESP_ERR_TIMEOUT rather than silently dropping a command.
esp_err_t BeginAudioActivity(AudioActivity activity, AudioSession* session);
esp_err_t EndAudioActivity(AudioSession* session);

// Runtime audio GPIOs are only changed by AudioService. The board HAL remains
// the sole bootstrap exception before this task starts.
esp_err_t SetAudioAmplifier(const AudioSession& session, bool enabled);

esp_err_t GetSharedAudioBus(
    const AudioSession& session, AudioBusHandle* bus);
esp_err_t AddAudioCodec(
    const AudioSession& session,
    AudioBusHandle bus,
    AudioCodecHandle* codec);
esp_err_t RemoveAudioCodec(
    const AudioSession& session, AudioCodecHandle* codec);
esp_err_t WriteAudioCodecRegister(
    const AudioSession& session,
    AudioCodecHandle codec,
    uint8_t reg,
    uint8_t value);
esp_err_t ReadAudioCodecRegister(
    const AudioSession& session,
    AudioCodecHandle codec,
    uint8_t reg,
    uint8_t* value);

esp_err_t CreateAudioRxChannel(
    const AudioSession& session,
    uint32_t sample_rate_hz,
    uint32_t dma_frame_count,
    bool left_aligned,
    AudioChannelHandle* rx);
esp_err_t CreateAudioTxChannel(
    const AudioSession& session,
    uint32_t sample_rate_hz,
    uint32_t dma_frame_count,
    bool left_aligned,
    AudioChannelHandle* tx);
esp_err_t CreateAudioDuplexChannels(
    const AudioSession& session,
    uint32_t sample_rate_hz,
    uint32_t dma_frame_count,
    bool left_aligned,
    AudioChannelHandle* rx,
    AudioChannelHandle* tx);
esp_err_t EnableAudioChannel(
    const AudioSession& session, AudioChannelHandle channel);
esp_err_t DisableAudioChannel(
    const AudioSession& session, AudioChannelHandle channel);
esp_err_t DeleteAudioChannel(
    const AudioSession& session, AudioChannelHandle* channel);
esp_err_t ReadAudioChannel(
    const AudioSession& session,
    AudioChannelHandle channel,
    void* destination,
    size_t bytes,
    size_t* bytes_read,
    TickType_t timeout);
esp_err_t WriteAudioChannel(
    const AudioSession& session,
    AudioChannelHandle channel,
    const void* source,
    size_t bytes,
    size_t* bytes_written,
    TickType_t timeout);
esp_err_t ResetAudioTxChannel(
    const AudioSession& session, AudioChannelHandle channel);

esp_err_t PrepareAudioServiceForSleep(
    const power::PrepareSleepCommand& command);
void RollbackAudioServiceAfterSleepAbort(uint32_t generation);
void ReleaseAudioServiceDeepSleepHolds();

AudioSnapshot GetAudioSnapshot();
const char* AudioActivityName(AudioActivity activity);
const char* AudioStateName(AudioState state);

}  // namespace wqn::services
