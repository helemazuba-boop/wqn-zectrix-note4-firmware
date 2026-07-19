#include "power/sleep_protocol.h"

namespace wqn::power {

const char* SleepModeName(SleepMode mode)
{
    switch (mode) {
        case SleepMode::kIdle:
            return "idle";
        case SleepMode::kBatteryEmergency:
            return "battery-emergency";
        default:
            return "unknown";
    }
}

const char* SleepServiceName(SleepService service)
{
    switch (service) {
        case SleepService::kDisplay:
            return "display";
        case SleepService::kStorage:
            return "storage";
        case SleepService::kAudio:
            return "audio";
        case SleepService::kConnectivity:
            return "connectivity";
        case SleepService::kCount:
        default:
            return "unknown";
    }
}

const char* SleepPrepareStatusName(SleepPrepareStatus status)
{
    switch (status) {
        case SleepPrepareStatus::kReady:
            return "ready";
        case SleepPrepareStatus::kDenied:
            return "denied";
        case SleepPrepareStatus::kTimedOut:
            return "timed-out";
        default:
            return "unknown";
    }
}

}  // namespace wqn::power
