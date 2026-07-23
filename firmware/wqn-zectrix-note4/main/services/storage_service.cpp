#include "services/storage_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "storage_service";
constexpr UBaseType_t kQueueDepth = 8;
constexpr UBaseType_t kReplyQueueDepth = 4;
constexpr uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = 6;

struct StorageCommand {
    wqn::services::StorageTransaction transaction = nullptr;
    void* context = nullptr;
    uint32_t request_id = 0;
};

struct StorageReply {
    uint32_t request_id = 0;
    esp_err_t result = ESP_FAIL;
};

StaticQueue_t g_queue_storage;
uint8_t g_queue_buffer[kQueueDepth * sizeof(StorageCommand)] = {};
QueueHandle_t g_queue = nullptr;

StaticQueue_t g_reply_queue_storage;
uint8_t g_reply_queue_buffer[kReplyQueueDepth * sizeof(StorageReply)] = {};
QueueHandle_t g_reply_queue = nullptr;

StaticSemaphore_t g_call_mutex_storage;
SemaphoreHandle_t g_call_mutex = nullptr;
TaskHandle_t g_task = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;
uint32_t g_next_request_id = 1;

void StorageServiceTask(void*)
{
    ESP_LOGI(kTag, "storage service started: queue_depth=%u", static_cast<unsigned>(kQueueDepth));
    while (true) {
        StorageCommand command;
        if (xQueueReceive(g_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        StorageReply reply;
        reply.request_id = command.request_id;
        reply.result = command.transaction == nullptr
            ? ESP_ERR_INVALID_ARG
            : command.transaction(command.context);
        if (xQueueSend(g_reply_queue, &reply, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(kTag, "failed to publish storage reply: request=%lu",
                static_cast<unsigned long>(reply.request_id));
        }
    }
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
    if (g_queue == nullptr) {
        g_queue = xQueueCreateStatic(
            kQueueDepth,
            sizeof(StorageCommand),
            g_queue_buffer,
            &g_queue_storage);
    }
    if (g_reply_queue == nullptr) {
        g_reply_queue = xQueueCreateStatic(
            kReplyQueueDepth,
            sizeof(StorageReply),
            g_reply_queue_buffer,
            &g_reply_queue_storage);
    }
    if (g_call_mutex == nullptr) {
        g_call_mutex = xSemaphoreCreateMutexStatic(&g_call_mutex_storage);
    }
    taskEXIT_CRITICAL(&g_start_lock);
    if (g_queue == nullptr || g_reply_queue == nullptr || g_call_mutex == nullptr) {
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
    if (transaction == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (IsStorageServiceTask()) {
        return transaction(context);
    }
    if (g_queue == nullptr || g_reply_queue == nullptr ||
        g_call_mutex == nullptr || g_task == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_call_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    StorageCommand command;
    command.transaction = transaction;
    command.context = context;
    command.request_id = g_next_request_id++;
    if (command.request_id == 0) {
        command.request_id = g_next_request_id++;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xQueueSend(g_queue, &command, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(g_call_mutex);
        return ESP_ERR_TIMEOUT;
    }

    StorageReply reply;
    if (xQueueReceive(g_reply_queue, &reply, portMAX_DELAY) == pdTRUE &&
        reply.request_id == command.request_id) {
        result = reply.result;
    } else {
        ESP_LOGE(kTag, "storage reply mismatch: expected=%lu actual=%lu",
            static_cast<unsigned long>(command.request_id),
            static_cast<unsigned long>(reply.request_id));
    }
    xSemaphoreGive(g_call_mutex);
    return result;
}

}  // namespace wqn::services
