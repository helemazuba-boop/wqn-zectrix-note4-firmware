#pragma once

#include <cstdint>
#include <limits>

namespace wqn::stdpro_ws {

// Caller serializes access. Notification bits only wake the owner; this
// mailbox binds their meaning to one client lifetime and retires old backlog.
class WsEventMailbox {
public:
    uint32_t Begin()
    {
        Retire();
        if (last_generation_ == std::numeric_limits<uint32_t>::max()) return 0;
        active_generation_ = ++last_generation_;
        return active_generation_;
    }

    void Retire()
    {
        active_generation_ = 0;
        pending_ = 0;
    }

    bool Matches(uint32_t generation) const
    {
        return generation != 0 && generation == active_generation_;
    }

    bool Post(uint32_t generation, uint32_t bits)
    {
        if (!Matches(generation)) return false;
        pending_ |= bits;
        return true;
    }

    uint32_t Take()
    {
        const uint32_t bits = pending_;
        pending_ = 0;
        return bits;
    }

private:
    uint32_t last_generation_ = 0;
    uint32_t active_generation_ = 0;
    uint32_t pending_ = 0;
};

}  // namespace wqn::stdpro_ws
