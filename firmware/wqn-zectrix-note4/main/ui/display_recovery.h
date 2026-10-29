#pragma once

#include <cstdint>

namespace device_ui_internal {

struct DisplayRecoveryStatus {
    uint8_t consecutive_failures = 0;
    bool paused = false;
    bool probe_pending = false;
    bool probe_in_flight = false;
    int64_t retry_at_ms = 0;
    int64_t paused_at_ms = 0;

    bool HasPendingRetry() const { return (!paused && consecutive_failures != 0) || probe_pending; }
    bool CanQueue(int64_t now_ms) const { return (!paused || probe_pending) && now_ms >= retry_at_ms; }
    bool RequiresFull() const { return consecutive_failures != 0; }
};

// Protected by the refresh mutex. Only the EPD owner starts/completes attempts;
// UI may request one probe after a fresh physical press. No storage or timer.
class DisplayRecoveryPolicy {
public:
    inline static constexpr int64_t kRetryDelaysMs[] = {1000, 5000, 15000};
    static constexpr unsigned kAutomaticRetries = sizeof(kRetryDelaysMs) / sizeof(kRetryDelaysMs[0]);
    static constexpr int64_t kProbeCooldownMs = 1000;

    DisplayRecoveryStatus status() const { return state_; }

    bool TryBegin(int64_t now_ms)
    {
        if (!state_.CanQueue(now_ms)) return false;
        if (state_.paused) {
            state_.probe_pending = false;
            state_.probe_in_flight = true;
        }
        return true;
    }

    void Presented() { state_ = {}; }

    void Failed(int64_t now_ms)
    {
        if (state_.consecutive_failures < UINT8_MAX) ++state_.consecutive_failures;
        state_.probe_pending = state_.probe_in_flight = false;
        if (state_.consecutive_failures <= kAutomaticRetries) {
            state_.retry_at_ms = now_ms + kRetryDelaysMs[state_.consecutive_failures - 1];
        } else {
            state_.paused = true;
            state_.paused_at_ms = now_ms;
            state_.retry_at_ms = now_ms + kProbeCooldownMs;
        }
    }

    bool RequestProbe(int64_t input_at_ms, int64_t now_ms)
    {
        if (!state_.paused || state_.probe_pending || state_.probe_in_flight ||
            input_at_ms <= state_.paused_at_ms || input_at_ms > now_ms) return false;
        state_.probe_pending = true;
        // Preserve the cooldown even when queued button events arrive in a burst.
        if (state_.retry_at_ms < now_ms) state_.retry_at_ms = now_ms;
        return true;
    }

private:
    DisplayRecoveryStatus state_;
};

}  // namespace device_ui_internal
