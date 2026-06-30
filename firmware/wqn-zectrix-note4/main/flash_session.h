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

struct FlashUiState {
    FlashStatus status = FlashStatus::kIdle;
    std::string user_transcript;
    std::string assistant_text;
    std::string pending_text;
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
void OnFlashButtonPressed();
void OnFlashButtonReleased();

}  // namespace wqn
