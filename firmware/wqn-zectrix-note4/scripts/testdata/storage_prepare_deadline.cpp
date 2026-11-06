// Full production storage service and sleep coordinator are separate TUs.
// This TU uses actual maintenance context/step/driver/sleep request and power
// boundary, with VFS/cache payload and RTOS/PM ports. Not device/HIL evidence.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/storage_service.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/fault_diagnostics.h"
#include "power/sleep_protocol.h"

using namespace wqn::runtime;
using namespace std::chrono_literals;
std::atomic<int64_t> clock_us{1000000};
std::atomic<int> allocation_count{0},fail_allocation{0};
std::atomic<int> completed{0},checkpoints{0},compactions{0},cache_loads{0},cleanups{0};
std::atomic<bool> hold_owner{false},hold_io{false},io_entered{false},hold_give{false},give_entered{false};
std::mutex control;
std::condition_variable changed;
std::thread owner;
thread_local bool on_owner=false;
bool stopping=false,notifications=false;
int owner_identity;
size_t checks=0,failures=0;
int64_t admission_cost_us=0;
bool queue_rejected=false,semaphore_rejected=false,expire_at_notify=false,expire_in_cpu=false;
TickType_t admission_ticks=0,observation_ticks=0;
enum class WaitMode { kNormal,kQueuedTimeout,kRunningTimeout,kLateComplete,kPublicationRace };
WaitMode wait_mode=WaitMode::kNormal;
enum class Stage { kNone,kCache,kCheckpoint,kCompact };
Stage stall_stage=Stage::kNone;
esp_err_t io_result=ESP_OK;
struct Stop {};
struct HostPm { int kind=0; std::atomic<int> holds{0}; } light_pm,cpu_pm;

void Check(bool pass,const char* message)
{
    ++checks;
    if (!pass) {
        if (failures<20) std::fprintf(stderr,"FAIL %s at %lld us\n",message,static_cast<long long>(clock_us.load()));
        ++failures;
    }
}
template <class Predicate> void UntilImpl(Predicate predicate,int line)
{
    std::unique_lock<std::mutex> lock(control);
    if (!changed.wait_for(lock,2s,predicate)) {
        std::fprintf(stderr,"HARNESS control wait did not settle at line %d, stage=%d completed=%d entered=%d clock=%lld\n",
            line,static_cast<int>(stall_stage),completed.load(),static_cast<int>(io_entered.load()),
            static_cast<long long>(clock_us.load())); std::abort();
    }
}
#define Until(predicate) UntilImpl(predicate,__LINE__)
void* operator new(size_t bytes,const std::nothrow_t&) noexcept
{
    if (++allocation_count==fail_allocation) return nullptr;
    try { return ::operator new(bytes); } catch(...) { return nullptr; }
}
void operator delete(void* p,const std::nothrow_t&) noexcept { ::operator delete(p); }
void HostLog(const char*,const char*,...) {}
int64_t esp_timer_get_time() { return clock_us.load(); }
int esp_pm_lock_create(int kind,int,const char*,esp_pm_lock_handle_t* output)
{ *output=kind==ESP_PM_NO_LIGHT_SLEEP?&light_pm:&cpu_pm; (*output)->kind=kind; return ESP_OK; }
int esp_pm_lock_acquire(esp_pm_lock_handle_t lock) {
    ++lock->holds;
    if (lock==&cpu_pm && expire_in_cpu) clock_us=2000000;
    return ESP_OK;
}
int esp_pm_lock_release(esp_pm_lock_handle_t lock) { --lock->holds; return ESP_OK; }
namespace wqn::runtime {
void RecordFault(FaultDomain,FaultPhase,int32_t,uint32_t,uint32_t) {}
}
QueueHandle_t xQueueCreateStatic(unsigned depth,size_t size,uint8_t*,StaticQueue_t* queue)
{ queue->depth=depth; queue->item_size=size; return queue; }
int xQueueSend(QueueHandle_t queue,const void* payload,TickType_t ticks)
{
    admission_ticks=ticks;
    clock_us+=admission_cost_us;
    if (queue_rejected) { clock_us+=static_cast<int64_t>(ticks)*1000000/configTICK_RATE_HZ; return pdFALSE; }
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.size()>=queue->depth) return pdFALSE;
    const auto* bytes=static_cast<const uint8_t*>(payload);
    queue->items.emplace_back(bytes,bytes+queue->item_size);
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue,void* payload,TickType_t)
{
    if (hold_owner) return pdFALSE;
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.empty()) return pdFALSE;
    std::memcpy(payload,queue->items.front().data(),queue->item_size); queue->items.pop_front();
    return pdTRUE;
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return on_owner?&owner_identity:nullptr; }
int xTaskCreate(void (*entry)(void*),const char*,unsigned,void* arg,unsigned,TaskHandle_t* task)
{
    *task=&owner_identity;
    owner=std::thread([=] { on_owner=true; try { entry(arg); } catch(const Stop&) {} });
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t)
{
    const int before=completed.load();
    if (expire_at_notify) clock_us+=1000000;
    { std::lock_guard<std::mutex> lock(control); notifications=true; }
    changed.notify_all();
    if (expire_at_notify) Until([=] { return completed>before; });
}
unsigned ulTaskNotifyTake(int,TickType_t)
{
    std::unique_lock<std::mutex> lock(control);
    changed.wait(lock,[] { return stopping || (notifications && !hold_owner); });
    if (stopping) throw Stop{};
    notifications=false; return 1;
}
void vTaskDelay(TickType_t) { ++completed; changed.notify_all(); std::this_thread::yield(); }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 12000; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage)
{ return semaphore_rejected?nullptr:storage; }
int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    { std::lock_guard<std::mutex> lock(semaphore->mutex); semaphore->ready=true; }
    semaphore->cv.notify_all();
    if (hold_give) {
        give_entered=true; changed.notify_all();
        Until([] { return !hold_give; });
        // Deliberately access the semaphore AFTER caller has received it.
        // The command must retain ownership across the entire Give operation.
        std::lock_guard<std::mutex> lock(semaphore->mutex);
        if (semaphore->ready) std::abort();
    }
    return pdTRUE;
}
void ReleaseIo() { hold_io=false; changed.notify_all(); }
int xSemaphoreTake(SemaphoreHandle_t semaphore,TickType_t ticks)
{
    observation_ticks=ticks;
    if (ticks==0) std::abort();
    if (wait_mode==WaitMode::kQueuedTimeout) {
        clock_us+=static_cast<int64_t>(ticks)*1000000/configTICK_RATE_HZ;
        return pdFALSE;
    }
    if (wait_mode==WaitMode::kRunningTimeout || wait_mode==WaitMode::kLateComplete) {
        Until([] { return io_entered.load(); });
        if (ticks==portMAX_DELAY) {
            // Finite baseline observation: let the old unbounded waiter finish
            // 1 s beyond the requested deadline, without invalidating its stack.
            clock_us=3000000; ReleaseIo();
        } else {
            clock_us+=static_cast<int64_t>(ticks)*1000000/configTICK_RATE_HZ;
            if (wait_mode==WaitMode::kRunningTimeout) return pdFALSE;
            ReleaseIo();
        }
    }
    if (wait_mode==WaitMode::kPublicationRace) Until([] { return give_entered.load(); });
    std::unique_lock<std::mutex> lock(semaphore->mutex);
    if (!semaphore->cv.wait_for(lock,2s,[=] { return semaphore->ready; })) std::abort();
    semaphore->ready=false; return pdTRUE;
}
void Stall(Stage stage)
{
    if (stage!=stall_stage || !hold_io) return;
    io_entered=true; changed.notify_all();
    Until([] { return !hold_io; });
}
void HostCleanup() { ++cleanups; changed.notify_all(); }

constexpr char kTag[]="sleep-test";
#define ESP_RETURN_ON_ERROR(expr,...) do { const auto result=(expr); if (result!=ESP_OK) return result; } while(false)
namespace wqn {
template<class T> using WordStorePsramAllocator=std::allocator<T>;
struct WordOutboxMaintenanceGate { bool (*should_defer)(void*)=nullptr; void* context=nullptr; };
uint32_t GetDeckScopeGeneration() { return 1; }
bool IsWordDeckScopeReady() { return true; }
}
// Journal/cache contents and VFS calls are ports; the mutation/proof/step and
// request lifetime code around those calls is the unmodified production code.
struct OutboxRecord { uint64_t ordinal=1; };
struct OutboxScan {
    std::vector<OutboxRecord> acknowledged{{1}},pending,suspended;
    size_t ack_records=128;
    bool partial_tail=false,backup_source=false;
} scan;
uint64_t g_session_mutation_generation=0;
constexpr size_t kRuntimeCompactAckThreshold=64;
constexpr int kPersistedSessionModes[]={0,1,2,3,4,5,6};
esp_err_t CheckOutboxAppendBudget(const OutboxScan&,bool) { return ESP_OK; }
esp_err_t EnsureOutboxCache(OutboxScan** output)
{ ++cache_loads; Stall(Stage::kCache); *output=&scan; return io_result; }
esp_err_t CheckpointSessionFromOutbox(const OutboxScan&,int)
{ ++checkpoints; Stall(Stage::kCheckpoint); return io_result; }
esp_err_t CompactCachedOutbox(OutboxScan* cache)
{ ++compactions; Stall(Stage::kCompact); if (io_result==ESP_OK) cache->acknowledged.clear(); return io_result; }
@@CONTEXT@@
@@STEP@@
@@DRIVER@@
namespace wqn {
@@PREPARE@@
#if HAS_OWNED
int audio_prepares=0,connectivity_prepares=0;
esp_err_t PrepareDisplayForSleep(int64_t) { clock_us+=1000; return ESP_OK; }
namespace services {
esp_err_t PrepareAudioServiceForSleep(const power::PrepareSleepCommand&)
{ ++audio_prepares; clock_us+=1000; return ESP_OK; }
esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand&)
{ ++connectivity_prepares; clock_us+=1000; return ESP_OK; }
}
@@POWER@@
#endif
}

void AllowOwner()
{
    hold_owner=false;
    { std::lock_guard<std::mutex> lock(control); notifications=true; }
    changed.notify_all();
}
void Reset()
{
    hold_owner=true; hold_io=false; hold_give=false; changed.notify_all();
    Check(!HasActiveSleepBlockers() && light_pm.holds==0,"previous request has no residual storage/PM hold");
    clock_us=1000000; cleanups=checkpoints=compactions=cache_loads=0;
    allocation_count=fail_allocation=0; io_entered=give_entered=false;
    queue_rejected=semaphore_rejected=expire_at_notify=expire_in_cpu=false;
    admission_cost_us=0; admission_ticks=observation_ticks=0;
    wait_mode=WaitMode::kNormal; stall_stage=Stage::kNone; io_result=ESP_OK; scan={};
    Check(TryBeginSleepQuiesce(42),"normal power preparation admits with storage blocker zero");
}
esp_err_t Prepare(int64_t deadline=2000000,uint32_t generation=42)
{
#if HAS_OWNED
    return wqn::PrepareStorageForSleep(deadline,generation);
#else
    (void)generation;
    return wqn::PrepareStorageForSleep(deadline);
#endif
}
void Finish(int before,bool abandoned=false)
{
    ReleaseIo(); hold_give=false; AllowOwner();
    Until([=] { return completed>before || (abandoned && cleanups==1); });
    Check(!HasActiveSleepBlockers() && light_pm.holds==0,"terminal owner releases payload hold and no-light-sleep lock");
}
void Running(Stage stage)
{
    Reset(); stall_stage=stage; hold_io=true; wait_mode=WaitMode::kRunningTimeout; AllowOwner();
    const int before=completed.load();
    const auto result=Prepare();
    Check(result==ESP_ERR_TIMEOUT && clock_us<=2010000,"power caller returns timeout at one absolute deadline");
#if HAS_OWNED
    Check(cleanups==0 && ActiveSleepBlockerCount(SleepBlocker::kStorage)==1 && light_pm.holds==1,
          "detached running request retains independent payload and actual sleep/PM blocker");
    CancelSleepQuiesce(42);
    Check(!TryBeginSleepQuiesce(43),"next normal sleep attempt cannot overtake a detached write");
    Check(Prepare(4000000,43)==ESP_ERR_INVALID_STATE,"storage boundary refuses a second preparation while old owner writes");
    Finish(before);
    Check(cleanups==1,"running request payload is destroyed exactly once after owner use");
    Check(stage==Stage::kCompact ? compactions==1 : compactions==0,"no new reclaim is started after timeout");
    if (stage==Stage::kCache) Check(checkpoints==0,"cache load crossing deadline cannot start a new checkpoint");
    Check(TryBeginSleepQuiesce(44),"sleep admission recovers after detached owner finishes");
    CancelSleepQuiesce(44);
#else
    Finish(before); CancelSleepQuiesce(42);
#endif
}
#if HAS_OWNED
void LeaseIdentity()
{
    Reset();
    Check(!SleepLease::TryAcquire(SleepBlocker::kStorage,"ordinary",__FILE__,__LINE__),"ordinary lease admission stays closed");
    Check(!SleepLease::TryAcquireStoragePreparation(0,"zero",__FILE__,__LINE__),"zero generation cannot authorize preparation");
    Check(!SleepLease::TryAcquireStoragePreparation(43,"stale",__FILE__,__LINE__),"wrong generation cannot authorize preparation");
    auto hold=SleepLease::TryAcquireStoragePreparation(42,"current",__FILE__,__LINE__);
    Check(static_cast<bool>(hold) && ActiveSleepBlockerCount(SleepBlocker::kStorage)==1,"current quiesce generation owns a real storage lease");
    CancelSleepQuiesce(42);
    Check(!SleepLease::TryAcquireStoragePreparation(42,"late",__FILE__,__LINE__),"rolled-back generation cannot authorize new preparation");
    Check(!TryBeginSleepQuiesce(43),"remaining preparation hold blocks next normal quiesce");
    hold.Reset();
    Check(TryBeginSleepQuiesce(43),"lease release restores normal quiesce admission"); CancelSleepQuiesce(43);
}
void QueueBudget()
{
    Reset(); admission_cost_us=900000; wait_mode=WaitMode::kQueuedTimeout;
    const int before=completed.load();
    Check(Prepare()==ESP_ERR_TIMEOUT && clock_us<=2010000,"queue admission and result observation share one budget");
    Check(observation_ticks<=pdMS_TO_TICKS(100),"observation receives only remaining queue budget");
    Check(cleanups==0 && ActiveSleepBlockerCount(SleepBlocker::kStorage)==1,"queued abandon keeps independently owned command alive until drained");
    Finish(before,true);
    Check(checkpoints==0 && cache_loads==0 && compactions==0 && cleanups==1,"abandoned queued context is reaped without running VFS");
    CancelSleepQuiesce(42);
}
void LateComplete()
{
    Reset(); stall_stage=Stage::kCheckpoint; hold_io=true; wait_mode=WaitMode::kLateComplete; AllowOwner();
    const int before=completed.load();
    Check(Prepare()==ESP_ERR_TIMEOUT,"late completion signal cannot publish Ready after deadline");
    Finish(before); Check(cleanups==1 && compactions==0,"late checkpoint completes safely without new reclaim");
    CancelSleepQuiesce(42);
}
void PublicationRace()
{
    Reset(); hold_give=true; wait_mode=WaitMode::kPublicationRace; AllowOwner();
    const int before=completed.load();
    Check(Prepare()==ESP_OK && checkpoints==7 && compactions==1,"healthy owner preserves all seven checkpoints and reclamation");
    Check(cleanups==0 && !HasActiveSleepBlockers(),"command reference protects semaphore publication after caller returns");
    Finish(before); Check(cleanups==1,"last publication reference releases payload exactly once");
    CancelSleepQuiesce(42);
}
struct GenericPayload { SleepLease hold; explicit GenericPayload(SleepLease&& lease):hold(std::move(lease)) {} };
std::atomic<int> generic_calls{0},generic_cleanups{0};
esp_err_t GenericTransaction(void*) { ++generic_calls; return ESP_OK; }
void GenericCleanup(void* p) { ++generic_cleanups; delete static_cast<GenericPayload*>(p); }
void ExpiredOwner()
{
    for(bool during_cpu_acquire: {false,true}) {
        Reset(); generic_calls=generic_cleanups=0;
        expire_at_notify=!during_cpu_acquire; expire_in_cpu=during_cpu_acquire; AllowOwner();
        const int before=completed.load();
        auto hold=SleepLease::TryAcquireStoragePreparation(42,"expired",__FILE__,__LINE__);
        auto* payload=new GenericPayload(std::move(hold));
        Check(wqn::services::ExecuteOwnedStorageTransactionUntil(GenericTransaction,payload,GenericCleanup,"expired",2000000)==ESP_ERR_TIMEOUT,
              "request expiring at dispatch or during owner setup returns timeout");
        Until([=] { return completed>before; });
        Check(generic_calls==0 && generic_cleanups==1,"owner rechecks current time immediately before invoking transaction");
        CancelSleepQuiesce(42);
    }
}
void Faults()
{
    for(int failure: {1,2,3,4}) {
        Reset();
        if (failure<=2) fail_allocation=failure;
        if (failure==3) semaphore_rejected=true;
        if (failure==4) queue_rejected=true;
        const auto result=Prepare();
        Check(result==(failure==4?ESP_ERR_TIMEOUT:ESP_ERR_NO_MEM),"allocation/semaphore/admission failures propagate");
        Check(!HasActiveSleepBlockers() && light_pm.holds==0,"pre-admission failure releases preparation hold");
        Check(cleanups==(failure==1?0:1),"failed request frees transferred payload exactly once");
        CancelSleepQuiesce(42);
    }
    Reset(); Check(Prepare(1000000)==ESP_ERR_TIMEOUT && allocation_count==0,"already expired power command performs no allocation/dispatch");
    Check(Prepare(2000000,43)==ESP_ERR_INVALID_STATE && allocation_count==0,"stale power generation rejected before allocation/dispatch");
    CancelSleepQuiesce(42);
    Reset(); io_result=ESP_FAIL; AllowOwner(); const int before=completed.load();
    Check(Prepare()==ESP_FAIL,"owner I/O error is retained before deadline");
    Finish(before); Check(cleanups==1,"I/O failure retires owned payload"); CancelSleepQuiesce(42);
}
void PowerBroadcast()
{
    for(bool stall: {false,true}) {
        Reset(); wqn::audio_prepares=wqn::connectivity_prepares=0;
        if(stall) { stall_stage=Stage::kCheckpoint; hold_io=true; wait_mode=WaitMode::kRunningTimeout; }
        AllowOwner(); const int before=completed.load();
        wqn::power::PrepareSleepCommand command;
        command.generation=42; command.deadline_us=clock_us+43000000;
        const auto results=wqn::BroadcastPrepareSleep(command);
        const auto storage=results[static_cast<size_t>(wqn::power::SleepService::kStorage)];
        Check(storage.generation==42 && storage.status==(stall?wqn::power::SleepPrepareStatus::kTimedOut:wqn::power::SleepPrepareStatus::kReady),
              "actual power broadcast passes owning generation and reports Ready versus timeout correctly");
        Check(wqn::AllServicesReady(command,results)==!stall,"actual normal-sleep readiness gate rejects timed-out storage");
        Check(wqn::audio_prepares==1 && wqn::connectivity_prepares==1 && clock_us<=6013000,
              "actual power broadcast continues later services within the storage budget");
        Finish(before); CancelSleepQuiesce(42);
    }
}
#endif
int main(int argc,char** argv)
{
    Check(InitSleepCoordinator()==ESP_OK && wqn::services::StartStorageService()==ESP_OK,"real coordinators initialize with host ports");
    const std::string selected=argc>1?argv[1]:"all";
    if(selected=="all" || selected=="running") Running(Stage::kCheckpoint);
#if HAS_OWNED
    if(selected=="cache") Running(Stage::kCache);
    if(selected=="all") { Running(Stage::kCache); Running(Stage::kCompact); Faults(); PowerBroadcast(); }
    if(selected=="all" || selected=="lease") LeaseIdentity();
    if(selected=="all" || selected=="queue") QueueBudget();
    if(selected=="all" || selected=="late") LateComplete();
    if(selected=="all" || selected=="publication") PublicationRace();
    if(selected=="all" || selected=="expired") ExpiredOwner();
#endif
    { std::lock_guard<std::mutex> lock(control); stopping=true; }
    changed.notify_all(); owner.join();
    std::printf("Storage preparation deadline: %zu checks, %zu failures; %d Hz; real service/coordinator + host threads/VFS ports; NOT HIL\n",
                checks,failures,configTICK_RATE_HZ);
    return failures?1:0;
}
