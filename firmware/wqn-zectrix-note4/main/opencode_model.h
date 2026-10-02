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

// [detail] Cloud detail tier requested by this device: 0 = brief (text only,
// with a one-line digest standing in for a tool-only turn), 1 = standard
// (text + tool blocks), 2 = full (+ thinking). Sent as `?detail=N` on the
// history, run and events requests; the gateway omits the optional fields
// accordingly, and below full it also stops streaming reasoning.
//
// No writer this round: the value is pinned here so the request path can be
// built and verified end to end. The NVS-backed setting and its switch land
// together in the next round -- until then, flipping this constant and
// reflashing is the only way to exercise a tier on device.
constexpr uint8_t kOpenCodeDetailDefault = 2;

struct AgentSessionState {
    AiFeatureUiState ui;
    std::vector<AgentSessionOption> sessions;
    size_t selected_session = 0;
    std::string current_session_id;
    std::string current_session_title;
    std::string pending_permission_id;
    // The session that raised the pending permission. It is the attached session
    // for a permission this device asked about, and a subagent's own id when a
    // subagent raised it -- the reply route is scoped to whichever it was, so
    // the id travels with the ask from the frame that delivered it to the POST.
    std::string pending_permission_session;
    // Live `agent.question` ask. The device answers by option index, never by
    // field id: the gateway owns that mapping. The id carries the gateway's
    // step (`{formId}#{step}`) and is echoed back verbatim, which is how a
    // multi-field form is answered one step per request. An empty option list
    // means the field has nothing the device can choose; the bar then offers
    // only 自定义回答, which aborts the run instead of replying.
    std::string pending_question_id;
    std::string pending_question_title;
    // The session that raised the pending question; see pending_permission_session.
    std::string pending_question_session;
    std::vector<OpenCodeQuestionOption> pending_question_options;
    // Session id whose transcript is already in the kAgent channel. Backfill
    // runs once per session, on switch and on first observe, so a reconnect
    // cannot append the same turns twice.
    std::string history_loaded_session_id;
    int64_t confirmation_armed_at_ms = 0;
    bool session_locked = false;
    bool stream_active = false;
    // [follow] Per-turn viewport-follow state for the Agent tier; same
    // semantics as AiSessionState::follow_active / user_moved. Armed by the
    // turn-start paths in opencode_session.cpp, cleared by manual scrolls.
    bool follow_active = false;
    bool user_moved = false;
    // [detail] See kOpenCodeDetailDefault. Rides the snapshot to the UI with
    // every other field; the request builders read it at command-arm time.
    uint8_t detail_level = kOpenCodeDetailDefault;
};

}  // namespace wqn
