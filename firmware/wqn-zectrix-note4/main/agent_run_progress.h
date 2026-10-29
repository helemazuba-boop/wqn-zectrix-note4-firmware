#pragma once

#include <cstdint>
#include <string>

#include "opencode_client.h"

namespace wqn {

// Local stream observation policy, not a verdict on upstream execution.
class AgentRunProgress {
public:
    enum class Timeout { kNone, kFirstEvent, kProgress };
    static constexpr int64_t kFirstEventUs = 60LL * 1000 * 1000;
    static constexpr int64_t kProgressUs = 5LL * 60 * 1000 * 1000;

    AgentRunProgress(bool submitted_run, int64_t now_us, const std::string& session)
        : enabled_(submitted_run), last_progress_us_(now_us), session_(session) {}

    void Observe(const OpenCodeEvent& event, int64_t now_us)
    {
        if (!enabled_) return;
        if (event.kind == OpenCodeEventKind::kAccepted ||
            event.kind == OpenCodeEventKind::kAttached) {
            // An initial receipt is evidence, repeated receipts are not new work.
            if (!seen_event_) { seen_event_ = true; last_progress_us_ = now_us; }
            return;
        }
        seen_event_ = true;
        last_progress_us_ = now_us;
        const std::string& owner = event.session_id.rfind("ses_", 0) == 0 ? event.session_id : session_;
        if (event.kind == OpenCodeEventKind::kPermission) {
            permission_id_ = event.permission_id;
            permission_session_ = owner;
            // Match the UI: a permission supersedes the question slot.
            question_id_.clear();
            question_session_.clear();
        } else if (event.kind == OpenCodeEventKind::kQuestion) {
            // The UI holds one question behind a permission and drops a third
            // ask. Keep the answerable identity, not the unshown replacement.
            if (permission_id_.empty() || question_id_.empty()) {
                question_id_ = event.question_id;
                question_session_ = owner;
            }
        } else if ((event.kind == OpenCodeEventKind::kStatus &&
                    (event.status == "idle" || event.status == "error")) ||
                   (event.kind == OpenCodeEventKind::kError && event.fatal)) {
            permission_id_.clear();
            question_id_.clear();
        }
    }

    void Replied(const OpenCodeOutboundReply& reply, bool succeeded, int64_t now_us)
    {
        if (!enabled_ || !succeeded) return;
        const std::string& owner = OpenCodeReplySessionId(reply, session_);
        std::string& id = reply.is_question ? question_id_ : permission_id_;
        const std::string& pending_owner = reply.is_question ? question_session_ : permission_session_;
        const std::string& replied_id = reply.is_question ? reply.question_id : reply.permission_id;
        if (!id.empty() && id == replied_id && owner == pending_owner) {
            id.clear();
            last_progress_us_ = now_us;
        }
    }

    Timeout Expired(int64_t now_us) const
    {
        // An observe stream is intentionally long lived. A run awaiting a real
        // permission/question also remains attached while the user decides.
        if (!enabled_ || !permission_id_.empty() || !question_id_.empty()) return Timeout::kNone;
        const int64_t budget = seen_event_ ? kProgressUs : kFirstEventUs;
        if (now_us - last_progress_us_ < budget) return Timeout::kNone;
        return seen_event_ ? Timeout::kProgress : Timeout::kFirstEvent;
    }

private:
    bool enabled_;
    bool seen_event_ = false;
    int64_t last_progress_us_;
    std::string session_;
    std::string permission_id_, permission_session_;
    std::string question_id_, question_session_;
};

}  // namespace wqn
