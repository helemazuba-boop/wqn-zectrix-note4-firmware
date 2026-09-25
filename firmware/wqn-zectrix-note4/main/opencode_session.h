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
// Answer the live `agent.question` ask by option index. The index is the slot
// the option bar is showing, so it is always within the (at most two) options
// the gateway projected.
esp_err_t ReplyPendingOpenCodeQuestion(int index);
esp_err_t StartOpenCodeVoiceInput();
esp_err_t StopOpenCodeVoiceInput();
esp_err_t ConfirmOpenCodePrompt(int64_t confirmed_at_ms);
// Discard a prompt that has not been submitted yet.
void CancelOpenCodePrompt();
// Ask the stream worker to interrupt a run that is already submitted.
void InterruptOpenCodeRun();
// [scroll-clamp] min_scroll/max_scroll come from the AI page viewport layout
// (GetAiScrollBounds) and are the only correct bounds: on that page 0 is the
// anchor and NEGATIVE offsets scroll further down, so the offset must not be
// floored at 0 here. See the definition in opencode_session.cpp.
void ScrollOpenCodeResponse(int direction, int32_t min_scroll, int32_t max_scroll);
// [follow] Absolute, clamped write of the Agent viewport offset. ScrollOpenCode-
// Response moves relative to the offset the worker last saw; a turn jump and the
// per-tick auto-follow both compute an absolute target from the renderer's own
// layout pass, so they must not go through the relative path.
void SetOpenCodeScrollOffsetClamped(int32_t target, int32_t min_scroll, int32_t max_scroll);
// [follow] Per-turn viewport-follow state (see AgentSessionState). The per-tick
// follow step (UiRuntime::DispatchAiViewportFollow) retires the follow through
// this setter when the answer body lands, and the input path clears it on a
// manual scroll -- the UI copy alone would be overwritten by the next snapshot.
void SetOpenCodeFollowState(bool active, bool user_moved);
// [detail] The cloud detail tier every Agent request is projected at (0 简要 /
// 1 标准 / 2 详细, kOpenCodeDetailDefault in opencode_model.h), written from the
// status bar's detail control. A real change also drops the transcript cache key
// (history_loaded_session_id), so the next lock/observe re-reads the session at
// the new tier. It deliberately does NOT arm kLoadHistory: a live stream owns
// the command slot for its whole duration (ArmWorkerLocked) and LoadHistory
// stomps the phase to kIdle when no stream is attached -- arming from here would
// fight both.
void SetOpenCodeDetailLevel(uint8_t level);
bool CopyOpenCodeSessionToUi(AgentSessionState* state);

}  // namespace wqn
