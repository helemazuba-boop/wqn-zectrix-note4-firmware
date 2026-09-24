#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ai_feature.h"
#include "opencode_client.h"

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
    std::string pending_permission_id;
    // Live `agent.question` ask. The device answers by option index, never by
    // field id: the gateway owns that mapping.
    std::string pending_question_id;
    std::string pending_question_title;
    std::vector<OpenCodeQuestionOption> pending_question_options;
    // Session id whose transcript is already in the kAgent channel. Backfill
    // runs once per session, on switch and on first observe, so a reconnect
    // cannot append the same turns twice.
    std::string history_loaded_session_id;
    int64_t confirmation_armed_at_ms = 0;
    bool session_locked = false;
    bool stream_active = false;
};

}  // namespace wqn
