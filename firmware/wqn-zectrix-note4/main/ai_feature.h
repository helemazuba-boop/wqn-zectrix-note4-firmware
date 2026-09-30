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
    std::string action_hint;
    int32_t scroll_offset_lines = 0;
    bool requires_confirmation = false;
};

const char* AiFeaturePhaseLabel(AiFeaturePhase phase);
bool AiFeatureCanStartVoiceInput(AiFeaturePhase phase);
bool AiFeatureCanSubmit(const AiFeatureUiState& state);
AiFeaturePhase AiFeaturePhaseFromLegacy(AiSessionStatus status);

}  // namespace wqn
