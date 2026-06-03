#pragma once

#include "esp_err.h"
#include "ui_model.h"

namespace wqn {

esp_err_t InitAiSession();
esp_err_t StartAiRecordingSession();
esp_err_t StopAiRecordingAndSubmit();
bool CopyAiSessionToUi(AiSessionState* state);

}  // namespace wqn
