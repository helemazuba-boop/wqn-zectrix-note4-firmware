#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "power/sleep_protocol.h"
#include "runtime/sleep_coordinator.h"

namespace wqn::services {

enum class ConnectivityState : uint8_t {
    kOff,
    kProvisioning,
    kConnecting,
    kWaitingIp,
    kOnline,
    kOnlineIdleTail,
    kBackoff,
    kQuiescing,
};

enum class ConnectivityDemandReason : uint8_t {
    kAiInteractive,
    kCloudInteractive,
    kSyncInteractive,
    kSyncBackground,
    kBulkBackground,
};

enum class ConnectivityDemandPriority : uint8_t {
    kNone,
    kBackground,
    kInteractive,
};

enum class ConnectivityWaitResult : uint8_t {
    kOnline,
    kNeedsProvisioning,
    kAuthFailed,
    kUnavailable,
    kTimedOut,
    kQuiescing,
    kCancelled,
};

struct ConnectivityDemandTicket {
    uint32_t id = 0;
    uint32_t activation_generation = 0;
    ConnectivityDemandReason reason = ConnectivityDemandReason::kSyncBackground;
};

class ConnectivityDemand {
public:
    ConnectivityDemand() = default;
    ~ConnectivityDemand();

    ConnectivityDemand(const ConnectivityDemand&) = delete;
    ConnectivityDemand& operator=(const ConnectivityDemand&) = delete;
    ConnectivityDemand(ConnectivityDemand&& other) noexcept;
    ConnectivityDemand& operator=(ConnectivityDemand&& other) noexcept;

    explicit operator bool() const { return id_ != 0; }
    uint32_t id() const { return id_; }
    uint32_t activation_generation() const { return activation_generation_; }
    ConnectivityDemandTicket ticket() const
    {
        return {id_, activation_generation_, reason_};
    }
    ConnectivityDemandReason reason() const { return reason_; }
    void Reset();

private:
    friend ConnectivityDemand AcquireConnectivityDemand(
        ConnectivityDemandReason reason,
        const char* owner,
        const char* file,
        int line);

    ConnectivityDemand(
        uint32_t id,
        uint32_t activation_generation,
        ConnectivityDemandReason reason,
        runtime::SleepLease&& sleep_lease);

    uint32_t id_ = 0;
    uint32_t activation_generation_ = 0;
    ConnectivityDemandReason reason_ = ConnectivityDemandReason::kSyncBackground;
    runtime::SleepLease sleep_lease_;
};

struct ConnectivitySnapshot {
    ConnectivityState state = ConnectivityState::kOff;
    bool online = false;
    int rssi = 0;
    // SSID of the slot currently connected or being attempted ("" when unknown,
    // e.g. compile-time developer credentials).
    char active_ssid[33] = {};
    // SSID of the other stored credential ("" when there is no second slot).
    char backup_ssid[33] = {};
    // True when a second stored credential is available for failover.
    bool has_backup = false;
    uint8_t demand_count = 0;
    uint32_t demand_mask = 0;
    ConnectivityDemandPriority demand_priority = ConnectivityDemandPriority::kNone;
    uint8_t backoff_rounds = 0;
    bool backoff_suspended = false;
};

ConnectivityDemand AcquireConnectivityDemand(
    ConnectivityDemandReason reason,
    const char* owner,
    const char* file,
    int line);
ConnectivityWaitResult WaitForConnectivity(
    const ConnectivityDemand& demand,
    TickType_t timeout);
ConnectivityWaitResult WaitForConnectivity(
    ConnectivityDemandTicket ticket,
    TickType_t timeout);
esp_err_t ConnectivityWaitResultToEspErr(ConnectivityWaitResult result);
const char* ConnectivityWaitResultName(ConnectivityWaitResult result);

// Compatibility readiness wait for API internals. A caller must already own
// a ConnectivityDemand; this function never creates a hidden radio lifetime.
esp_err_t WaitForConnectivity(TickType_t timeout);

bool IsConnectivityOnline();
int GetConnectivityRssi();
void SetConnectivityProvisioning();
ConnectivitySnapshot GetConnectivitySnapshot();

esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand& command);
void RollbackConnectivityAfterSleepAbort();

}  // namespace wqn::services
