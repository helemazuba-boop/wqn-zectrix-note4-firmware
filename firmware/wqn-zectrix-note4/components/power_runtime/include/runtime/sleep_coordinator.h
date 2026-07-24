#pragma once

#include <cstdint>

#include "esp_err.h"

namespace wqn::runtime {

enum class SleepBlocker : uint8_t {
    kDisplay = 0,
    kTodoCloud,
    kWordCloud,
    kNoteCloud,
    kOnlineSync,
    kProvisioning,
    kAudio,
    kAiSession,
    kFlashSession,
    kStorage,
    kConnectivity,
    kUsbPower,
    kCount,
};

const char* SleepBlockerName(SleepBlocker blocker);

// Must run once during app_main before any service task can acquire a lease.
// The shared ESP_PM_NO_LIGHT_SLEEP lock is held while at least one lease exists.
esp_err_t InitSleepCoordinator();

// Short-lived ownership token for CPU-bound foreground work. This requests
// the configured maximum CPU frequency without disabling DFS globally. Keep
// its scope narrow; work that must also block sleep still needs a SleepLease.
class CpuPerformanceLease {
public:
    CpuPerformanceLease() = default;
    ~CpuPerformanceLease();

    CpuPerformanceLease(const CpuPerformanceLease&) = delete;
    CpuPerformanceLease& operator=(const CpuPerformanceLease&) = delete;

    CpuPerformanceLease(CpuPerformanceLease&& other) noexcept;
    CpuPerformanceLease& operator=(CpuPerformanceLease&& other) noexcept;

    static CpuPerformanceLease TryAcquire();

    explicit operator bool() const { return active_; }
    void Reset();

private:
    explicit CpuPerformanceLease(bool active) : active_(active) {}

    bool active_ = false;
};

// Move-only ownership token for work that must finish before deep sleep.
// Acquisition fails once the power coordinator starts quiescing the system.
class SleepLease {
public:
    SleepLease() = default;
    ~SleepLease();

    SleepLease(const SleepLease&) = delete;
    SleepLease& operator=(const SleepLease&) = delete;

    SleepLease(SleepLease&& other) noexcept;
    SleepLease& operator=(SleepLease&& other) noexcept;

    static SleepLease TryAcquire(
        SleepBlocker blocker,
        const char* holder = "unspecified",
        const char* file = "unknown",
        int line = 0);

    explicit operator bool() const { return active_; }
    void Reset();

private:
    explicit SleepLease(SleepBlocker blocker, uint8_t slot, uint32_t lease_id)
        : blocker_(blocker), slot_(slot), lease_id_(lease_id), active_(true)
    {
    }

    SleepBlocker blocker_ = SleepBlocker::kDisplay;
    uint8_t slot_ = UINT8_MAX;
    uint32_t lease_id_ = 0;
    bool active_ = false;
};

// Atomically closes acquisition, then succeeds only when no leases remain.
// A successful call must be paired with CancelSleepQuiesce(generation) if
// sleep is aborted. Deep sleep itself resets the process state.
bool TryBeginSleepQuiesce(uint32_t generation);
// Emergency shutdown closes acquisition even if work is still draining.
bool BeginEmergencySleepQuiesce(uint32_t generation);
void CancelSleepQuiesce(uint32_t generation);
bool IsSleepQuiescing();
uint32_t CurrentSleepGeneration();
bool HasActiveSleepBlockers();
uint32_t ActiveSleepBlockerCount(SleepBlocker blocker);
void LogLongHeldSleepLeases(int64_t now_us, int64_t warning_after_us);

}  // namespace wqn::runtime
