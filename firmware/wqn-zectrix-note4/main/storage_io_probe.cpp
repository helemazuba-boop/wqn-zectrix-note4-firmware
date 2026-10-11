#include "storage_io_probe.h"

#include <atomic>
#include <cstdio>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spiffs_api.h"
#include "spiffs_nucleus.h"

namespace {

constexpr char kTag[] = "storage_io_probe";
std::atomic<TaskHandle_t> g_owner{nullptr};
std::atomic<const esp_partition_t*> g_partition{nullptr};
// Only the published owner accesses these; other tasks do not even read them.
wqn::measure::PartitionIoStats g_stats;
bool g_inside_partition_call = false;
bool g_inside_gc_call = false;

bool IsOwnerTask()
{
    const TaskHandle_t owner = g_owner.load(std::memory_order_acquire);
    return owner != nullptr && owner == xTaskGetCurrentTaskHandle();
}

bool Capture(const esp_partition_t* partition)
{
    const TaskHandle_t owner = g_owner.load(std::memory_order_acquire);
    if (owner == nullptr || owner != xTaskGetCurrentTaskHandle() ||
        partition != g_partition.load(std::memory_order_relaxed)) {
        return false;
    }
    if (g_inside_partition_call) {
        // Keep inclusive parent time, but never add nested API time twice.
        ++g_stats.nested_calls;
        return false;
    }
    g_inside_partition_call = true;
    return true;
}

void Account(wqn::measure::PartitionIoBucket& bucket, size_t bytes,
             int64_t elapsed_us, esp_err_t result)
{
    ++bucket.calls;
    bucket.bytes += bytes;  // Requested bytes; not proof of physical programming.
    bucket.us += elapsed_us;
    if (elapsed_us > bucket.max_us) bucket.max_us = elapsed_us;
    if (result != ESP_OK) ++bucket.failures;
    g_inside_partition_call = false;
}

void AccountGcApi(wqn::measure::PartitionIoBucket& bucket, size_t bytes,
                  int64_t elapsed_us, esp_err_t result)
{
    if (!g_inside_gc_call) return;
    ++bucket.calls;
    bucket.bytes += bytes;
    bucket.us += elapsed_us;
    if (elapsed_us > bucket.max_us) bucket.max_us = elapsed_us;
    if (result != ESP_OK) ++bucket.failures;
}

int64_t FreeDataBytes(const spiffs* fs)
{
    const int64_t pages =
        static_cast<int64_t>(SPIFFS_PAGES_PER_BLOCK(fs) - SPIFFS_OBJ_LOOKUP_PAGES(fs)) *
            (static_cast<int64_t>(fs->block_count) - 2) -
        fs->stats_p_allocated - fs->stats_p_deleted;
    return pages * static_cast<int64_t>(SPIFFS_DATA_PAGE_SIZE(fs));
}

bool CaptureGc(spiffs* fs)
{
    // Only inspect owner-private state after the cross-task guard. Validate
    // the actual ESP-IDF adapter/partition; phys_size alone is NOT an identity.
    if (!IsOwnerTask() || fs == nullptr || fs->user_data == nullptr ||
        fs->cfg.hal_read_f != spiffs_api_read ||
        fs->cfg.hal_write_f != spiffs_api_write ||
        fs->cfg.hal_erase_f != spiffs_api_erase) return false;
    const auto* adapter = static_cast<const esp_spiffs_t*>(fs->user_data);
    if (adapter->fs != fs ||
        adapter->partition != g_partition.load(std::memory_order_relaxed)) return false;
    if (g_inside_gc_call) {
        ++g_stats.gc.nested_calls;
        return false;  // Outer GC wall/API subset remains inclusive.
    }
    auto& gc = g_stats.gc;
    if (!gc.fs_seen) {
        gc.fs_seen = true;
        gc.block_count = fs->block_count;
        gc.block_size = fs->cfg.log_block_size;
        gc.page_size = fs->cfg.log_page_size;
        gc.free_before = gc.free_min = gc.free_max = fs->free_blocks;
        gc.allocated_before = fs->stats_p_allocated;
        gc.deleted_before = fs->stats_p_deleted;
        gc.lookup_pages = SPIFFS_OBJ_LOOKUP_PAGES(fs);
        gc.data_page_bytes = SPIFFS_DATA_PAGE_SIZE(fs);
        gc.free_data_bytes_before = FreeDataBytes(fs);
    }
    if (fs->free_blocks < gc.free_min) gc.free_min = fs->free_blocks;
    if (fs->free_blocks > gc.free_max) gc.free_max = fs->free_blocks;
    g_inside_gc_call = true;
    return true;
}

void FinishGc(spiffs* fs, int64_t elapsed_us, s32_t result, bool quick)
{
    auto& gc = g_stats.gc;
    if (quick) {
        ++gc.quick_calls;
        gc.quick_us += elapsed_us;
        if (result == SPIFFS_ERR_NO_DELETED_BLOCKS) ++gc.quick_no_deleted;
        else if (result < SPIFFS_OK) ++gc.quick_errors;
    } else {
        ++gc.check_calls;
        gc.check_us += elapsed_us;
        if (result < SPIFFS_OK) ++gc.check_errors;
    }
    gc.free_after = fs->free_blocks;
    gc.allocated_after = fs->stats_p_allocated;
    gc.deleted_after = fs->stats_p_deleted;
    gc.free_data_bytes_after = FreeDataBytes(fs);
    if (fs->free_blocks < gc.free_min) gc.free_min = fs->free_blocks;
    if (fs->free_blocks > gc.free_max) gc.free_max = fs->free_blocks;
    g_inside_gc_call = false;
}

}  // namespace

// GNU ld --wrap redirects SDK references, not the original implementation.
// Forward exactly once with the original arguments/result. Not a raw writer.
extern "C" esp_err_t __real_esp_partition_read(
    const esp_partition_t*, size_t, void*, size_t);
extern "C" esp_err_t __real_esp_partition_write(
    const esp_partition_t*, size_t, const void*, size_t);
extern "C" esp_err_t __real_esp_partition_erase_range(
    const esp_partition_t*, size_t, size_t);
extern "C" s32_t __real_spiffs_gc_check(spiffs*, u32_t);
extern "C" s32_t __real_spiffs_gc_quick(spiffs*, u16_t);

extern "C" s32_t __wrap_spiffs_gc_check(spiffs* fs, u32_t bytes)
{
    if (!CaptureGc(fs)) return __real_spiffs_gc_check(fs, bytes);
    const int64_t started_us = esp_timer_get_time();
    const s32_t result = __real_spiffs_gc_check(fs, bytes);
    FinishGc(fs, esp_timer_get_time() - started_us, result, false);
    return result;
}

extern "C" s32_t __wrap_spiffs_gc_quick(spiffs* fs, u16_t max_free_pages)
{
    if (!CaptureGc(fs)) return __real_spiffs_gc_quick(fs, max_free_pages);
    const int64_t started_us = esp_timer_get_time();
    const s32_t result = __real_spiffs_gc_quick(fs, max_free_pages);
    FinishGc(fs, esp_timer_get_time() - started_us, result, true);
    return result;
}

extern "C" esp_err_t __wrap_esp_partition_read(
    const esp_partition_t* partition, size_t offset, void* dst, size_t bytes)
{
    if (!Capture(partition)) return __real_esp_partition_read(partition, offset, dst, bytes);
    const int64_t started_us = esp_timer_get_time();
    const esp_err_t result = __real_esp_partition_read(partition, offset, dst, bytes);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    AccountGcApi(g_stats.gc.read, bytes, elapsed_us, result);
    Account(g_stats.read, bytes, elapsed_us, result);
    return result;
}

extern "C" esp_err_t __wrap_esp_partition_write(
    const esp_partition_t* partition, size_t offset, const void* src, size_t bytes)
{
    if (!Capture(partition)) return __real_esp_partition_write(partition, offset, src, bytes);
    const int64_t started_us = esp_timer_get_time();
    const esp_err_t result = __real_esp_partition_write(partition, offset, src, bytes);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    AccountGcApi(g_stats.gc.write, bytes, elapsed_us, result);
    Account(g_stats.write, bytes, elapsed_us, result);
    return result;
}

extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t* partition, size_t offset, size_t bytes)
{
    if (!Capture(partition)) return __real_esp_partition_erase_range(partition, offset, bytes);
    const int64_t started_us = esp_timer_get_time();
    const esp_err_t result = __real_esp_partition_erase_range(partition, offset, bytes);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    AccountGcApi(g_stats.gc.erase, bytes, elapsed_us, result);
    Account(g_stats.erase, bytes, elapsed_us, result);
    return result;
}

namespace wqn::measure {

bool InitializePartitionIoProbe()
{
    if (g_owner.load(std::memory_order_acquire) != nullptr) return false;
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    g_partition.store(partition, std::memory_order_release);
    const esp_app_desc_t* app = esp_app_get_description();
    char elf_prefix[17] = {};
    for (size_t i = 0; i < 8; ++i) {
        std::snprintf(elf_prefix + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
    ESP_LOGI(kTag,
             "storage partition probe READY schema=1 enabled=%d scope=task+partition "
             "partition=storage app_version=%s elf_sha256=%s gc_probe=1 partition_bytes=%lu",
             partition != nullptr ? 1 : 0, app->version, elf_prefix,
             static_cast<unsigned long>(partition != nullptr ? partition->size : 0));
    return partition != nullptr;
}

PartitionIoStage::PartitionIoStage()
{
    if (g_partition.load(std::memory_order_acquire) == nullptr) return;
    TaskHandle_t expected = nullptr;
    active_ = g_owner.compare_exchange_strong(
        expected, xTaskGetCurrentTaskHandle(), std::memory_order_acq_rel);
    if (active_) {
        g_stats = {};
        g_stats.scope_ok = true;
        g_inside_partition_call = false;
        g_inside_gc_call = false;
        started_us_ = esp_timer_get_time();
    }
}

PartitionIoStage::~PartitionIoStage()
{
    if (active_) Finish();
}

PartitionIoStats PartitionIoStage::Finish()
{
    if (!active_) return {};
    g_stats.span_us = esp_timer_get_time() - started_us_;
    const PartitionIoStats result = g_stats;
    g_owner.store(nullptr, std::memory_order_release);
    active_ = false;
    return result;
}

void LogPartitionIo(const char* kind, size_t bytes, int round, const char* phase,
                    int64_t vfs_us, const PartitionIoStats& s)
{
    // Wall time inside top-level esp_partition APIs, NOT chip busy time. Each
    // API can include bus/cache locks, driver verification and preemption.
    ESP_LOGI(kTag,
             "storage partition io: kind=%s bytes=%u round=%d phase=%s "
             "vfs_us=%lld span_us=%lld scope_ok=%d nested_calls=%lu "
             "read_calls=%lu read_bytes=%llu read_us=%lld read_max_us=%lld read_failures=%lu "
             "write_calls=%lu write_bytes=%llu write_us=%lld write_max_us=%lld write_failures=%lu "
             "erase_calls=%lu erase_bytes=%llu erase_us=%lld erase_max_us=%lld erase_failures=%lu gc_probe=1",
             kind, static_cast<unsigned>(bytes), round, phase,
             static_cast<long long>(vfs_us), static_cast<long long>(s.span_us),
             s.scope_ok ? 1 : 0, static_cast<unsigned long>(s.nested_calls),
             static_cast<unsigned long>(s.read.calls), static_cast<unsigned long long>(s.read.bytes),
             static_cast<long long>(s.read.us), static_cast<long long>(s.read.max_us),
             static_cast<unsigned long>(s.read.failures),
             static_cast<unsigned long>(s.write.calls), static_cast<unsigned long long>(s.write.bytes),
             static_cast<long long>(s.write.us), static_cast<long long>(s.write.max_us),
             static_cast<unsigned long>(s.write.failures),
             static_cast<unsigned long>(s.erase.calls), static_cast<unsigned long long>(s.erase.bytes),
             static_cast<long long>(s.erase.us), static_cast<long long>(s.erase.max_us),
             static_cast<unsigned long>(s.erase.failures));

    const auto& gc = s.gc;
    ESP_LOGI(kTag,
             "storage spiffs gc: schema=1 kind=%s bytes=%u round=%d phase=%s scope_ok=%d "
             "gc_check_calls=%lu gc_quick_calls=%lu gc_nested_calls=%lu "
             "gc_check_us=%lld gc_quick_us=%lld gc_check_errors=%lu gc_quick_errors=%lu "
             "gc_quick_no_deleted=%lu fs_seen=%d block_count=%lu block_size=%lu page_size=%lu "
             "free_before=%lld free_after=%lld free_min=%lld free_max=%lld "
             "allocated_before=%lld allocated_after=%lld deleted_before=%lld deleted_after=%lld "
             "gc_read_calls=%lu gc_read_bytes=%llu gc_read_us=%lld "
             "gc_write_calls=%lu gc_write_bytes=%llu gc_write_us=%lld "
             "gc_erase_calls=%lu gc_erase_bytes=%llu gc_erase_us=%lld "
             "lookup_pages=%lu data_page_bytes=%lu free_data_bytes_before=%lld free_data_bytes_after=%lld",
             kind, static_cast<unsigned>(bytes), round, phase, s.scope_ok ? 1 : 0,
             static_cast<unsigned long>(gc.check_calls), static_cast<unsigned long>(gc.quick_calls),
             static_cast<unsigned long>(gc.nested_calls),
             static_cast<long long>(gc.check_us), static_cast<long long>(gc.quick_us),
             static_cast<unsigned long>(gc.check_errors), static_cast<unsigned long>(gc.quick_errors),
             static_cast<unsigned long>(gc.quick_no_deleted), gc.fs_seen ? 1 : 0,
             static_cast<unsigned long>(gc.block_count), static_cast<unsigned long>(gc.block_size),
             static_cast<unsigned long>(gc.page_size),
             static_cast<long long>(gc.free_before), static_cast<long long>(gc.free_after),
             static_cast<long long>(gc.free_min), static_cast<long long>(gc.free_max),
             static_cast<long long>(gc.allocated_before), static_cast<long long>(gc.allocated_after),
             static_cast<long long>(gc.deleted_before), static_cast<long long>(gc.deleted_after),
             static_cast<unsigned long>(gc.read.calls), static_cast<unsigned long long>(gc.read.bytes),
             static_cast<long long>(gc.read.us),
             static_cast<unsigned long>(gc.write.calls), static_cast<unsigned long long>(gc.write.bytes),
             static_cast<long long>(gc.write.us),
             static_cast<unsigned long>(gc.erase.calls), static_cast<unsigned long long>(gc.erase.bytes),
             static_cast<long long>(gc.erase.us),
             static_cast<unsigned long>(gc.lookup_pages),
             static_cast<unsigned long>(gc.data_page_bytes),
             static_cast<long long>(gc.free_data_bytes_before),
             static_cast<long long>(gc.free_data_bytes_after));
}

}  // namespace wqn::measure
