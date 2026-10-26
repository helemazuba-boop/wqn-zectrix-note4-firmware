#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include "voice_idle_disconnect.h"
#include "runtime/fault_diagnostics.h"

bool deny_completion_allocation = false;
unsigned completion_allocations = 0;
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    ++completion_allocations;
    if (deny_completion_allocation) return nullptr;
    try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

using esp_err_t = int;
using TickType_t = uint32_t;
using UBaseType_t = unsigned;
using TaskHandle_t = void*;
using QueueHandle_t = void*;
using portMUX_TYPE = int;
struct StaticSemaphore_t { bool ready = false; };
using SemaphoreHandle_t = StaticSemaphore_t*;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 258;
constexpr int ESP_ERR_INVALID_STATE = 259, ESP_ERR_NO_MEM = 260, ESP_ERR_TIMEOUT = 261;
constexpr int ESP_ERR_NOT_FOUND = 262;
constexpr int pdTRUE = 1, pdFALSE = 0;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
const char* esp_err_to_name(int) { return "host-result"; }
int64_t esp_timer_get_time() { return 1000000; }
UBaseType_t uxTaskGetStackHighWaterMark(void*) { return 10000; }
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); \
} } while (false)

bool on_storage_owner = false;
bool allow_storage_lease = true;
unsigned storage_leases = 0;
namespace wqn {
enum class AiSessionStatus { kIdle, kListening };
namespace runtime {
unsigned fault_reports = 0;
FaultPhase fault_phase = FaultPhase::kReportedError;
int32_t fault_code = 0;
uint32_t fault_operation = 0;
void RecordFault(FaultDomain domain, FaultPhase phase, int32_t code, uint32_t operation, uint32_t) {
    CHECK(domain == FaultDomain::kStorage);
    ++fault_reports; fault_phase = phase; fault_code = code; fault_operation = operation;
}
enum class SleepBlocker { kStorage };
struct SleepLease {
    bool held;
    explicit SleepLease(bool acquire) : held(acquire) { if (held) ++storage_leases; }
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int)
    {
        return SleepLease(allow_storage_lease);
    }
    explicit operator bool() const { return held; }
    ~SleepLease() { if (held) --storage_leases; }
};
struct CpuPerformanceLease {
    static CpuPerformanceLease TryAcquire() { return {}; }
    ~CpuPerformanceLease() {}
};
}
namespace services {
using StorageTransaction = esp_err_t (*)(void*);
bool IsStorageServiceTask() { return on_storage_owner; }
esp_err_t ExecuteForegroundStorageTransaction(StorageTransaction, void*, const char*);
}
esp_err_t LoadAccessToken(std::string*);
}

@@STORAGE_TYPES@@
QueueHandle_t g_background_queue = reinterpret_cast<void*>(1);
QueueHandle_t g_foreground_queue = reinterpret_cast<void*>(2);
TaskHandle_t g_task = reinterpret_cast<void*>(3);
StorageCommand queued;
bool queue_has_command = false, queue_send_ok = true, semaphore_create_ok = true;
bool queued_timeout = false, running_timeout = false;
bool background_wait_retry = false;
unsigned caller_retry_delays = 0;
QueueHandle_t last_queue = nullptr;
unsigned nvs_reads = 0, infinite_waits = 0;
esp_err_t nvs_result = ESP_OK;
struct OwnerYield {};
void StorageServiceTask(void*);
uint32_t NextRequestId() { static uint32_t next = 1; return next++; }
bool ReceiveNextCommand(StorageCommand* command)
{
    if (!queue_has_command) return false;
    *command = queued;
    queue_has_command = false;
    return true;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage)
{
    return semaphore_create_ok ? storage : nullptr;
}
int xQueueSend(QueueHandle_t queue, const StorageCommand* command, TickType_t)
{
    CHECK(queue == g_foreground_queue || queue == g_background_queue);
    last_queue = queue;
    if (!queue_send_ok) return pdFALSE;
    CHECK(!queue_has_command);
    queued = *command;
    queue_has_command = true;
    return pdTRUE;
}
void xTaskNotifyGive(TaskHandle_t task) { CHECK(task == g_task); }
void ulTaskNotifyTake(int, TickType_t) { throw OwnerYield{}; }
void vTaskDelay(int ticks) {
    if (on_storage_owner) throw OwnerYield{};
    CHECK(ticks == 1); ++caller_retry_delays;
}
void PumpOwner()
{
    on_storage_owner = true;
    try { StorageServiceTask(nullptr); } catch (const OwnerYield&) {}
    on_storage_owner = false;
}
StaticSemaphore_t ui_mutex;
SemaphoreHandle_t g_lock = &ui_mutex;
int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->ready = true;
    return pdTRUE;
}
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    if (semaphore == &ui_mutex) return pdTRUE;
    if (background_wait_retry && ticks == portMAX_DELAY) {
        background_wait_retry = false;
        return pdFALSE;
    }
    if (queued_timeout) return pdFALSE;
    if (running_timeout && ticks != portMAX_DELAY) {
        // Controlled RTOS interleave: the owner won kQueued -> kRunning
        // while the caller's bounded wait expired. Finish after the caller
        // enters its mandatory unbounded wait, using its still-live context.
        queued.completion->state.store(kCompletionRunning);
        return pdFALSE;
    }
    if (ticks == portMAX_DELAY) ++infinite_waits;
    if (running_timeout) {
        CHECK(ticks == portMAX_DELAY);
        on_storage_owner = true;
        queued.completion->result = queued.transaction(queued.context);
        queued.completion->state.store(kCompletionDone);
        on_storage_owner = false;
        queue_has_command = false;
        xSemaphoreGive(semaphore);
    } else if (!semaphore->ready) {
        PumpOwner();
    }
    CHECK(semaphore->ready);
    semaphore->ready = false;
    return pdTRUE;
}

@@STORAGE_OWNER@@
@@STORAGE_CALLER@@
namespace wqn::services {
@@STORAGE_FOREGROUND@@
}
namespace wqn {
esp_err_t LoadAccessToken(std::string* token)
{
    CHECK(on_storage_owner);
    CHECK(storage_leases == 1);
    CHECK(token != nullptr);
    ++nvs_reads;
    if (nvs_result == ESP_OK) *token = "fixture-token";
    return nvs_result;
}
@@TOKEN@@
}

struct RecordingState {
    bool toast_visible = true;
    wqn::AiSessionStatus status = wqn::AiSessionStatus::kListening;
    int32_t toast_recording_ms = 0;
    std::string toast_label = "录音中";
} g_state;
unsigned revisions = 0;
void MarkChanged() { ++revisions; }
@@CLOCK@@

enum class ConnectionState { kDisconnected, kConnecting, kReady };
enum class TurnState { kIdle, kRecording, kFinalInFlight = 3,
                       kRemoteHandoff, kComplete = 6 };
std::atomic<TurnState> g_turn_state{TurnState::kIdle};
std::atomic<ConnectionState> g_conn_state{ConnectionState::kReady};
std::atomic<bool> g_session_ready{true}, g_is_connected{true};
uint32_t g_active_conn_op_gen = 0;
portMUX_TYPE g_idle_disconnect_mux = 0;
wqn::stdpro_ws::IdleDisconnectGate g_idle_disconnect_gate;
unsigned destroys = 0;
void DoDestroyWs()
{
    CHECK(!g_session_ready.load() && !g_is_connected.load());
    ++destroys;
    g_conn_state.store(ConnectionState::kDisconnected);
}
@@IDLE@@

struct BackgroundContext { bool open = false; unsigned calls = 0; };
int BeginBackground(void* pointer)
{
    auto* context = static_cast<BackgroundContext*>(pointer);
    CHECK(on_storage_owner); context->open = true; ++context->calls;
    return ESP_OK;
}
int AbortBackground(void* pointer)
{
    auto* context = static_cast<BackgroundContext*>(pointer);
    CHECK(on_storage_owner && context->open); context->open = false; ++context->calls;
    return ESP_OK;
}
void BackgroundTransactions()
{
    BackgroundContext context;
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "begin", false) == ESP_OK);
    CHECK(context.open && context.calls == 1 && last_queue == g_background_queue);
    const unsigned before_abort = completion_allocations;
    deny_completion_allocation = true;
    const auto result = ExecuteStorageTransactionInternal(AbortBackground, &context, "abort", false);
    CHECK(result == ESP_OK && !context.open && context.calls == 2);
    CHECK(completion_allocations == before_abort && !queue_has_command);
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "foreground", true) == ESP_ERR_NO_MEM);
    CHECK(!context.open && !queue_has_command && context.calls == 2);
    deny_completion_allocation = false;

    background_wait_retry = true;
    const auto delays_before = caller_retry_delays;
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "wait-retry", false) == ESP_OK);
    CHECK(context.open && context.calls == 3 && caller_retry_delays == delays_before + 1);
    CHECK(!queue_has_command);
    CHECK(ExecuteStorageTransactionInternal(AbortBackground, &context, "abort", false) == ESP_OK);
    CHECK(!context.open && context.calls == 4);

    const auto allocations_before = completion_allocations;
    queue_send_ok = false;
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "queue-reject", false) == ESP_ERR_TIMEOUT);
    CHECK(!queue_has_command && !context.open && context.calls == 4);
    queue_send_ok = true;
    semaphore_create_ok = false;
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "semaphore-reject", false) == ESP_ERR_NO_MEM);
    semaphore_create_ok = true;
    g_task = nullptr;
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "not-started", false) == ESP_ERR_INVALID_STATE);
    g_task = reinterpret_cast<void*>(3);
    CHECK(context.calls == 4 && completion_allocations == allocations_before);
    on_storage_owner = true;
    CHECK(ExecuteStorageTransactionInternal(BeginBackground, &context, "nested", false) == ESP_OK);
    CHECK(ExecuteStorageTransactionInternal(AbortBackground, &context, "nested", false) == ESP_OK);
    on_storage_owner = false;
    CHECK(context.calls == 6 && !context.open && completion_allocations == allocations_before);
    const auto reports_before_empty = wqn::runtime::fault_reports;
    deny_completion_allocation = true;
    CHECK(ExecuteStorageTransactionInternal([](void*) { return ESP_ERR_NOT_FOUND; },
        &context, "background-empty", false) == ESP_ERR_NOT_FOUND);
    CHECK(wqn::runtime::fault_reports == reports_before_empty);
    CHECK(ExecuteStorageTransactionInternal([](void*) { return ESP_FAIL; },
        &context, "background-error", false) == ESP_FAIL);
    CHECK(wqn::runtime::fault_reports == reports_before_empty + 1 &&
        wqn::runtime::fault_phase == wqn::runtime::FaultPhase::kStorageExecution);
    CHECK(completion_allocations == allocations_before && !queue_has_command);
    deny_completion_allocation = false;
}

int main()
{
    using wqn::LoadAccessTokenFromStorageService;
    std::string token = "stale";
    CHECK(LoadAccessTokenFromStorageService(nullptr) == ESP_ERR_INVALID_ARG);
    allow_storage_lease = false;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_ERR_INVALID_STATE);
    CHECK(token.empty() && storage_leases == 0 && nvs_reads == 0);
    allow_storage_lease = true;
    g_task = nullptr;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_ERR_INVALID_STATE);
    CHECK(token.empty() && nvs_reads == 0);
    g_task = reinterpret_cast<void*>(3);
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_OK);
    CHECK(token == "fixture-token" && nvs_reads == 1 && !queue_has_command);
    nvs_result = ESP_FAIL;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_FAIL);
    CHECK(token.empty() && nvs_reads == 2);
    CHECK(wqn::runtime::fault_reports == 1 && wqn::runtime::fault_phase == wqn::runtime::FaultPhase::kStorageExecution);
    CHECK(wqn::runtime::fault_code == ESP_FAIL && wqn::runtime::fault_operation != 0);
    nvs_result = ESP_OK;
    queue_send_ok = false;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_ERR_TIMEOUT);
    CHECK(nvs_reads == 2 && !queue_has_command);
    CHECK(wqn::runtime::fault_reports == 2 && wqn::runtime::fault_phase == wqn::runtime::FaultPhase::kStorageQueueTimeout);
    queue_send_ok = true;
    semaphore_create_ok = false;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_ERR_NO_MEM);
    CHECK(nvs_reads == 2);
    semaphore_create_ok = true;
    queued_timeout = true;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_ERR_TIMEOUT);
    CHECK(wqn::runtime::fault_reports == 3 && wqn::runtime::fault_code == ESP_ERR_TIMEOUT);
    CHECK(queued.completion->state.load() == kCompletionAbandoned);
    queued_timeout = false;
    PumpOwner(); // Production owner skips the abandoned caller context.
    CHECK(nvs_reads == 2 && !queue_has_command);
    running_timeout = true;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_OK);
    CHECK(token == "fixture-token" && infinite_waits == 1 && nvs_reads == 3);
    running_timeout = false;
    on_storage_owner = true;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_OK);
    on_storage_owner = false;
    CHECK(nvs_reads == 4 && !queue_has_command);
    CHECK(storage_leases == 0);

    const auto reports_before_empty_query = wqn::runtime::fault_reports;
    nvs_result = ESP_ERR_NOT_FOUND;
    CHECK(LoadAccessTokenFromStorageService(&token) == ESP_ERR_NOT_FOUND);
    CHECK(wqn::runtime::fault_reports == reports_before_empty_query);
    nvs_result = ESP_OK;

    BackgroundTransactions();

    SetAiRecordingLabel(0);
    CHECK(revisions == 1 && g_state.toast_label.find("00:00") != std::string::npos);
    for (int ms = 1; ms < 1000; ++ms) SetAiRecordingLabel(ms);
    CHECK(revisions == 1 && g_state.toast_recording_ms == 999);
    SetAiRecordingLabel(1000);
    CHECK(revisions == 2 && g_state.toast_label.find("00:01") != std::string::npos);
    SetAiRecordingLabel(61999);
    CHECK(revisions == 3 && g_state.toast_label.find("01:01") != std::string::npos);
    SetAiRecordingLabel(0); // A fresh recording resets the visible clock.
    CHECK(revisions == 4);
    g_state.toast_visible = false;
    SetAiRecordingLabel(9000);
    CHECK(revisions == 4 && g_state.toast_recording_ms == 0);
    g_state.toast_visible = true;
    g_state.status = wqn::AiSessionStatus::kIdle;
    SetAiRecordingLabel(9000);
    CHECK(revisions == 4);
    g_lock = nullptr;
    SetAiRecordingLabel(9000);
    CHECK(revisions == 4);

    TryIdleDisconnect();
    CHECK(destroys == 0);
    RequestIdleDisconnect();
    for (TurnState state : {TurnState::kRecording,
                           TurnState::kFinalInFlight, TurnState::kRemoteHandoff}) {
        g_turn_state.store(state);
        TryIdleDisconnect();
        CHECK(destroys == 0 && g_session_ready.load());
    }
    g_turn_state.store(TurnState::kIdle);
    g_conn_state.store(ConnectionState::kConnecting);
    TryIdleDisconnect();
    CHECK(destroys == 0);
    g_conn_state.store(ConnectionState::kReady);
    g_active_conn_op_gen = 12;
    TryIdleDisconnect();
    CHECK(destroys == 0);
    g_active_conn_op_gen = 0;
    TryIdleDisconnect();
    CHECK(destroys == 1);
    TryIdleDisconnect();
    CHECK(destroys == 1);

    RequestIdleDisconnect();
    g_idle_disconnect_gate.BeginUse(); // New connection supersedes old cleanup.
    g_idle_disconnect_gate.EndUse();
    TryIdleDisconnect();
    CHECK(destroys == 1);
    g_idle_disconnect_gate.BeginUse();
    RequestIdleDisconnect(); // Leave while an API caller still awaits completion.
    TryIdleDisconnect();
    CHECK(destroys == 1);
    g_idle_disconnect_gate.EndUse();
    TryIdleDisconnect();
    CHECK(destroys == 2);
    g_idle_disconnect_gate.BeginUse();
    g_idle_disconnect_gate.BeginUse();
    RequestIdleDisconnect();
    g_idle_disconnect_gate.EndUse();
    TryIdleDisconnect();
    CHECK(destroys == 2);
    g_idle_disconnect_gate.EndUse();
    TryIdleDisconnect();
    CHECK(destroys == 3);
    std::printf("audio runtime: %u checks passed\n", checks);
}
