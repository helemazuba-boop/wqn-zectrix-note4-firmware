#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "ui_model.h"

namespace wqn {

esp_err_t InitAiSession();
esp_err_t StartAiRecordingSession();
esp_err_t StopAiRecordingAndSubmit();
bool CopyAiSessionToUi(AiSessionState* state);
void SetAiTier(AiTier tier);
AiTier GetAiTier();

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

}  // namespace wqn
