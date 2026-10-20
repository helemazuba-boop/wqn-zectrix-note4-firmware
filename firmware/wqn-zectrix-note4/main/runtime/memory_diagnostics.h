#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace wqn::runtime {

enum class MemoryStage : uint8_t {
    kBoot, kPeriodic, kAllocationFailed,
    kWifiConnect, kWifiDisconnected, kWifiGotIp,
    kCloudInteractiveBegin, kCloudInteractiveEnd,
    kCloudBulkBegin, kCloudBulkEnd,
    kAiBegin, kAiEnd, kAgentBegin, kAgentEnd, kAudioBegin, kAudioEnd,
};

#if CONFIG_WQN_MEMORY_DIAGNOSTICS
// Called once at boot. Diagnostic startup failure never stops business startup.
void InitMemoryDiagnostics();
// Task context only: query heaps, enqueue a fixed-size value, never print or
// allocate. The diagnostic worker serializes output; a full queue drops oldest.
void RecordMemorySnapshot(MemoryStage stage, uint32_t operation = 0);
uint32_t NextMemoryOperation();
// Nonblocking, task context. The diagnostic worker exports the retained PSRAM
// history to serial in bounded batches. Repeated requests never clear history.
void RequestMemoryDiagnosticsDump();
#else
inline void InitMemoryDiagnostics() {}
inline void RecordMemorySnapshot(MemoryStage, uint32_t = 0) {}
inline uint32_t NextMemoryOperation() { return 0; }
inline void RequestMemoryDiagnosticsDump() {}
#endif

class MemoryDiagnosticScope {
public:
    MemoryDiagnosticScope(MemoryStage begin, MemoryStage end)
        : end_(end), operation_(NextMemoryOperation()) {
        RecordMemorySnapshot(begin, operation_);
    }
    ~MemoryDiagnosticScope() { RecordMemorySnapshot(end_, operation_); }
    MemoryDiagnosticScope(const MemoryDiagnosticScope&) = delete;
    MemoryDiagnosticScope& operator=(const MemoryDiagnosticScope&) = delete;
private:
    MemoryStage end_;
    uint32_t operation_;
};

}  // namespace wqn::runtime
