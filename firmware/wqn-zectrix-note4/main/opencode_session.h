#pragma once

#include "esp_err.h"
#include "opencode_model.h"

namespace wqn {

esp_err_t InitOpenCodeSession();
esp_err_t RequestOpenCodeSessionList();
esp_err_t MoveOpenCodeSessionSelection(int direction);
esp_err_t LockSelectedOpenCodeSession();
esp_err_t CreateNewOpenCodeSession();
esp_err_t ObserveOpenCodeSession();
esp_err_t ReplyPendingOpenCodePermission(bool approve);
esp_err_t StartOpenCodeVoiceInput();
esp_err_t StopOpenCodeVoiceInput();
esp_err_t ConfirmOpenCodePrompt(int64_t confirmed_at_ms);
void CancelOpenCodePrompt();
// [scroll-clamp] min_scroll/max_scroll come from the AI page viewport layout
// (GetAiScrollBounds) and are the only correct bounds: on that page 0 is the
// anchor and NEGATIVE offsets scroll further down, so the offset must not be
// floored at 0 here. See the definition in opencode_session.cpp.
void ScrollOpenCodeResponse(int direction, int32_t min_scroll, int32_t max_scroll);
bool CopyOpenCodeSessionToUi(AgentSessionState* state);
bool IsOpenCodeSessionActive();

}  // namespace wqn
