#pragma once

#include <cstdint>
#include <cstring>

namespace wqn::stdpro_ws {

// Owner-task only. Identity-bound; later/duplicate observations can shorten
// an absolute handoff budget, never extend it. No callback, allocation or I/O.
class TurnReleaseDeadline {
public:
    static constexpr int64_t kResultGraceMs = 3000;
    // Existing ASR callers wait at most 120 s plus their release observation.
    // This owner failsafe also covers callers that soft-cancel and stop waiting.
    static constexpr int64_t kMaxAsrHandoffMs = 150000;

    void Reset() { generation_ = 0; request_id_[0] = '\0'; due_ms_ = 0; }

    void Arm(uint32_t generation, const char* request_id, int64_t now_ms, int64_t budget_ms)
    {
        if (generation == 0 || request_id == nullptr || request_id[0] == '\0' ||
            std::strlen(request_id) >= sizeof(request_id_) || budget_ms <= 0) return;
        const int64_t due = now_ms + budget_ms;
        if (generation_ == generation && std::strcmp(request_id_, request_id) == 0) {
            if (due_ms_ == 0 || due < due_ms_) due_ms_ = due;
            return;
        }
        generation_ = generation;
        std::strcpy(request_id_, request_id);
        due_ms_ = due;
    }

    bool Expired(uint32_t generation, const char* request_id, int64_t now_ms) const
    {
        return due_ms_ > 0 && now_ms >= due_ms_ && generation == generation_ &&
            request_id != nullptr && std::strcmp(request_id_, request_id) == 0;
    }

private:
    uint32_t generation_ = 0;
    char request_id_[64] = {};
    int64_t due_ms_ = 0;
};

}  // namespace wqn::stdpro_ws
