#include "runtime/memory_diagnostics.h"

#if CONFIG_WQN_MEMORY_DIAGNOSTICS

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <new>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_WQN_MEMORY_TRACE_ENABLE
#include "esp_heap_trace.h"
#include "esp_memory_utils.h"
#endif

namespace wqn::runtime {
namespace {

constexpr char kTag[] = "mem_diag";
constexpr size_t kSnapshotDepth = 16;
constexpr size_t kFailureDepth = 8;
constexpr size_t kDrainPerPoll = 4;
constexpr size_t kStackBytes = 4096;
constexpr int64_t kSampleIntervalUs = CONFIG_WQN_MEMORY_SAMPLE_SECONDS * 1000000LL;

// These bounded rings and their lock stay internal, including when a failed
// allocation originates in an ISR with the PSRAM/flash cache unavailable.
template <typename T, size_t Depth>
struct Ring {
    T entries[Depth]{};
    size_t head = 0;
    size_t count = 0;
    uint32_t lost = 0;

    void IRAM_ATTR Push(const T& entry) {
        entries[head] = entry;
        head = (head + 1) % Depth;
        if (count == Depth) ++lost;
        else ++count;
    }
    bool Pop(T* out) {
        if (count == 0) return false;
        *out = entries[(head + Depth - count) % Depth];
        --count;
        return true;
    }
};

struct Pool {
    size_t free;
    size_t minimum;
    size_t largest;
};

struct Snapshot {
    int64_t time_us;
    uint32_t sequence;
    uint32_t operation;
    MemoryStage stage;
    Pool internal;
    Pool dma;
    Pool psram;
};

struct Failure {
    int64_t time_us;
    size_t size;
    uint32_t caps;
    const char* function;  // IDF supplies a static allocator function name.
    uint32_t sequence;
    bool in_isr;
};

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
Ring<Snapshot, kSnapshotDepth> g_snapshots;
Ring<Failure, kFailureDepth> g_failures;
uint32_t g_snapshot_sequence = 0;
uint32_t g_failure_sequence = 0;
std::atomic<uint32_t> g_operation{0};
StaticTask_t g_task_storage;
int64_t g_next_sample_us = 0;  // Diagnostic task only.
std::atomic<bool> g_dump_requested{false};

#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
constexpr size_t kHistorySampleDepth = CONFIG_WQN_MEMORY_HISTORY_HOURS * 60;
constexpr size_t kHistoryEventDepth = 128;
constexpr size_t kHistoryFailureDepth = 32;
constexpr int64_t kHistoryIntervalUs = 60 * 1000000LL;
constexpr size_t kHistoryDumpPerPoll = 16;

struct HistoryRecord {
    Snapshot snapshot;
    uint32_t failures;
    uint32_t sample_lost;
    uint32_t failure_lost;
};

struct HistoryFailure {
    Failure failure;
    uint32_t queue_lost;
};

struct History {
    Ring<HistoryRecord, kHistorySampleDepth> samples;
    Ring<HistoryRecord, kHistoryEventDepth> events;
    Ring<HistoryFailure, kHistoryFailureDepth> failures;
};

// Only DiagnosticTask accesses history after startup. Producers/ISRs keep
// using the internal queues and never dereference this PSRAM pointer.
History* g_history = nullptr;
int64_t g_next_history_us = 0;

struct DumpCursor {
    size_t index = 0;
    size_t remaining = 0;
    uint32_t last_sequence = 0;
};

struct HistoryDump {
    bool active = false;
    uint32_t id = 0;
    DumpCursor samples;
    DumpCursor events;
    DumpCursor failures;
    uint32_t overwritten = 0;
    uint32_t emitted = 0;
};
HistoryDump g_history_dump;

uint32_t Sequence(const HistoryRecord& r) { return r.snapshot.sequence; }
uint32_t Sequence(const HistoryFailure& r) { return r.failure.sequence; }

template <typename T, size_t Depth>
DumpCursor HistoryCursor(const Ring<T, Depth>& ring)
{
    return {(ring.head + Depth - ring.count) % Depth, ring.count,
            ring.count ? Sequence(ring.entries[(ring.head + Depth - 1) % Depth]) : 0};
}

template <typename T, size_t Depth>
void AdvanceCursor(DumpCursor* cursor)
{
    cursor->index = (cursor->index + 1) % Depth;
    --cursor->remaining;
}

template <typename T, size_t Depth>
const T* PeekHistory(const Ring<T, Depth>& ring, DumpCursor* cursor)
{
    while (cursor->remaining != 0) {
        const T& entry = ring.entries[cursor->index];
        // New records may overwrite a slot during a long export. Do not emit
        // post-request data as part of the captured history; report the gap.
        if (static_cast<int32_t>(Sequence(entry) - cursor->last_sequence) <= 0) return &entry;
        AdvanceCursor<T, Depth>(cursor);
        ++g_history_dump.overwritten;
    }
    return nullptr;
}

void ArchiveSnapshot(const Snapshot& s, uint32_t failures, uint32_t sample_lost,
                     uint32_t failure_lost)
{
    if (g_history == nullptr) return;
    const HistoryRecord record{s, failures, sample_lost, failure_lost};
    if (s.stage == MemoryStage::kBoot || s.stage == MemoryStage::kPeriodic) {
        if (s.stage == MemoryStage::kBoot || s.time_us >= g_next_history_us) {
            g_history->samples.Push(record);
            g_next_history_us = s.time_us + kHistoryIntervalUs;
        }
    } else {
        g_history->events.Push(record);
    }
}
#endif

void IRAM_ATTR AllocationFailed(size_t size, uint32_t caps, const char* function)
{
    // No logging, heap queries, allocation, task-name lookup or formatting in
    // this hook. The short ISR-safe spinlock protects only internal scalars.
    const int64_t now = esp_timer_get_time();
    const bool in_isr = xPortInIsrContext();
    portENTER_CRITICAL_SAFE(&g_lock);
    g_failures.Push(Failure{now, size, caps, function, ++g_failure_sequence, in_isr});
    portEXIT_CRITICAL_SAFE(&g_lock);
}

Pool ReadPool(uint32_t caps)
{
    multi_heap_info_t info{};
    heap_caps_get_info(&info, caps);
    return {info.total_free_bytes, info.minimum_free_bytes, info.largest_free_block};
}

const char* StageName(MemoryStage stage)
{
    switch (stage) {
        case MemoryStage::kBoot: return "boot";
        case MemoryStage::kPeriodic: return "periodic";
        case MemoryStage::kAllocationFailed: return "allocation_failed";
        case MemoryStage::kWifiConnect: return "wifi_connect";
        case MemoryStage::kWifiDisconnected: return "wifi_disconnected";
        case MemoryStage::kWifiGotIp: return "wifi_got_ip";
        case MemoryStage::kCloudInteractiveBegin: return "cloud_interactive_begin";
        case MemoryStage::kCloudInteractiveEnd: return "cloud_interactive_end";
        case MemoryStage::kCloudBulkBegin: return "cloud_bulk_begin";
        case MemoryStage::kCloudBulkEnd: return "cloud_bulk_end";
        case MemoryStage::kAiBegin: return "ai_begin";
        case MemoryStage::kAiEnd: return "ai_end";
        case MemoryStage::kAgentBegin: return "agent_begin";
        case MemoryStage::kAgentEnd: return "agent_end";
        case MemoryStage::kAudioBegin: return "audio_begin";
        case MemoryStage::kAudioEnd: return "audio_end";
    }
    return "unknown";
}

void LogSnapshot(const Snapshot& s, uint32_t failures, uint32_t snapshot_lost,
                 uint32_t failure_lost, const char* kind = "sample", uint32_t export_id = 0)
{
    ESP_LOGI(kTag,
        "%s seq=%" PRIu32 " t_ms=%" PRId64 " stage=%s op=%" PRIu32
        " internal_free=%u internal_min=%u internal_largest=%u"
        " dma_free=%u dma_min=%u dma_largest=%u"
        " psram_free=%u psram_min=%u psram_largest=%u"
        " failures=%" PRIu32 " sample_lost=%" PRIu32 " failure_lost=%" PRIu32 " export=%" PRIu32,
        kind, s.sequence, s.time_us / 1000, StageName(s.stage), s.operation,
        static_cast<unsigned>(s.internal.free), static_cast<unsigned>(s.internal.minimum),
        static_cast<unsigned>(s.internal.largest), static_cast<unsigned>(s.dma.free),
        static_cast<unsigned>(s.dma.minimum), static_cast<unsigned>(s.dma.largest),
        static_cast<unsigned>(s.psram.free), static_cast<unsigned>(s.psram.minimum),
        static_cast<unsigned>(s.psram.largest), failures, snapshot_lost, failure_lost, export_id);
}

void LogFailure(const Failure& failure, uint32_t lost,
                const char* kind = "alloc_failed", uint32_t export_id = 0)
{
    ESP_LOGW(kTag,
        "%s seq=%" PRIu32 " t_ms=%" PRId64 " bytes=%u caps=0x%08" PRIx32
        " function=%s isr=%u failure_lost=%" PRIu32 " export=%" PRIu32,
        kind, failure.sequence, failure.time_us / 1000, static_cast<unsigned>(failure.size),
        failure.caps, failure.function ? failure.function : "unknown", failure.in_isr, lost, export_id);
}

void PollHistoryDump()
{
    const bool requested = g_dump_requested.exchange(false, std::memory_order_acq_rel);
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
    if (requested) {
        if (g_history == nullptr) {
            ESP_LOGW(kTag, "history_unavailable: PSRAM history was not allocated");
        } else if (g_history_dump.active) {
            ESP_LOGI(kTag, "history_busy export=%" PRIu32, g_history_dump.id);
        } else {
            g_history_dump.active = true;
            ++g_history_dump.id;
            g_history_dump.samples = HistoryCursor(g_history->samples);
            g_history_dump.events = HistoryCursor(g_history->events);
            g_history_dump.failures = HistoryCursor(g_history->failures);
            g_history_dump.overwritten = 0;
            g_history_dump.emitted = 0;
            ESP_LOGI(kTag,
                "history_begin export=%" PRIu32 " t_ms=%" PRId64
                " samples=%u events=%u failures=%u sample_overwritten=%" PRIu32
                " event_overwritten=%" PRIu32 " failure_overwritten=%" PRIu32,
                g_history_dump.id, esp_timer_get_time() / 1000,
                static_cast<unsigned>(g_history->samples.count), static_cast<unsigned>(g_history->events.count),
                static_cast<unsigned>(g_history->failures.count), g_history->samples.lost,
                g_history->events.lost, g_history->failures.lost);
        }
    }
    if (!g_history_dump.active) return;
    for (size_t n = 0; n < kHistoryDumpPerPoll; ++n) {
        const HistoryRecord* sample = PeekHistory(g_history->samples, &g_history_dump.samples);
        const HistoryRecord* event = PeekHistory(g_history->events, &g_history_dump.events);
        const HistoryFailure* failure = PeekHistory(g_history->failures, &g_history_dump.failures);
        if (sample == nullptr && event == nullptr && failure == nullptr) {
            ESP_LOGI(kTag, "history_end export=%" PRIu32 " emitted=%" PRIu32
                " overwritten_during_export=%" PRIu32,
                g_history_dump.id, g_history_dump.emitted, g_history_dump.overwritten);
            g_history_dump.active = false;
            break;
        }
        if (failure != nullptr) {
            LogFailure(failure->failure, failure->queue_lost, "history_alloc_failed", g_history_dump.id);
            AdvanceCursor<HistoryFailure, kHistoryFailureDepth>(&g_history_dump.failures);
        } else if (sample != nullptr && (event == nullptr ||
                   static_cast<int32_t>(sample->snapshot.sequence - event->snapshot.sequence) < 0)) {
            LogSnapshot(sample->snapshot, sample->failures, sample->sample_lost,
                        sample->failure_lost, "history_sample", g_history_dump.id);
            AdvanceCursor<HistoryRecord, kHistorySampleDepth>(&g_history_dump.samples);
        } else {
            LogSnapshot(event->snapshot, event->failures, event->sample_lost,
                        event->failure_lost, "history_sample", g_history_dump.id);
            AdvanceCursor<HistoryRecord, kHistoryEventDepth>(&g_history_dump.events);
        }
        ++g_history_dump.emitted;
    }
#else
    if (requested) ESP_LOGI(kTag, "history_unavailable: offline history is disabled");
#endif
}

#if CONFIG_WQN_MEMORY_TRACE_ENABLE
// Optional internal storage captures ISR allocations too. No trace buffer or
// allocation/free instrumentation is enabled by the normal firmware profile.
heap_trace_record_t g_trace_records[CONFIG_WQN_MEMORY_TRACE_RECORDS];
bool g_trace_started = false;
bool g_trace_running = false;
int64_t g_trace_stop_us = 0;
size_t g_trace_dump_index = 0;
size_t g_trace_dump_count = 0;

void PollTrace(int64_t now)
{
    if (!g_trace_started && now >= CONFIG_WQN_MEMORY_TRACE_DELAY_SECONDS * 1000000LL) {
        g_trace_started = true;  // Exactly one window per boot, including errors.
        esp_err_t result = heap_trace_init_standalone(
            g_trace_records, CONFIG_WQN_MEMORY_TRACE_RECORDS);
        if (result == ESP_OK) result = heap_trace_start(HEAP_TRACE_LEAKS);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "trace_start_failed error=%s", esp_err_to_name(result));
            return;
        }
        g_trace_running = true;
        g_trace_stop_us = now + CONFIG_WQN_MEMORY_TRACE_SECONDS * 1000000LL;
        ESP_LOGI(kTag, "trace_begin t_ms=%" PRId64 " seconds=%u capacity=%u buffer_bytes=%u",
            now / 1000, CONFIG_WQN_MEMORY_TRACE_SECONDS, CONFIG_WQN_MEMORY_TRACE_RECORDS,
            static_cast<unsigned>(sizeof(g_trace_records)));
    }
    if (g_trace_running && now >= g_trace_stop_us) {
        const esp_err_t result = heap_trace_stop();
        // IDF returns INVALID_STATE if already stopped. Do not retry forever
        // or produce an unbounded error stream if another diagnostic stopped it.
        g_trace_running = false;
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "trace_stop_failed error=%s", esp_err_to_name(result));
            return;
        }
        heap_trace_summary_t summary{};
        if (heap_trace_summary(&summary) == ESP_OK) {
            g_trace_dump_count = summary.count;
            ESP_LOGI(kTag,
                "trace_end t_ms=%" PRId64 " outstanding=%u capacity=%u overflow=%u allocations=%u frees=%u",
                now / 1000, static_cast<unsigned>(summary.count),
                static_cast<unsigned>(summary.capacity), static_cast<unsigned>(summary.has_overflowed),
                static_cast<unsigned>(summary.total_allocations), static_cast<unsigned>(summary.total_frees));
        }
    }
    for (size_t n = 0; n < kDrainPerPoll && g_trace_dump_index < g_trace_dump_count; ++n) {
        heap_trace_record_t record{};
        const size_t index = g_trace_dump_index++;
        if (heap_trace_get(index, &record) != ESP_OK) continue;
        char callers[CONFIG_HEAP_TRACING_STACK_DEPTH * 12 + 1]{};
        size_t used = 0;
        for (size_t frame = 0; frame < CONFIG_HEAP_TRACING_STACK_DEPTH; ++frame) {
            const int written = std::snprintf(callers + used, sizeof(callers) - used,
                " %p", record.alloced_by[frame]);
            if (written < 0 || static_cast<size_t>(written) >= sizeof(callers) - used) break;
            used += static_cast<size_t>(written);
        }
        ESP_LOGI(kTag, "trace_record index=%u address=%p bytes=%u pool=%s callers=%s",
            static_cast<unsigned>(index), record.address, static_cast<unsigned>(record.size),
            esp_ptr_external_ram(record.address) ? "psram" : "internal", callers);
    }
}
#endif

void PollDiagnostics(int64_t now)
{
    if (now >= g_next_sample_us) {
        RecordMemorySnapshot(MemoryStage::kPeriodic);
        g_next_sample_us = now + kSampleIntervalUs;
    }
    bool saw_failure = false;
    for (size_t n = 0; n < kDrainPerPoll; ++n) {
        Failure failure{};
        portENTER_CRITICAL(&g_lock);
        const bool found = g_failures.Pop(&failure);
        const uint32_t lost = g_failures.lost;
        portEXIT_CRITICAL(&g_lock);
        if (!found) break;
        saw_failure = true;
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
        if (g_history != nullptr) g_history->failures.Push({failure, lost});
#endif
        LogFailure(failure, lost);
    }
    // Explicitly a later observation, not a claim about heap state at failure.
    if (saw_failure) RecordMemorySnapshot(MemoryStage::kAllocationFailed);
    for (size_t n = 0; n < kDrainPerPoll; ++n) {
        Snapshot snapshot{};
        portENTER_CRITICAL(&g_lock);
        const bool found = g_snapshots.Pop(&snapshot);
        const uint32_t failures = g_failure_sequence;
        const uint32_t snapshot_lost = g_snapshots.lost;
        const uint32_t failure_lost = g_failures.lost;
        portEXIT_CRITICAL(&g_lock);
        if (!found) break;
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
        ArchiveSnapshot(snapshot, failures, snapshot_lost, failure_lost);
#endif
        LogSnapshot(snapshot, failures, snapshot_lost, failure_lost);
    }
#if CONFIG_WQN_MEMORY_TRACE_ENABLE
    PollTrace(now);
#endif
    PollHistoryDump();
}

void DiagnosticTask(void*)
{
    for (;;) {
        PollDiagnostics(esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

uint32_t NextMemoryOperation()
{
    return g_operation.fetch_add(1, std::memory_order_relaxed) + 1;
}

void RequestMemoryDiagnosticsDump()
{
    g_dump_requested.store(true, std::memory_order_release);
}

void RecordMemorySnapshot(MemoryStage stage, uint32_t operation)
{
    Snapshot snapshot{};
    snapshot.time_us = esp_timer_get_time();
    snapshot.operation = operation;
    snapshot.stage = stage;
    // INTERNAL alone also includes non-byte-addressable IRAM. Select the pool
    // usable by strings/packets; DMA is an overlapping view, never additive.
    snapshot.internal = ReadPool(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot.dma = ReadPool(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    snapshot.psram = ReadPool(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&g_lock);
    snapshot.sequence = ++g_snapshot_sequence;
    g_snapshots.Push(snapshot);
    portEXIT_CRITICAL(&g_lock);
}

void InitMemoryDiagnostics()
{
    // Startup assembly is the only caller. No retry or background stack churn.
    const esp_err_t result = heap_caps_register_failed_alloc_callback(AllocationFailed);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "failure_hook_unavailable error=%s", esp_err_to_name(result));
    }
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    constexpr uint32_t stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    constexpr uint32_t stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    // This worker never reads/writes NVS or flash and can safely use PSRAM.
    // Refuse diagnostics rather than spend the internal WiFi reserve if the
    // requested stack pool is exhausted.
    auto* stack = static_cast<StackType_t*>(heap_caps_malloc(kStackBytes, stack_caps));
    if (stack == nullptr) {
        ESP_LOGW(kTag, "disabled: diagnostic stack unavailable");
        return;
    }
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
    if (g_history == nullptr) {
        void* memory = heap_caps_calloc(1, sizeof(History), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory != nullptr) g_history = new (memory) History{};
        if (g_history == nullptr) {
            ESP_LOGW(kTag, "history_disabled: PSRAM unavailable; live diagnostics continue");
        } else {
            ESP_LOGI(kTag, "history_enabled hours=%u interval_s=60 psram_bytes=%u",
                CONFIG_WQN_MEMORY_HISTORY_HOURS, static_cast<unsigned>(sizeof(History)));
        }
    }
#endif
    RecordMemorySnapshot(MemoryStage::kBoot);
    g_next_sample_us = esp_timer_get_time() + kSampleIntervalUs;
    const TaskHandle_t task = xTaskCreateStatic(DiagnosticTask, "wqn_mem_diag",
        kStackBytes / sizeof(StackType_t), nullptr, 1, stack, &g_task_storage);
    if (task == nullptr) {
        heap_caps_free(stack);
#if CONFIG_WQN_MEMORY_HISTORY_ENABLE
        heap_caps_free(g_history);
        g_history = nullptr;
#endif
        ESP_LOGW(kTag, "disabled: diagnostic task unavailable");
        return;
    }
    ESP_LOGI(kTag, "enabled interval_s=%u queue_bytes=%u stack_bytes=%u stack_pool=%s",
        CONFIG_WQN_MEMORY_SAMPLE_SECONDS,
        static_cast<unsigned>(sizeof(g_snapshots) + sizeof(g_failures)),
        static_cast<unsigned>(kStackBytes),
        stack_caps & MALLOC_CAP_SPIRAM ? "psram" : "internal");
}

}  // namespace wqn::runtime

#endif  // CONFIG_WQN_MEMORY_DIAGNOSTICS
