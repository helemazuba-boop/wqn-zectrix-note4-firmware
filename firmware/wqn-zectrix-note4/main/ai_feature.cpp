#include "ai_feature.h"

#include "ui_model.h"

namespace wqn {

const char* AiFeaturePhaseLabel(AiFeaturePhase phase)
{
    switch (phase) {
        case AiFeaturePhase::kLoading:
            return "加载";
        case AiFeaturePhase::kRecording:
            return "录音";
        case AiFeaturePhase::kTranscribing:
            return "转写";
        case AiFeaturePhase::kAwaitingConfirmation:
            return "待确认";
        case AiFeaturePhase::kSubmitting:
            return "提交";
        case AiFeaturePhase::kRunning:
            return "执行";
        case AiFeaturePhase::kComplete:
            return "完成";
        case AiFeaturePhase::kError:
            return "错误";
        case AiFeaturePhase::kIdle:
        default:
            return "空闲";
    }
}

bool AiFeaturePhaseIsBusy(AiFeaturePhase phase)
{
    return phase == AiFeaturePhase::kLoading ||
           phase == AiFeaturePhase::kRecording ||
           phase == AiFeaturePhase::kTranscribing ||
           phase == AiFeaturePhase::kSubmitting ||
           phase == AiFeaturePhase::kRunning;
}

bool AiFeatureCanStartVoiceInput(AiFeaturePhase phase)
{
    return phase == AiFeaturePhase::kIdle ||
           phase == AiFeaturePhase::kAwaitingConfirmation ||
           phase == AiFeaturePhase::kComplete ||
           phase == AiFeaturePhase::kError;
}

bool AiFeatureCanSubmit(const AiFeatureUiState& state)
{
    return state.phase == AiFeaturePhase::kAwaitingConfirmation &&
           state.requires_confirmation && !state.prompt_text.empty();
}

AiFeaturePhase AiFeaturePhaseFromLegacy(AiSessionStatus status)
{
    switch (status) {
        case AiSessionStatus::kPreparingCapture:
            return AiFeaturePhase::kLoading;
        case AiSessionStatus::kListening:
            return AiFeaturePhase::kRecording;
        case AiSessionStatus::kWaitingReply:
            return AiFeaturePhase::kSubmitting;
        case AiSessionStatus::kStreaming:
            return AiFeaturePhase::kRunning;
        case AiSessionStatus::kReplyReady:
            return AiFeaturePhase::kComplete;
        case AiSessionStatus::kError:
            return AiFeaturePhase::kError;
        case AiSessionStatus::kIdle:
        default:
            return AiFeaturePhase::kIdle;
    }
}

}  // namespace wqn
