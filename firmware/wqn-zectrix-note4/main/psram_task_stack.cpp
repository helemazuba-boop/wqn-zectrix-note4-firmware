#include "psram_task_stack.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace wqn {
namespace {

constexpr char kTag[] = "wqn_task_stack";

}  // namespace

StackType_t* AllocTaskStack(size_t bytes, const char* owner, bool* out_in_psram)
{
    if (out_in_psram != nullptr) {
        *out_in_psram = false;
    }
    if (bytes == 0) {
        return nullptr;
    }

    // Only tasks that never touch flash or NVS on their own stack may land
    // here. ESP-IDF cannot access PSRAM while the cache is disabled, and the
    // flash/NVS paths disable it -- including *reads*: esp_partition_read and
    // nvs_get_* both go through spi_flash_disable_interrupts_caches_and_other_cpu,
    // which asserts esp_task_stack_is_sane_cache_disabled() on the calling
    // task's stack pointer. A PSRAM stack there reboots the device.
    //
    // That assertion is what reverted commit 7af4ff4: it moved the AI worker
    // stack to PSRAM and the device crashed at boot on LoadAccessToken ->
    // nvs_get_str -> esp_partition_read. Work dispatched to another task (the
    // storage service, the persist worker queue) is fine -- the flash access
    // then runs on that task's internal stack.
    StackType_t* stack = static_cast<StackType_t*>(
        heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stack != nullptr) {
        if (out_in_psram != nullptr) {
            *out_in_psram = true;
        }
    } else {
        // Fall back to internal rather than failing: a device that boots with
        // a slightly smaller margin is far better than one that does not boot,
        // and this runs during init where refusing is not actionable.
        ESP_LOGW(kTag, "%s: PSRAM stack unavailable, falling back to internal",
                 owner == nullptr ? "task" : owner);
        stack = static_cast<StackType_t*>(
            heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    ESP_LOGI(kTag,
             "%s task stack: bytes=%u in_psram=%d dma_free=%u "
             "dma_largest=%u internal_free=%u internal_largest=%u",
             owner == nullptr ? "task" : owner,
             static_cast<unsigned>(bytes),
             stack != nullptr && out_in_psram != nullptr && *out_in_psram ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return stack;
}

}  // namespace wqn
