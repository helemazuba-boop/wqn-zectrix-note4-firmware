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
    std::string tool_label;        // "🔧 tool..." or "✅ tool done"
    std::string error_message;
    int64_t status_since_ms = 0;
};

esp_err_t InitFlashSession();
esp_err_t StartFlashSession();
esp_err_t StopFlashSession();
FlashStatus GetFlashStatus();
bool IsFlashConnected();
bool CopyFlashStateToUi(FlashUiState* state);
bool IsFlashTranscribing();

// Periodically called from the UI task; closes the audio amp after the configured
// idle tail so long pauses between server audio deltas don't leave the speaker
// enabled (pop/click at the next turn). Safe to call when flash is idle.
void PollFlashAmpIdle();
void OnFlashButtonPressed();
void OnFlashButtonReleased();

}  // namespace wqn
