#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "ui_model.h"

namespace wqn {

esp_err_t InitAiSession();
esp_err_t StartAiRecordingSession();
esp_err_t StopAiRecordingAndSubmit();
// Clear STD/PRO conversation context (AiHistory + conversation_id + display
// state). Called on leave-AI-screen so each visit starts a fresh conversation;
// the cloud (esp32_ai_conversations) keeps the saved history for reuse later.
void ClearAiConversationContext();
bool CopyAiSessionToUi(AiSessionState* state);
void SetAiTier(AiTier tier);
AiTier GetAiTier();
// [shell->wire] Status-bar toggle setters: propagate UI changes into g_state so
// they survive CopyAiSessionToUi's full-struct overwrite and reach the request.
void SetAiThinkingLevel(ThinkingLevel level);
void SetAiTtsOn(bool on);
void SetAiExpandContent(bool expanded);
int32_t GetAiScrollOffsetLines();

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

// Power-transition guard. True while preparation, capture submission or SSE
// response processing owns AI session state.
bool IsAiSessionActive();

// ---- v2 chat surface (top toast + scrollable viewport) ----------------------
//
// The AI page renders a viewport at y=51..299 with the status-bar above it.
// Status changes flow through a top toast that overrides idle chrome for the
// duration of recording, upload and waiting/reply states. There is no
// per-second "3.1s" counter on the wait toast — the cloud pipeline is
// expected to express state transitions through stage events instead of
// latency feedback.
//
// Callbacks (long-press confirm / long-release confirm) feed these helpers,
// and the v2 SSE consumer flushes history rows into AiHistory. The UI side
// renders straight off the history snapshot + scroll offset + toast fields.
void ShowAiToast(const std::string& label);                   // e.g. "● 上传…"
void HideAiToast();                                            // for ready / idle
void SetAiRecordingLabel(int32_t elapsed_ms);                  // updates recording toast
void ResetAiScroll();                                          // recenter on newest
// Single-lock read-clamp-write of the scroll offset. Bounds come from the
// shared AI layout pass (GetAiScrollBounds); degenerate bounds fail open.
void SetAiScrollOffsetLinesClamped(int32_t target, int32_t min_scroll, int32_t max_scroll);
void StampScrollNoOpHint();                                    // flash "已最新" hint at bottom
int32_t GetAiScrollOffsetLines();

// Returns true if there is an active toast (used by the page renderer to draw
// the y=27..51 region in addition to the viewport).
bool IsAiToastVisible();
const std::string& CurrentAiToastLabel();

}  // namespace wqn
