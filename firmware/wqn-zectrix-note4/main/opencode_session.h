#pragma once

#include "esp_err.h"
#include "opencode_model.h"

namespace wqn {

esp_err_t InitOpenCodeSession();
esp_err_t RequestOpenCodeSessionList();
esp_err_t MoveOpenCodeSessionSelection(int direction);
esp_err_t LockSelectedOpenCodeSession();
esp_err_t StartOpenCodeVoiceInput();
esp_err_t StopOpenCodeVoiceInput();
esp_err_t ConfirmOpenCodePrompt(int64_t confirmed_at_ms);
void CancelOpenCodePrompt();
void ScrollOpenCodeResponse(int direction);
bool CopyOpenCodeSessionToUi(AgentSessionState* state);
bool IsOpenCodeSessionActive();

}  // namespace wqn
