#pragma once

#include <string>

#include "esp_err.h"
#include "ui_model.h"

namespace wqn {

enum class FlashStatus {
    kIdle,
    kConnecting,
    kStreaming,
    kError,
};

// Public surface returned to the UI; everything is a snapshot.
struct FlashUiState {
    FlashStatus status = FlashStatus::kIdle;
    std::string user_transcript;   // incremental ASR text
    std::string assistant_text;    // incremental assistant transcript
    std::string pending_text;      // human-readable status
    std::string tool_label;        // "tool..." or "tool done"
    std::string error_message;
    int64_t status_since_ms = 0;
    // [phase-fix] kStreaming means "WebSocket connected", not "ASR active".
    // Expose turn-level facts so the UI can render the real Flash phase.
    bool connected = false;
    bool capture_started = false;
    bool response_in_flight = false;
    bool response_started = false;  // thinking/text delta received
    bool playback_active = false;
};

esp_err_t InitFlashSession();
esp_err_t StartFlashSession();
esp_err_t StopFlashSession();
bool CopyFlashStateToUi(FlashUiState* state);
// Compatibility UI poll. Amp maintenance runs on the playback owner task.
void PollFlashAmpIdle();
// UI calls publish one admitted press/release to the lifecycle owner; they
// perform no synchronous network, audio, or drain operations.
void OnFlashButtonPressed();
void OnFlashButtonReleased(bool submit = true);  // [mistouch] submit=false discards the turn (short tap)

// [barge-in] Invalidate Flash TTS packets immediately; the playback owner drains
// the ringbuffer and I2S DMA, and the lifecycle owner asks the proxy to
// cancel the in-flight response so the server stops sending more audio. Called
// by non-PTT button events so the user is not forced to listen to the whole
// reply. PTT (Confirm hold) keeps its own barge-in in OnFlashButtonPressed
// (which also starts a new turn). No-op when nothing is playing.
void AbortFlashPlayback();

}  // namespace wqn
