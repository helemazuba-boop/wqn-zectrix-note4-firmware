#pragma once

#include <atomic>
#include <cstdint>

namespace wqn::stdpro_ws {

// All accesses are serialized by the transport's short critical section.
// Public connect/start operations remain registered until their caller stops
// waiting; an idle owner must not tear down the connection under that wait.
class IdleDisconnectGate {
public:
    void BeginUse()
    {
        ++generation_;
        ++users_;
        pending_.store(false, std::memory_order_release);
    }

    void EndUse() { users_.fetch_sub(1, std::memory_order_release); }

    void Request()
    {
        requested_generation_.store(generation_.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
        pending_.store(true, std::memory_order_release);
    }

    bool TryClaim(bool idle, uint32_t* generation)
    {
        if (!pending_.load(std::memory_order_acquire) ||
            users_.load(std::memory_order_acquire) != 0 || !idle) {
            return false;
        }
        pending_.store(false, std::memory_order_release);
        *generation = requested_generation_.load(std::memory_order_relaxed);
        return true;
    }

private:
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint32_t> requested_generation_{0};
    std::atomic<uint32_t> users_{0};
    std::atomic<bool> pending_{false};
};

}  // namespace wqn::stdpro_ws
