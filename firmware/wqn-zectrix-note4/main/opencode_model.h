#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ai_feature.h"

namespace wqn {

struct AgentSessionOption {
    std::string id;
    std::string title;
    int64_t updated_at = 0;
};

struct AgentSessionState {
    AiFeatureUiState ui;
    std::vector<AgentSessionOption> sessions;
    size_t selected_session = 0;
    std::string current_session_id;
    std::string current_session_title;
    int64_t confirmation_armed_at_ms = 0;
    bool session_locked = false;
    bool stream_active = false;
};

}  // namespace wqn
