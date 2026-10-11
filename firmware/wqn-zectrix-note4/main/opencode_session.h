#pragma once

#include "esp_err.h"
#include "opencode_model.h"

namespace wqn {

// Why an Agent request was refused. Every attach entry point returns the same
// bare `ESP_ERR_INVALID_STATE` for all of these, which made "the list is
// empty", "another feature holds the sleep lease" and "a bounded read is still
// running" indistinguishable to the caller -- the UI could only log "rejected".
// Passed as an optional out-param so the failure mode is nameable.
enum class OpenCodeRejectReason : uint8_t {
    kNone,
    // The worker owns the command slot but no switch can be taken: a bounded
    // read is running (transcript backfill, voice capture), or the stream that
    // was attached has just reached its terminal frame and the tail is closing
    // it. All of these clear within seconds, so this is a retry, not a wall.
    kWorkerBusy,
    // Another feature holds the sleep lease, so this one cannot start.
    kLeaseBusy,
    // No session is selected in the picker.
    kNoSelection,
    // There is no session to act on at all.
    kNoSession,
};

esp_err_t InitOpenCodeSession();
// `reason` (optional) is written only when the call fails, so a caller that does
// not care can pass nullptr. When a stream is attached, the list request is not
// refused at all: it detaches from that stream -- leaving the run going in the
// cloud -- and then loads the list. That is the switch described in
// opencode_client.h.
esp_err_t RequestOpenCodeSessionList(OpenCodeRejectReason* reason = nullptr);
esp_err_t MoveOpenCodeSessionSelection(int direction);
esp_err_t LockSelectedOpenCodeSession(OpenCodeRejectReason* reason = nullptr);
esp_err_t CreateNewOpenCodeSession(OpenCodeRejectReason* reason = nullptr);
esp_err_t ObserveOpenCodeSession(OpenCodeRejectReason* reason = nullptr);
esp_err_t ReplyPendingOpenCodePermission(bool approve);
// Answer the live `agent.question` ask by option index. The index is the slot
// the option bar is showing, so it is always within the (at most two) options
// the gateway projected.
esp_err_t ReplyPendingOpenCodeQuestion(int index);
esp_err_t StartOpenCodeVoiceInput();
esp_err_t StopOpenCodeVoiceInput();
// [agent] B3 / D-lease: called when the UI leaves the Agent tier (a tier cycle,
// or leaving the AI screen entirely). Detaches from the attached stream without
// asking upstream to stop anything -- the cloud run keeps going -- and drops the
// sleep lease the stream was holding. Safe to call when nothing is attached;
// see the definition in opencode_session.cpp.
void LeaveOpenCodeAgentTier();
// [voice-pipe] Down short press during a capture phase. While recording this
// hard-aborts the WS turn (audio discarded, nothing submitted); while
// transcribing it soft-cancels (the transcript is discarded when it lands).
// Both land on idle with a "已取消" label; a phase outside the capture pair
// makes it a no-op.
void CancelAgentVoiceInput();
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
