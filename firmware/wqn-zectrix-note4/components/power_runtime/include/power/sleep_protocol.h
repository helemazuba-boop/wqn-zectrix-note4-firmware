#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace wqn::power {

enum class SleepMode : uint8_t {
    kIdle = 0,
    kBatteryEmergency,
};

enum class SleepService : uint8_t {
    kDisplay = 0,
    kStorage,
    kAudio,
    kConnectivity,
    kCount,
};

enum class SleepPrepareStatus : uint8_t {
    kReady = 0,
    kDenied,
    kTimedOut,
};

struct PrepareSleepCommand {
    uint32_t generation = 0;
    int64_t deadline_us = 0;
    SleepMode mode = SleepMode::kIdle;
};

struct PrepareSleepResult {
    uint32_t generation = 0;
    SleepService service = SleepService::kDisplay;
    SleepPrepareStatus status = SleepPrepareStatus::kDenied;
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

constexpr size_t kSleepServiceCount = static_cast<size_t>(SleepService::kCount);
using PrepareSleepResults = std::array<PrepareSleepResult, kSleepServiceCount>;

const char* SleepModeName(SleepMode mode);
const char* SleepServiceName(SleepService service);
const char* SleepPrepareStatusName(SleepPrepareStatus status);

}  // namespace wqn::power
