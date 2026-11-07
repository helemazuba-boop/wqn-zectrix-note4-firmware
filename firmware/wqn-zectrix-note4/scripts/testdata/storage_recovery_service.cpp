#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include "services/storage_recovery_service.h"
#include "services/storage_service.h"
#include "psram_task_stack.h"
using namespace wqn;
using namespace wqn::runtime;
using namespace wqn::services;
std::atomic<unsigned> checks{0}, failures{0};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL recovery-service:%d: %s\n", __LINE__, #x); } } while (false)
struct Task {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned notifications = 0;
    bool stop = false;
    std::thread thread;
} task;
struct Stop {};
thread_local bool storage_owner = false;
std::mutex owner_mutex;
std::condition_variable owner_cv;
bool block_owner = false;
std::atomic<unsigned> owner_entered{0}, inspect_calls{0}, reset_calls{0}, ui_notifications{0};
std::atomic<bool> wrong_owner{false};
int start_error = ESP_OK, delivery_error = ESP_OK;
bool fail_alloc = false, fail_create = false, preservation_only = false;
unsigned allocations = 0, frees = 0;
void* allocation = nullptr;
void* ui = reinterpret_cast<void*>(1);
StorageRecoverySnapshot returned_snapshot;
StorageRecoverySnapshot observed_expected;
StorageRecoveryResetResult returned_reset;
int64_t esp_timer_get_time() { return 1000000; }
void HostLog(const char*, const char*, ...) {}
namespace wqn {
StackType_t* AllocTaskStack(size_t bytes, const char*, bool*) {
    if (fail_alloc) return nullptr;
    CHECK(bytes == 8192); ++allocations;
    allocation = std::malloc(bytes); return static_cast<StackType_t*>(allocation);
}
}
void heap_caps_free(void* pointer) { CHECK(pointer == allocation); ++frees; std::free(pointer); allocation = nullptr; }
TaskHandle_t xTaskCreateStatic(void(*entry)(void*), const char*, uint32_t depth, void* arg,
                             unsigned, StackType_t* stack, StaticTask_t*) {
    CHECK(depth == 8192/sizeof(StackType_t) && stack == allocation);
    if (fail_create) return nullptr;
    task.thread = std::thread([=] { try { entry(arg); } catch (const Stop&) {} });
    return &task;
}
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks) {
    CHECK(clear == pdTRUE && ticks == portMAX_DELAY);
    std::unique_lock<std::mutex> lock(task.mutex);
    task.cv.wait(lock, [] { return task.notifications != 0 || task.stop; });
    if (task.stop) throw Stop{};
    const auto count = task.notifications; task.notifications = 0; return count;
}
int xTaskNotifyGive(TaskHandle_t target) {
    if (target == ui) { ui_notifications.fetch_add(1); return pdTRUE; }
    if (target != &task) std::abort();
    std::lock_guard<std::mutex> lock(task.mutex); ++task.notifications; task.cv.notify_all(); return pdTRUE;
}
namespace wqn::services {
esp_err_t RunStorageRecoveryExportOnOwner(StorageRecoveryExportKind kind, bool(*cancelled)(void*), void* context,
    int64_t deadline, void(*progress)(uint32_t,void*), StorageRecoveryExportResult* result) {
    CHECK(storage_owner && deadline == 1801000000LL);
    CHECK(kind == StorageRecoveryExportKind::kRaw || kind == StorageRecoveryExportKind::kLogical);
    progress(1234, context); result->persisted_data_bytes = 1234;
    return result->error = cancelled(context) ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t StartStorageService() { return start_error; }
bool IsStorageServiceTask() { return storage_owner; }
esp_err_t ExecuteStorageTransactionNamed(StorageTransaction transaction, void* context, const char*) {
    owner_entered.fetch_add(1);
    std::unique_lock<std::mutex> lock(owner_mutex);
    owner_cv.wait(lock, [] { return !block_owner; });
    if (delivery_error != ESP_OK) return delivery_error;
    storage_owner = true;
    const auto result = transaction(context);
    storage_owner = false;
    return result;
}
}
namespace wqn::runtime {
esp_err_t ReadStorageEvidenceBootContextOnOwner(StorageEvidenceBootContext* out) {
    CHECK(storage_owner); out->preservation_only = preservation_only; return ESP_OK;
}
esp_err_t InspectStorageRecovery(StorageRecoverySnapshot* out) {
    if (!storage_owner) wrong_owner.store(true);
    inspect_calls.fetch_add(1); *out = returned_snapshot; return returned_snapshot.error;
}
StorageRecoveryResetResult AuthorizeStorageRecoveryReset(const StorageRecoverySnapshot& expected) {
    if (!storage_owner) wrong_owner.store(true);
    reset_calls.fetch_add(1); observed_expected = expected; return returned_reset;
}
}
template<class Predicate> bool Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
int main() {
    uint32_t id = 0; StorageRecoveryResult result;
    CHECK(!SubmitStorageRecoveryInspect(&id));
    CHECK(StartStorageRecoveryService(nullptr) == ESP_ERR_INVALID_ARG);
    start_error = ESP_FAIL; CHECK(StartStorageRecoveryService(ui) == ESP_FAIL);
    start_error = ESP_OK; fail_alloc = true; CHECK(StartStorageRecoveryService(ui) == ESP_ERR_NO_MEM);
    fail_alloc = false; fail_create = true; CHECK(StartStorageRecoveryService(ui) == ESP_ERR_NO_MEM);
    CHECK(allocations == 1 && frees == 1);
    fail_create = false; CHECK(StartStorageRecoveryService(ui) == ESP_OK);
    CHECK(StartStorageRecoveryService(ui) == ESP_OK);
    CHECK(StartStorageRecoveryService(reinterpret_cast<void*>(2)) == ESP_ERR_INVALID_STATE);
    CHECK(!ReadStorageRecoveryResult(nullptr) && !ReadStorageRecoveryResult(&result));
    CHECK(!AcknowledgeStorageRecoveryResult(0));
    returned_snapshot.error = ESP_OK; returned_snapshot.observed_generation = 2;
    returned_snapshot.reset_policy = StorageRecoveryResetPolicy::kAllowed;
    { std::lock_guard<std::mutex> lock(owner_mutex); block_owner = true; }
    CHECK(SubmitStorageRecoveryInspect(&id)); CHECK(id != 0);
    CHECK(Wait([] { return owner_entered.load() == 1; }));
    CHECK(!SubmitStorageRecoveryInspect(nullptr));
    uint32_t ignored = 0;
    for (unsigned i = 0; i < 20; ++i) CHECK(!SubmitStorageRecoveryInspect(&ignored));
    CHECK(!ReadStorageRecoveryResult(&result));
    CHECK(!AcknowledgeStorageRecoveryResult(id));
    { std::lock_guard<std::mutex> lock(owner_mutex); block_owner = false; owner_cv.notify_all(); }
    CHECK(Wait([&] { return ReadStorageRecoveryResult(&result); }));
    CHECK(result.operation_id == id && result.error == ESP_OK);
    CHECK(result.operation == StorageRecoveryOperation::kInspect && result.snapshot.observed_generation == 2);
    CHECK(!SubmitStorageRecoveryInspect(&ignored));
    CHECK(!AcknowledgeStorageRecoveryResult(id+1));
    CHECK(ReadStorageRecoveryResult(&result));
    CHECK(AcknowledgeStorageRecoveryResult(id)); CHECK(!AcknowledgeStorageRecoveryResult(id));
    for (auto outcome : {StorageRecoveryResetOutcome::kRecorded, StorageRecoveryResetOutcome::kUncertain,
                         StorageRecoveryResetOutcome::kRejected}) {
        returned_reset = {outcome == StorageRecoveryResetOutcome::kRecorded ? ESP_OK : ESP_FAIL, outcome};
        auto expected = returned_snapshot; expected.observed_generation = 1;
        { std::lock_guard<std::mutex> lock(owner_mutex); block_owner = true; }
        CHECK(SubmitStorageRecoveryReset(expected, &id)); expected.observed_generation = 99;
        { std::lock_guard<std::mutex> lock(owner_mutex); block_owner = false; owner_cv.notify_all(); }
        CHECK(Wait([&] { return ReadStorageRecoveryResult(&result); }));
        CHECK(result.operation_id == id && result.operation == StorageRecoveryOperation::kAuthorizeReset);
        CHECK(result.reset.outcome == outcome && result.error == returned_reset.error);
        CHECK(observed_expected.observed_generation == 1); // No borrowed stack context.
        CHECK(!AcknowledgeStorageRecoveryResult(id-1));
        CHECK(AcknowledgeStorageRecoveryResult(id));
    }
    delivery_error = ESP_ERR_NO_MEM;
    CHECK(SubmitStorageRecoveryReset(returned_snapshot, &id));
    CHECK(Wait([&] { return ReadStorageRecoveryResult(&result); }));
    CHECK(result.error == ESP_ERR_NO_MEM && result.reset.outcome == StorageRecoveryResetOutcome::kRejected);
    CHECK(AcknowledgeStorageRecoveryResult(id));
    delivery_error = ESP_OK;
    CHECK(!SubmitStorageRecoveryExport(StorageRecoveryExportKind::kRaw, false, &id));
    CHECK(!SubmitStorageRecoveryExport(static_cast<StorageRecoveryExportKind>(99), true, &id));
    for (auto kind : {StorageRecoveryExportKind::kRaw, StorageRecoveryExportKind::kLogical}) {
        { std::lock_guard<std::mutex> lock(owner_mutex); block_owner = true; }
        CHECK(SubmitStorageRecoveryExport(kind, true, &id));
        CHECK(!CancelStorageRecoveryExport(id+1));
        uint32_t progress = 999;
        CHECK(ReadStorageRecoveryExportProgress(id, &progress) && progress == 0);
        CHECK(CancelStorageRecoveryExport(id));
        CHECK(!AcknowledgeStorageRecoveryResult(id));
        { std::lock_guard<std::mutex> lock(owner_mutex); block_owner = false; owner_cv.notify_all(); }
        CHECK(Wait([&] { return ReadStorageRecoveryResult(&result); }));
        CHECK(result.operation_id == id && result.error == ESP_ERR_INVALID_STATE);
        CHECK(ReadStorageRecoveryExportProgress(id, &progress) && progress == 1234);
        CHECK(!CancelStorageRecoveryExport(id));
        CHECK(!SubmitStorageRecoveryInspect(&ignored));
        CHECK(!AcknowledgeStorageRecoveryResult(id+1));
        CHECK(AcknowledgeStorageRecoveryResult(id));
        CHECK(!ReadStorageRecoveryExportProgress(id, &progress));
    }
    preservation_only = true;
    for (int operation = 0; operation < 3; ++operation) {
        CHECK(operation == 0 ? SubmitStorageRecoveryInspect(&id) :
              operation == 1 ? SubmitStorageRecoveryReset(returned_snapshot, &id) :
              SubmitStorageRecoveryExport(StorageRecoveryExportKind::kLogical, true, &id));
        CHECK(Wait([&] { return ReadStorageRecoveryResult(&result); }));
        CHECK(result.error == ESP_ERR_INVALID_STATE);
        CHECK(AcknowledgeStorageRecoveryResult(id));
    }
    CHECK(SubmitStorageRecoveryExport(StorageRecoveryExportKind::kRaw, true, &id));
    CHECK(Wait([&] { return ReadStorageRecoveryResult(&result); }));
    CHECK(result.error == ESP_OK && result.exported.persisted_data_bytes == 1234);
    CHECK(AcknowledgeStorageRecoveryResult(id));
    CHECK(inspect_calls == 1 && reset_calls == 3 && !wrong_owner);
    CHECK(Wait([] { return ui_notifications.load() == 11; }));
    { std::lock_guard<std::mutex> lock(task.mutex); task.stop = true; task.cv.notify_all(); }
    task.thread.join(); heap_caps_free(allocation);
    CHECK(allocations == frees);
    std::printf("Storage recovery service: %u checks, %u failures (NOT HIL)\n", checks.load(), failures.load());
    return failures ? 1 : 0;
}
