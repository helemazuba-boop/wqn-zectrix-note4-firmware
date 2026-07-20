#pragma once

#include <cstdint>
#include <string>

#include "button_input.h"
#include "ui_model.h"

namespace device_ui_internal {

enum class RefreshSchedule;

enum class StatusControlAction : uint8_t {
    kNone,
    kAiTier,
    kAiThinking,
    kAiTts,
    kAiExpand,
    kAiClearContext,
    kWordScope,
    kWordSync,
};

struct StatusControlDescriptor {
    StatusControlAction action = StatusControlAction::kNone;
    std::string label;
    std::string value;
    bool enabled = true;
};

struct StatusControlProvider {
    wqn::StatusControlProvider id = wqn::StatusControlProvider::kNone;
    uint8_t control_count = 0;
};

StatusControlProvider ResolveStatusControlProvider(const wqn::UiState& state);
StatusControlDescriptor DescribeStatusControl(
    const wqn::UiState& state,
    uint8_t index);
bool OpenStatusControls(int64_t now_ms, wqn::UiState* state);
void CloseStatusControls(wqn::UiState* state);
RefreshSchedule ApplyStatusControlEvent(
    const wqn::ButtonEvent& event,
    int64_t now_ms,
    wqn::UiState* state);
bool RunStatusControlShellSelfTest();

}  // namespace device_ui_internal
