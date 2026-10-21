#include "sdkconfig.h"
#include "memory_diagnostics_ports.h"
#include "runtime/memory_diagnostics.cpp"
#include <thread>
#include <new>
#include <fstream>

// Catch incidental C++ allocation in the failed-allocation callback as well as
// explicit heap_caps calls. Other threads may allocate when std::thread starts.
thread_local size_t host_cpp_allocations = 0;
void* operator new(size_t size) {
    ++host_cpp_allocations;
    if (void* result = std::malloc(size)) return result;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }

using namespace wqn::runtime;
int checks = 0;
const char* host_export_path = nullptr;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL memory line %d: %s\n", __LINE__, #value); std::exit(1); } } while (0)

#if CONFIG_WQN_MEMORY_DIAGNOSTICS
void Reset() {
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
    if (g_history != nullptr) { heap_caps_free(g_history); g_history = nullptr; }
    g_history_dump = {}; g_next_history_us = 0;
#endif
    g_dump_requested = false;
    g_snapshots = {}; g_failures = {};
    g_snapshot_sequence = 0; g_failure_sequence = 0;
    g_next_sample_us = 1000000000; host_logs.clear();
#if CONFIG_WQN_MEMORY_TRACE_ENABLE
    g_trace_started = true;  // The trace scheduler is exercised separately.
#endif
}

void TestFailureCapture() {
    Reset();
    const auto queries = host_heap_queries.load(), allocations = host_allocations.load();
    const auto cpp_allocations = host_cpp_allocations;
    host_now = 1234567; host_isr = true;
    for (unsigned n = 1; n <= 12; ++n) AllocationFailed(n * 100, 0x804, "heap_caps_malloc");
    host_isr = false;
    CHECK(host_cpp_allocations == cpp_allocations);
    CHECK(host_allocations == allocations && host_heap_queries == queries && host_logs.empty());
    CHECK(g_failures.count == 8 && g_failures.lost == 4 && g_failure_sequence == 12);
    Failure f{};
    CHECK(g_failures.Pop(&f));
    CHECK(f.size == 500 && f.caps == 0x804 && f.time_us == 1234567 && f.in_isr);
    CHECK(std::string(f.function) == "heap_caps_malloc" && f.sequence == 5);
    host_now = 5000000;
    PollDiagnostics(host_now);
    CHECK(g_failures.count == 3);
    CHECK(host_logs[0].find("t_ms=1234 bytes=600 caps=0x00000804") != std::string::npos);
    CHECK(host_logs.back().find("t_ms=5000 stage=allocation_failed") != std::string::npos);
    CHECK(host_logs.back().find("failures=12 sample_lost=0 failure_lost=4") != std::string::npos);
}

void TestPeriodicAndScope() {
    Reset(); host_now = 10000000; g_next_sample_us = host_now;
    PollDiagnostics(host_now);
    CHECK(g_snapshot_sequence == 1 && host_logs.size() == 1);
    PollDiagnostics(host_now + kSampleIntervalUs - 1);
    CHECK(g_snapshot_sequence == 1);
    host_now += kSampleIntervalUs;
    PollDiagnostics(host_now);
    CHECK(g_snapshot_sequence == 2);
    auto early_return = [] {
        const MemoryDiagnosticScope scope(MemoryStage::kAiBegin, MemoryStage::kAiEnd);
        return;
    };
    early_return();
    Snapshot begin{}, end{};
    CHECK(g_snapshots.Pop(&begin) && g_snapshots.Pop(&end));
    CHECK(begin.stage == MemoryStage::kAiBegin && end.stage == MemoryStage::kAiEnd);
    CHECK(begin.operation != 0 && begin.operation == end.operation);
    CHECK(begin.internal.free == 100000 && begin.dma.free == 90000 && begin.psram.free == 6000000);
    CHECK(begin.internal.minimum == 60000 && begin.internal.largest == 32000);
}

void TestConcurrentProducers() {
    Reset();
    constexpr unsigned producers = 4, per_producer = 1000;
    std::atomic<unsigned> done{0};
    std::thread threads[producers];
    for (auto& thread : threads) thread = std::thread([&] {
        for (unsigned n = 0; n < per_producer; ++n) {
            RecordMemorySnapshot(MemoryStage::kWifiConnect, n + 1);
            AllocationFailed(n + 1, 0x80c, "heap_caps_calloc");
        }
        ++done;
    });
    uint32_t consumed = 0, last = 0, failures = 0, last_failure = 0;
    do {
        Snapshot snapshot{}; Failure failure{};
        portENTER_CRITICAL(&g_lock);
        const bool sample_found = g_snapshots.Pop(&snapshot);
        const bool failure_found = g_failures.Pop(&failure);
        portEXIT_CRITICAL(&g_lock);
        if (sample_found) {
            CHECK(snapshot.sequence > last && snapshot.internal.free == 100000);
            CHECK(snapshot.operation >= 1 && snapshot.operation <= per_producer);
            last = snapshot.sequence; ++consumed;
        }
        if (failure_found) {
            CHECK(failure.sequence > last_failure && failure.caps == 0x80c);
            CHECK(failure.size >= 1 && failure.size <= per_producer);
            last_failure = failure.sequence; ++failures;
        }
    } while (done != producers);
    for (auto& thread : threads) thread.join();
    CHECK(consumed + g_snapshots.count + g_snapshots.lost == producers * per_producer);
    CHECK(failures + g_failures.count + g_failures.lost == producers * per_producer);
}

void TestStartup() {
    Reset(); host_stack_fail = false; host_task_fail = false;
    InitMemoryDiagnostics();
    CHECK(host_hook == AllocationFailed && host_stack != nullptr);
    CHECK(host_stack_size == 4096 && host_stack_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    heap_caps_free(host_stack);
    host_stack_fail = true;
    const unsigned allocations = host_allocations;
    InitMemoryDiagnostics();
    CHECK(host_stack == nullptr && host_allocations == allocations + 1);
    CHECK(host_logs.back().find("disabled") != std::string::npos);
    host_stack_fail = false; host_task_fail = true;
    InitMemoryDiagnostics();
    CHECK(host_stack == nullptr && host_logs.back().find("task unavailable") != std::string::npos);
    host_task_fail = false;
}

#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
Snapshot HistorySnapshot(MemoryStage stage, int64_t time_us) {
    Snapshot s{};
    s.sequence = ++g_snapshot_sequence; s.time_us = time_us; s.stage = stage;
    s.internal = {100000, 50000, 32000};
    s.dma = {90000, 40000, 30000}; s.psram = {6000000, 5000000, 4000000};
    return s;
}

void StartHistory() {
    InitMemoryDiagnostics();
    CHECK(g_history != nullptr && host_stack != nullptr);
    CHECK(host_history_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    heap_caps_free(host_stack); host_logs.clear();
    g_snapshots = {}; g_snapshot_sequence = 0;
}

void TestOfflineHistory() {
    Reset(); StartHistory();
    // 24 hours plus 3 minutes, with 30-second input. History keeps minute
    // samples while preserving recent event and failure rings independently.
    for (size_t n = 0; n < (kHistorySampleDepth + 3) * 2; ++n)
        ArchiveSnapshot(HistorySnapshot(MemoryStage::kPeriodic, n * 30000000LL), n, 0, 0);
    CHECK(g_history->samples.count == kHistorySampleDepth && g_history->samples.lost == 3);
    const auto first = HistoryCursor(g_history->samples);
    CHECK(g_history->samples.entries[first.index].snapshot.time_us == 180000000LL);
    for (size_t n = 0; n < kHistoryEventDepth + 7; ++n)
        ArchiveSnapshot(HistorySnapshot(MemoryStage::kAiBegin, 90000000000LL + n), n, 2, 3);
    CHECK(g_history->events.count == kHistoryEventDepth && g_history->events.lost == 7);
    for (size_t n = 0; n < kHistoryFailureDepth + 5; ++n)
        g_history->failures.Push({Failure{1000, 221, 0x80c, "heap_caps_malloc", static_cast<uint32_t>(n+1), true}, 4});
    CHECK(g_history->failures.count == kHistoryFailureDepth && g_history->failures.lost == 5);
    const auto allocations = host_allocations.load(), queries = host_heap_queries.load();
    const auto cpp = host_cpp_allocations;
    RequestMemoryDiagnosticsDump();
    CHECK(host_allocations == allocations && host_heap_queries == queries && host_logs.empty());
    CHECK(host_cpp_allocations == cpp);
    PollHistoryDump();
    CHECK(g_history_dump.active && g_history_dump.emitted == kHistoryDumpPerPoll);
    RequestMemoryDiagnosticsDump(); PollHistoryDump();
    CHECK(g_history_dump.id == 1 && g_history_dump.emitted == kHistoryDumpPerPoll * 2);
    for (size_t polls = 0; g_history_dump.active && polls < 200; ++polls) PollHistoryDump();
    CHECK(!g_history_dump.active && g_history_dump.overwritten == 0);
    CHECK(g_history_dump.emitted == kHistorySampleDepth + kHistoryEventDepth + kHistoryFailureDepth);
    CHECK(g_history->samples.count == kHistorySampleDepth && g_history->failures.count == kHistoryFailureDepth);
    CHECK(host_logs.front().find("sample_overwritten=3 event_overwritten=7 failure_overwritten=5") != std::string::npos);
    CHECK(host_logs.back().find("history_end export=1") != std::string::npos);
    if (host_export_path != nullptr) {
        std::ofstream exported(host_export_path);
        for (const auto& line : host_logs) exported << "I (0) mem_diag: " << line << '\n';
    }
    uint32_t last_sequence = 0;
    for (const auto& line : host_logs) {
        if (line.find("history_sample seq=") == 0) {
            unsigned sequence = 0;
            CHECK(std::sscanf(line.c_str(), "history_sample seq=%u", &sequence) == 1);
            CHECK(sequence > last_sequence); last_sequence = sequence;
        }
        if (line.find("history_alloc_failed") == 0)
            CHECK(line.find("isr=1 failure_lost=4 export=1") != std::string::npos);
    }
    RequestMemoryDiagnosticsDump(); PollHistoryDump();
    CHECK(g_history_dump.id == 2 && g_history_dump.active);
    CHECK(host_allocations == allocations);  // No second buffer for export.
}

void TestHistoryOverwrittenDuringExport() {
    Reset(); StartHistory();
    for (size_t n = 0; n < kHistoryEventDepth; ++n)
        ArchiveSnapshot(HistorySnapshot(MemoryStage::kWifiConnect, n), 0, 0, 0);
    RequestMemoryDiagnosticsDump(); PollHistoryDump();
    for (size_t n = 0; n < kHistoryEventDepth * 2; ++n)
        ArchiveSnapshot(HistorySnapshot(MemoryStage::kWifiConnect, 1000+n), 0, 0, 0);
    PollHistoryDump();
    CHECK(!g_history_dump.active);
    CHECK(g_history_dump.emitted == kHistoryDumpPerPoll);
    CHECK(g_history_dump.overwritten == kHistoryEventDepth - kHistoryDumpPerPoll);
    // Export is a captured range; newly generated data is retained for a
    // subsequent export, never silently substituted into the current one.
    CHECK(g_history->events.count == kHistoryEventDepth);
    CHECK(host_logs.back().find("overwritten_during_export=112") != std::string::npos);
}

void TestHistoryAllocationFailure() {
    Reset(); host_history_fail = true;
    const auto allocations = host_history_allocations;
    InitMemoryDiagnostics();
    CHECK(g_history == nullptr && host_stack != nullptr);
    CHECK(host_history_allocations == allocations + 1);
    CHECK(host_history_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    RequestMemoryDiagnosticsDump(); PollHistoryDump();
    CHECK(host_logs.back().find("history_unavailable") != std::string::npos);
    heap_caps_free(host_stack); host_history_fail = false;
}
#endif

#if CONFIG_WQN_MEMORY_TRACE_ENABLE
void TestTraceWindow() {
    host_logs.clear(); host_trace_starts = 0; host_trace_stops = 0; host_trace_gets = 0;
    g_trace_started = false; g_trace_running = false;
    g_trace_dump_count = 0; g_trace_dump_index = 0;
    PollTrace(1999999);
    CHECK(host_trace_starts == 0);
    PollTrace(2000000);
    CHECK(host_trace_starts == 1 && g_trace_running && host_trace_gets == 0);
    PollTrace(4999999);
    CHECK(host_trace_stops == 0);
    PollTrace(5000000);
    CHECK(host_trace_stops == 1 && !g_trace_running && host_trace_gets == 4);
    CHECK(host_logs[1].find("outstanding=6 capacity=16 overflow=1") != std::string::npos);
    CHECK(host_logs.back().find("0x42001234") != std::string::npos);
    PollTrace(6000000); PollTrace(100000000);
    CHECK(host_trace_gets == 6 && host_trace_starts == 1 && host_trace_stops == 1);
    g_trace_started = false; host_trace_init_fail = true;
    PollTrace(200000000); PollTrace(201000000);
    CHECK(host_trace_starts == 1 && g_trace_started && !g_trace_running);
    host_trace_init_fail = false; g_trace_started = false;
    host_trace_stop_fail = true;
    PollTrace(300000000); PollTrace(303000000); PollTrace(304000000);
    CHECK(host_trace_stops == 2 && !g_trace_running);
    CHECK(host_logs.back().find("trace_stop_failed") != std::string::npos);
}
#endif
#endif

int main(int argc, char** argv) {
    if (argc > 1) host_export_path = argv[1];
#if CONFIG_WQN_MEMORY_DIAGNOSTICS
    TestFailureCapture(); TestPeriodicAndScope(); TestConcurrentProducers(); TestStartup();
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
    TestOfflineHistory(); TestHistoryOverwrittenDuringExport(); TestHistoryAllocationFailure();
#endif
#if CONFIG_WQN_MEMORY_TRACE_ENABLE
    TestTraceWindow();
#endif
    Reset();
#else
    InitMemoryDiagnostics();
    const MemoryDiagnosticScope scope(MemoryStage::kAiBegin, MemoryStage::kAiEnd);
    RecordMemorySnapshot(MemoryStage::kPeriodic);
    RequestMemoryDiagnosticsDump();
    CHECK(host_logs.empty() && host_allocations == 0 && host_heap_queries == 0);
#endif
    std::printf("RAM diagnostics: %d checks PASS (production module, host ports)\n", checks);
}
