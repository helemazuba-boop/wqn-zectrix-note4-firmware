#pragma once
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#define IRAM_ATTR
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
#define portENTER_CRITICAL_SAFE(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux) portEXIT_CRITICAL(mux)
using StackType_t = uint8_t;
struct StaticTask_t {};
using TaskHandle_t = void*;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1;
constexpr uint32_t MALLOC_CAP_8BIT = 4, MALLOC_CAP_DMA = 8;
constexpr uint32_t MALLOC_CAP_SPIRAM = 1024, MALLOC_CAP_INTERNAL = 2048;
struct multi_heap_info_t {
    size_t total_free_bytes = 0, minimum_free_bytes = 0, largest_free_block = 0;
};
inline std::atomic<int64_t> host_now{1000};
inline std::atomic<unsigned> host_heap_queries{0}, host_allocations{0};
inline std::vector<std::string> host_logs;
inline thread_local bool host_isr = false;
inline bool host_stack_fail = false, host_task_fail = false;
inline size_t host_stack_size = 0;
inline uint32_t host_stack_caps = 0;
inline void* host_stack = nullptr;
inline bool host_history_fail = false;
inline unsigned host_history_allocations = 0;
inline uint32_t host_history_caps = 0;
using esp_alloc_failed_hook_t = void (*)(size_t, uint32_t, const char*);
inline esp_alloc_failed_hook_t host_hook = nullptr;
inline int64_t esp_timer_get_time() { return host_now.load(); }
inline bool xPortInIsrContext() { return host_isr; }
inline void heap_caps_get_info(multi_heap_info_t* out, uint32_t caps) {
    ++host_heap_queries;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) *out = {100000, 60000, 32000};
    else if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) *out = {90000, 50000, 30000};
    else if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) *out = {6000000, 5000000, 4000000};
    else assert(false && "incorrect heap capabilities");
}
inline const char* esp_err_to_name(esp_err_t) { return "HOST_ERROR"; }
inline int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t hook) {
    host_hook = hook; return ESP_OK;
}
inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    ++host_allocations; host_stack_size = size; host_stack_caps = caps;
    if (host_stack_fail) return nullptr;
    host_stack = std::malloc(size); return host_stack;
}
inline void* heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
    ++host_allocations; ++host_history_allocations; host_history_caps = caps;
    return host_history_fail ? nullptr : std::calloc(count, size);
}
inline void heap_caps_free(void* ptr) { std::free(ptr); host_stack = nullptr; }
inline TaskHandle_t xTaskCreateStatic(void (*)(void*), const char*, size_t depth,
    void*, unsigned priority, StackType_t*, StaticTask_t*) {
    assert(depth == 4096 && priority == 1);
    return host_task_fail ? nullptr : reinterpret_cast<void*>(1);
}
inline void vTaskDelay(unsigned) {}
#define pdMS_TO_TICKS(value) (value)
inline void HostLog(const char*, const char* format, ...) __attribute__((format(printf, 2, 3)));
inline void HostLog(const char*, const char* format, ...) {
    char buffer[2048]; va_list args; va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    host_logs.emplace_back(buffer);
}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)

struct heap_trace_record_t {
    void* address = nullptr;
    size_t size = 0;
    void* alloced_by[CONFIG_HEAP_TRACING_STACK_DEPTH]{};
};
struct heap_trace_summary_t {
    size_t count = 0, capacity = 0, has_overflowed = 0;
    size_t total_allocations = 0, total_frees = 0;
};
constexpr int HEAP_TRACE_LEAKS = 0;
inline int host_trace_starts = 0, host_trace_stops = 0, host_trace_gets = 0;
inline bool host_trace_init_fail = false;
inline bool host_trace_stop_fail = false;
inline heap_trace_record_t* host_trace_buffer = nullptr;
inline size_t host_trace_capacity = 0;
inline int heap_trace_init_standalone(heap_trace_record_t* buffer, size_t count) {
    host_trace_buffer = buffer; host_trace_capacity = count;
    return host_trace_init_fail ? ESP_ERR_INVALID_STATE : ESP_OK;
}
inline int heap_trace_start(int mode) {
    assert(mode == HEAP_TRACE_LEAKS); ++host_trace_starts; return ESP_OK;
}
inline int heap_trace_stop() {
    ++host_trace_stops; return host_trace_stop_fail ? ESP_ERR_INVALID_STATE : ESP_OK;
}
inline int heap_trace_summary(heap_trace_summary_t* summary) {
    *summary = {6, host_trace_capacity, 1, 100, 94}; return ESP_OK;
}
inline int heap_trace_get(size_t index, heap_trace_record_t* record) {
    assert(index < 6); ++host_trace_gets;
    record->size = 100 + index; record->address = reinterpret_cast<void*>(0x3fc00000);
    record->alloced_by[0] = reinterpret_cast<void*>(0x42001234);
    return ESP_OK;
}
inline bool esp_ptr_external_ram(const void*) { return false; }
