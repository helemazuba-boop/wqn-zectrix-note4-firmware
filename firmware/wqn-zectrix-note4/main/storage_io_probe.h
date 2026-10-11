#pragma once

#include <cstddef>
#include <cstdint>

// [measure] Diagnostic pass-through only. No flash calls originate here.
// Remove this module/linker wrapping with the temporary snapshot bench.
namespace wqn::measure {

struct PartitionIoBucket {
    uint32_t calls = 0;
    uint64_t bytes = 0;
    int64_t us = 0;
    int64_t max_us = 0;
    uint32_t failures = 0;
};

struct SpiffsGcStats {
    uint32_t check_calls = 0;
    uint32_t quick_calls = 0;
    uint32_t nested_calls = 0;
    uint32_t check_errors = 0;
    uint32_t quick_errors = 0;
    uint32_t quick_no_deleted = 0;
    int64_t check_us = 0;
    int64_t quick_us = 0;
    bool fs_seen = false;
    uint32_t block_count = 0;
    uint32_t block_size = 0;
    uint32_t page_size = 0;
    // First entry / last exit, plus extrema across all entries and exits.
    // -1 means no scoped GC function call, NOT zero free blocks/pages.
    int64_t free_before = -1;
    int64_t free_after = -1;
    int64_t free_min = -1;
    int64_t free_max = -1;
    int64_t allocated_before = -1;
    int64_t allocated_after = -1;
    int64_t deleted_before = -1;
    int64_t deleted_after = -1;
    PartitionIoBucket read;
    PartitionIoBucket write;
    PartitionIoBucket erase;
    // Incremental GC-state schema: clean data capacity, not SPIFFS_info free.
    uint32_t lookup_pages = 0;
    uint32_t data_page_bytes = 0;
    int64_t free_data_bytes_before = -1;
    int64_t free_data_bytes_after = -1;
};

struct PartitionIoStats {
    bool scope_ok = false;
    int64_t span_us = 0;
    uint32_t nested_calls = 0;
    PartitionIoBucket read;
    PartitionIoBucket write;
    PartitionIoBucket erase;
    // Inclusive GC function wall; the API buckets above already contain the
    // GC API subsets below. Never add GC wall to those API totals.
    SpiffsGcStats gc;
};

// Call once before the bench. Also logs device-derived app/ELF identity.
bool InitializePartitionIoProbe();

class PartitionIoStage {
public:
    PartitionIoStage();
    ~PartitionIoStage();
    PartitionIoStage(const PartitionIoStage&) = delete;
    PartitionIoStage& operator=(const PartitionIoStage&) = delete;
    PartitionIoStats Finish();

private:
    bool active_ = false;
    int64_t started_us_ = 0;
};

template<class Work>
PartitionIoStats MeasurePartitionIo(Work work)
{
    PartitionIoStage stage;
    work();
    return stage.Finish();
}

// Emit only AFTER all measured VFS operations; logging is never in an I/O span.
void LogPartitionIo(const char* kind, size_t bytes, int round, const char* phase,
                    int64_t vfs_us, const PartitionIoStats& stats);

}  // namespace wqn::measure
