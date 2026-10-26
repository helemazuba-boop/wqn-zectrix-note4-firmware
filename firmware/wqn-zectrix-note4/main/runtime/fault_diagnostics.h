#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace wqn::runtime {

enum class FaultDomain : uint8_t { kOther, kBoot, kStorage, kAudio, kRtc, kNetwork, kAi, kUi, kPersist, kSync };
enum class FaultPhase : uint8_t { kReportedError, kBootInit, kStorageExecution, kStorageQueueTimeout };
enum class BootPhase : uint32_t { kPlatform, kStorage, kServices, kUi, kStartupComplete };
constexpr size_t kFaultRecordDepth = 4;
constexpr int32_t kFaultCodeUnavailable = std::numeric_limits<int32_t>::min();

// Fixed numeric context only: no error text, credentials, server request IDs,
// paths or pointers. Build strings are the compile-time firmware identity.
// Uptime/clock epoch belong to boot_id, never to the current boot implicitly.
struct FaultRecord {
    uint32_t sequence;
    uint32_t boot_id;
    int64_t uptime_ms;
    int64_t unix_sec;
    uint32_t operation;
    uint32_t generation;
    uint32_t clock_epoch;
    int32_t code;
    char build[16];
    FaultDomain domain;
    FaultPhase phase;
    uint8_t clock_source;
    uint8_t reset_reason;
    uint8_t boot_phase;
    uint8_t rtc_continuity;
    uint8_t reserved[2];
};

struct FaultDiagnosticsSnapshot {
    uint32_t boot_id;
    uint32_t reset_reason;
    BootPhase boot_phase;
    uint32_t previous_boot_id;
    BootPhase previous_boot_phase;
    uint32_t recovered;
    uint32_t integrity_rejections; // cumulative rejected nonempty RTC copies
    uint32_t reports;              // saturating report count, not unique incidents
    uint32_t overwritten;          // intentional replacement of the oldest report
    uint32_t count;
    char build[16];
    char previous_build[16];
    FaultRecord records[kFaultRecordDepth];
};

// Task context only. Silent bounded RAM/RTC work under a short spinlock;
// no allocation, heap walks, Flash writes, worker or periodic sampling.
// Lazy initialization supports RecordError callers before normal assembly.
void InitFaultDiagnostics();
void SetFaultBootPhase(BootPhase phase);
void RecordFault(FaultDomain domain, FaultPhase phase, int32_t code,
                 uint32_t operation = 0, uint32_t generation = 0);
// Legacy error text remains only in the existing ordinary-RAM error ring.
void RecordLegacyFault(const char* tag);
// Copies a coherent snapshot, newest min(capacity=4, reports) records oldest
// first. Does not clear evidence. Returns false only for a null destination.
bool CopyFaultDiagnostics(FaultDiagnosticsSnapshot* out);
// Explicit task-context log export (boot diagnostics also calls it once).
void DumpFaultDiagnosticsToLog();

}  // namespace wqn::runtime
