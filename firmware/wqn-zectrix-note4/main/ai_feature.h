#pragma once

#include <cstdint>
#include <string>

namespace wqn {

enum class AiSessionStatus;

// Backend-neutral interaction phases shared by voice chat and Agent screens.
// A transcript is never executable while it is in kAwaitingConfirmation.
enum class AiFeaturePhase : uint8_t {
    kIdle,
    kLoading,
    kRecording,
    kTranscribing,
    kAwaitingConfirmation,
    kSubmitting,
    kRunning,
    kAwaitingPermission,
    kAwaitingQuestion,
    kComplete,
    kError,
};

struct AiFeatureUiState {
    AiFeaturePhase phase = AiFeaturePhase::kIdle;
    std::string title;
    std::string context_label;
    std::string status_label;
    std::string prompt_text;
    // [voice-pipe] Live ASR text for the in-flight voice capture. Appended by
    // the agent SSE callback while the turn is transcribing and rendered in
    // the pending bubble; the finalized text lands in prompt_text instead.
    std::string voice_partial;
    std::string response_text;
    std::string activity_text;
    // [banner] When activity_text was last set to something the user has to
    // read. activity_text is written in ~40 places and most of those lines must
    // never reach the screen (a connect notice the status label already states,
    // an answer already sitting in the transcript, a gesture hint that belongs
    // in the manual). Only the callers routed through
    // SetUserVisibleActivityLocked stamp this, and the line is on screen only
    // while the stamp is fresh -- see kAiActivityBannerTtlMs.
    int64_t activity_text_ms = 0;
    std::string action_hint;
    int32_t scroll_offset_lines = 0;
    bool requires_confirmation = false;
};

// [banner] How long a stamped activity_text line stays legible. Long enough to
// read a 20-character line, short enough that a stale event cannot outlive the
// state it described -- which is exactly what the bottom band used to let happen
// to "已连接 Session 事件流".
constexpr int64_t kAiActivityBannerTtlMs = 2000;

const char* AiFeaturePhaseLabel(AiFeaturePhase phase);
bool AiFeatureCanStartVoiceInput(AiFeaturePhase phase);
bool AiFeatureCanSubmit(const AiFeatureUiState& state);
AiFeaturePhase AiFeaturePhaseFromLegacy(AiSessionStatus status);

}  // namespace wqn
