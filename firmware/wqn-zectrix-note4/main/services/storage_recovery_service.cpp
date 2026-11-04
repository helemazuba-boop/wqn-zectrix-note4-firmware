#include "services/storage_recovery_service.h"

#include <atomic>
#include <type_traits>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "psram_task_stack.h"
#include "services/storage_service.h"

namespace {
using namespace wqn;
using namespace wqn::services;
constexpr char kTag[] = "storage_recovery";
constexpr size_t kStackBytes = 8192;
enum class State : uint8_t { kIdle, kFilling, kQueued, kRunning, kDone };
struct Request {
    uint32_t operation_id = 0;
    StorageRecoveryOperation operation = StorageRecoveryOperation::kInspect;
    runtime::StorageRecoverySnapshot expected;
};
static_assert(std::is_trivially_copyable_v<Request>);
static_assert(std::is_trivially_copyable_v<StorageRecoveryResult>);
// These payloads must remain in internal static RAM: the coordinator stack may
// be in PSRAM, but NVS reads/writes execute on StorageService's internal stack.
Request g_request;
StorageRecoveryResult g_result;
std::atomic<State> g_state{State::kIdle};
std::atomic<uint32_t> g_next_id{1};
TaskHandle_t g_task = nullptr;
TaskHandle_t g_ui = nullptr;
StaticTask_t g_tcb;
StackType_t* g_stack = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;

esp_err_t RunOnStorageOwner(void*)
{
    if (!IsStorageServiceTask()) return ESP_ERR_INVALID_STATE;
    if (g_request.operation == StorageRecoveryOperation::kInspect) {
        g_result.error = runtime::InspectStorageRecovery(&g_result.snapshot);
        // Metadata diagnostics only; no reset-id bytes, credentials, paths or
        // business records. This serial output is available without networking.
        ESP_LOGI(kTag, "diagnostic: error=%s marker=%u generation=%lu reset_id_bytes=%u policy=%u",
            esp_err_to_name(g_result.error), g_result.snapshot.generation_found ? 1u : 0u,
            static_cast<unsigned long>(g_result.snapshot.observed_generation),
            static_cast<unsigned>(g_result.snapshot.reset_id_size),
            static_cast<unsigned>(g_result.snapshot.reset_policy));
    } else {
        g_result.reset = runtime::AuthorizeStorageRecoveryReset(g_request.expected);
        g_result.error = g_result.reset.error;
    }
    return ESP_OK;  // Transaction delivery succeeded; operation error is explicit.
}

void Coordinator(void*)
{
    while (true) {
        State expected = State::kQueued;
        if (!g_state.compare_exchange_strong(expected, State::kRunning,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        g_result = {};
        g_result.operation_id = g_request.operation_id;
        g_result.operation = g_request.operation;
        const esp_err_t delivery = ExecuteStorageTransactionNamed(
            RunOnStorageOwner, nullptr, "boot-storage-recovery");
        if (delivery != ESP_OK) {
            g_result.error = delivery;
            g_result.reset.error = delivery;
            g_result.snapshot.error = delivery;
        }
        g_request = {};
        g_state.store(State::kDone, std::memory_order_release);
        xTaskNotifyGive(g_ui);
    }
}

bool Submit(StorageRecoveryOperation operation,
            const runtime::StorageRecoverySnapshot& snapshot, uint32_t* operation_id)
{
    if (operation_id == nullptr) return false;
    taskENTER_CRITICAL(&g_start_lock);
    const TaskHandle_t task = g_task;
    taskEXIT_CRITICAL(&g_start_lock);
    if (task == nullptr) return false;
    State expected = State::kIdle;
    if (!g_state.compare_exchange_strong(expected, State::kFilling,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    uint32_t id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    g_request = {id, operation, snapshot};
    *operation_id = id;
    g_state.store(State::kQueued, std::memory_order_release);
    xTaskNotifyGive(task);
    return true;
}
}  // namespace

namespace wqn::services {
esp_err_t StartStorageRecoveryService(TaskHandle_t ui_task)
{
    if (ui_task == nullptr) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&g_start_lock);
    if (g_task != nullptr || g_starting) {
        const esp_err_t result = g_task != nullptr && g_ui == ui_task
            ? ESP_OK : ESP_ERR_INVALID_STATE;
        taskEXIT_CRITICAL(&g_start_lock);
        return result;
    }
    g_starting = true;
    taskEXIT_CRITICAL(&g_start_lock);
    esp_err_t error = StartStorageService();
    TaskHandle_t created = nullptr;
    if (error == ESP_OK) {
        g_stack = AllocTaskStack(kStackBytes, "storage-recovery", nullptr);
        if (g_stack == nullptr) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) {
        g_ui = ui_task;
        created = xTaskCreateStatic(Coordinator, "wqn_recovery", TaskStackWords(kStackBytes),
            nullptr, 4, g_stack, &g_tcb);
        if (created == nullptr) error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        if (g_stack != nullptr) heap_caps_free(g_stack);
        g_stack = nullptr;
        g_ui = nullptr;
    }
    taskENTER_CRITICAL(&g_start_lock);
    g_task = created;
    g_starting = false;
    taskEXIT_CRITICAL(&g_start_lock);
    return error;
}

bool SubmitStorageRecoveryInspect(uint32_t* operation_id)
{
    return Submit(StorageRecoveryOperation::kInspect, {}, operation_id);
}
bool SubmitStorageRecoveryReset(
    const runtime::StorageRecoverySnapshot& expected, uint32_t* operation_id)
{
    return Submit(StorageRecoveryOperation::kAuthorizeReset, expected, operation_id);
}
bool ReadStorageRecoveryResult(StorageRecoveryResult* result)
{
    if (result == nullptr || g_state.load(std::memory_order_acquire) != State::kDone) return false;
    *result = g_result;
    return true;
}
bool AcknowledgeStorageRecoveryResult(uint32_t operation_id)
{
    if (g_state.load(std::memory_order_acquire) != State::kDone ||
        operation_id == 0 || g_result.operation_id != operation_id) return false;
    g_result = {};
    g_state.store(State::kIdle, std::memory_order_release);
    return true;
}
}  // namespace wqn::services
