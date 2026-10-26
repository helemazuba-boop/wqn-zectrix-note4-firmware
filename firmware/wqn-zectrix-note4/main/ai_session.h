#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "ui_model.h"

namespace wqn {

esp_err_t InitAiSession();
// The shared capture service normally mirrors PCM into the legacy Std/Pro
// WebSocket transport. A feature that owns the microphone for a DIFFERENT
// backend can suspend the tap; the Agent voice pipe deliberately keeps it
// enabled, because its tier=agent turn rides that same transport (the tap
// feeds stdpro_ws, which drops PCM until a turn reaches kRecording).
void SetAiAudioCaptureTapEnabled(bool enabled);
// [agent-voice] Exported for the Agent voice pipe (agent_voice_pipe.cpp),
// which drives its own stdpro_ws turn and therefore cannot reuse
// PrepareRecordingSession. ArmAiVoicePreroll only arms the one-shot replay of
// audio captured before the turn committed -- it must NOT install the STD SSE
// trampoline (SetSseCallback is a single global slot; the agent path installs
// its own trampoline).
void ArmAiVoicePreroll();
// Request-id used by the SSE idempotency headers: 16 hex chars.
std::string GenerateRequestId();
esp_err_t StartAiRecordingSession();
// Release stops capture and transcribes only. ConfirmAiPrompt is the sole submit gate.
esp_err_t StopAiRecordingAndSubmit();
esp_err_t ConfirmAiPrompt(int64_t confirmed_at_ms);
void CancelAiPrompt();
void CancelAiVoiceInput();
void LeaveStdAiTier();
esp_err_t ReplyAiInteraction(int index);
// Clear STD/PRO conversation context (AiHistory + conversation_id + display
// state). Called on leave-AI-screen so each visit starts a fresh conversation;
// the cloud (esp32_ai_conversations) keeps the saved history for reuse later.
void ClearAiConversationContext();
bool CopyAiSessionToUi(AiSessionState* state);
void SetAiTier(AiTier tier);
// [shell->wire] Status-bar toggle setters: propagate UI changes into g_state so
// they survive CopyAiSessionToUi's full-struct overwrite and reach the request.
void SetAiThinkingLevel(ThinkingLevel level);
void SetAiTtsOn(bool on);
void SetAiExpandContent(bool expanded);
// [follow] Auto-follow toggle (status-bar edit cluster) and the per-turn
// viewport-follow state. These MUST be setters, not UI-copy writes:
// CopyAiSessionToUi overwrites the whole struct, so a write that only touched
// the UI's copy would be reverted by the next snapshot.
void SetAiAutoFollow(bool follow);
void SetAiFollowState(bool active, bool user_moved);

// Lightweight, mutex-free snapshot of v2 SSE streaming bookkeeping. UI calls
// this from its own task to drive the EPD throttle and the status-bar chip
// without taking the heavy ai_session lock for the full state copy.
struct AiStreamingStatusView {
    bool streaming_active = false;
    bool force_full_render = false;
    AiSessionStatus status = AiSessionStatus::kIdle;
    int64_t status_since_ms = 0;
    int64_t last_render_ms = 0;
    std::string pending_label;
    std::string tool_label;
};

bool CopyAiStreamingStatus(AiStreamingStatusView* view);

// Recording feedback and scrolling update the shared session snapshot.
void SetAiRecordingLabel(int32_t elapsed_ms);                  // updates recording toast
// Single-lock read-clamp-write of the scroll offset. Bounds come from the
// shared AI layout pass (GetAiScrollBounds); degenerate bounds fail open.
void SetAiScrollOffsetLinesClamped(int32_t target, int32_t min_scroll, int32_t max_scroll);
void StampScrollNoOpHint();                                    // flash "已最新" hint at bottom
int32_t GetAiScrollOffsetLines();

}  // namespace wqn
