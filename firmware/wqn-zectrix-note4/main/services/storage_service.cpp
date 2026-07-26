#include "services/storage_service.h"

#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "runtime/sleep_coordinator.h"

namespace {

constexpr char kTag[] = "storage_service";
constexpr UBaseType_t kBackgroundQueueDepth = 8;
constexpr UBaseType_t kForegroundQueueDepth = 4;
// ESP-IDF's ROM miniz test alone requires an 8 KiB task stack. WQPC runs it
// below the storage transaction, SPIFFS and C++ serialization call chains, and
// the first COM7 HIL run proved that 8 KiB overflows. A second HIL run measured
// only 3296 bytes free at 16 KiB, below the 4 KiB safety floor, so retain the
// measured production call chain plus explicit bounded headroom.
constexpr uint32_t kTaskStackBytes = 20 * 1024;
constexpr UBaseType_t kStackWarningBytes = 4 * 1024;
constexpr UBaseType_t kTaskPriority = 6;

struct StorageCommand {
    wqn::services::StorageTransaction transaction = nullptr;
    void* context = nullptr;
    uint32_t request_id = 0;
    const char* owner = "background";
    struct StorageCompletion* completion = nullptr;
    int64_t queued_at_us = 0;
};

struct StorageCompletion {
    StaticSemaphore_t semaphore_storage = {};
    SemaphoreHandle_t semaphore = nullptr;
    uint32_t request_id = 0;
    esp_err_t result = ESP_FAIL;
};

StaticQueue_t g_background_queue_storage;
uint8_t g_background_queue_buffer[
    kBackgroundQueueDepth * sizeof(StorageCommand)] = {};
QueueHandle_t g_background_queue = nullptr;
StaticQueue_t g_foreground_queue_storage;
uint8_t g_foreground_queue_buffer[
    kForegroundQueueDepth * sizeof(StorageCommand)] = {};
QueueHandle_t g_foreground_queue = nullptr;
TaskHandle_t g_task = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;
std::atomic<uint32_t> g_next_request_id{1};

uint32_t NextRequestId()
{
    uint32_t request_id = g_next_request_id.fetch_add(1, std::memory_order_relaxed);
    if (request_id == 0) {
        request_id = g_next_request_id.fetch_add(1, std::memory_order_relaxed);
    }
    return request_id;
}

bool ReceiveNextCommand(StorageCommand* command)
{
    if (command == nullptr) {
        return false;
    }
    // The foreground queue is checked at every transaction boundary. A
    // transaction already inside SPIFFS/NVS remains atomic and non-preemptive.
    if (xQueueReceive(g_foreground_queue, command, 0) == pdTRUE) {
        return true;
    }
    return xQueueReceive(g_background_queue, command, 0) == pdTRUE;
}

void StorageServiceTask(void*)
{
    ESP_LOGI(
        kTag,
        "storage service started: foreground_depth=%u background_depth=%u",
        static_cast<unsigned>(kForegroundQueueDepth),
        static_cast<unsigned>(kBackgroundQueueDepth));
    while (true) {
        StorageCommand command;
        if (!ReceiveNextCommand(&command)) {
            // Enqueuers notify after publishing. Notifications accumulate, so
            // a send between the empty checks and this wait cannot be lost.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const int64_t started_us = esp_timer_get_time();
        const int64_t queue_wait_ms =
            command.queued_at_us > 0
            ? (started_us - command.queued_at_us) / 1000
            : 0;
        esp_err_t result = ESP_FAIL;
        {
            // Serialization, checksums and SPIFFS/NVS bookkeeping otherwise
            // run at the 40 MHz DFS floor. Boost only for the bounded
            // transaction; idle operation remains free to downclock.
            auto cpu_lease = wqn::runtime::CpuPerformanceLease::TryAcquire();
            result = command.transaction == nullptr
                ? ESP_ERR_INVALID_ARG
                : command.transaction(command.context);
        }
        const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
        // ESP-IDF reports this high-water mark in bytes, unlike upstream
        // FreeRTOS which documents stack words.
        const UBaseType_t free_stack_bytes = uxTaskGetStackHighWaterMark(nullptr);
        if (free_stack_bytes < kStackWarningBytes) {
            ESP_LOGW(
                kTag,
                "storage task stack low: request=%lu free_bytes=%u configured_bytes=%lu",
                static_cast<unsigned long>(command.request_id),
                static_cast<unsigned>(free_stack_bytes),
                static_cast<unsigned long>(kTaskStackBytes));
        } else {
            ESP_LOGI(
                kTag,
                "storage transaction complete: request=%lu owner=%s queue_wait_ms=%lld elapsed_ms=%lld result=%s stack_free_bytes=%u",
                static_cast<unsigned long>(command.request_id),
                command.owner == nullptr ? "unknown" : command.owner,
                static_cast<long long>(queue_wait_ms),
                static_cast<long long>(elapsed_ms),
                esp_err_to_name(result),
                static_cast<unsigned>(free_stack_bytes));
        }
        if (command.completion == nullptr ||
            command.completion->request_id != command.request_id) {
            ESP_LOGE(
                kTag,
                "invalid storage completion: request=%lu",
                static_cast<unsigned long>(command.request_id));
            continue;
        }
        command.completion->result = result;
        xSemaphoreGive(command.completion->semaphore);
    }
}

esp_err_t ExecuteStorageTransactionInternal(
    wqn::services::StorageTransaction transaction,
    void* context,
    const char* owner,
    bool foreground)
{
    if (transaction == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wqn::services::IsStorageServiceTask()) {
        return transaction(context);
    }
    if (g_background_queue == nullptr || g_foreground_queue == nullptr ||
        g_task == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    StorageCompletion completion;
    completion.semaphore = xSemaphoreCreateBinaryStatic(
        &completion.semaphore_storage);
    if (completion.semaphore == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    completion.request_id = NextRequestId();

    StorageCommand command;
    command.transaction = transaction;
    command.context = context;
    command.request_id = completion.request_id;
    command.owner = owner;
    command.completion = &completion;
    command.queued_at_us = esp_timer_get_time();
    QueueHandle_t queue = foreground ? g_foreground_queue : g_background_queue;
    if (xQueueSend(queue, &command, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotifyGive(g_task);
    if (xSemaphoreTake(completion.semaphore, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return completion.result;
}

}  // namespace

namespace wqn::services {

esp_err_t StartStorageService()
{
    taskENTER_CRITICAL(&g_start_lock);
    if (g_task != nullptr) {
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_OK;
    }
    if (g_starting) {
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_starting = true;
    if (g_background_queue == nullptr) {
        g_background_queue = xQueueCreateStatic(
            kBackgroundQueueDepth,
            sizeof(StorageCommand),
            g_background_queue_buffer,
            &g_background_queue_storage);
    }
    if (g_foreground_queue == nullptr) {
        g_foreground_queue = xQueueCreateStatic(
            kForegroundQueueDepth,
            sizeof(StorageCommand),
            g_foreground_queue_buffer,
            &g_foreground_queue_storage);
    }
    taskEXIT_CRITICAL(&g_start_lock);
    if (g_background_queue == nullptr || g_foreground_queue == nullptr) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t created_task = nullptr;
    if (xTaskCreate(
            StorageServiceTask,
            "storage_svc",
            kTaskStackBytes,
            nullptr,
            kTaskPriority,
            &created_task) != pdPASS) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&g_start_lock);
    g_task = created_task;
    g_starting = false;
    taskEXIT_CRITICAL(&g_start_lock);
    return ESP_OK;
}

bool IsStorageServiceTask()
{
    return g_task != nullptr && xTaskGetCurrentTaskHandle() == g_task;
}

esp_err_t ExecuteStorageTransaction(StorageTransaction transaction, void* context)
{
    return ExecuteStorageTransactionInternal(
        transaction, context, "background", false);
}

esp_err_t ExecuteStorageTransactionNamed(
    StorageTransaction transaction,
    void* context,
    const char* owner)
{
    return ExecuteStorageTransactionInternal(
        transaction,
        context,
        owner == nullptr ? "background" : owner,
        false);
}

esp_err_t ExecuteForegroundStorageTransaction(
    StorageTransaction transaction,
    void* context,
    const char* owner)
{
    return ExecuteStorageTransactionInternal(
        transaction,
        context,
        owner == nullptr ? "foreground" : owner,
        true);
}

}  // namespace wqn::services
