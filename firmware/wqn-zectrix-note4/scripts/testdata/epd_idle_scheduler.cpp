#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

using TickType_t=uint32_t;
using esp_err_t=int;
const char* esp_err_to_name(int){return "host-result";}
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1,ESP_OK=0,ESP_FAIL=1,ESP_ERR_TIMEOUT=2;
#define CONFIG_ESP_TASK_WDT_EN 1
int off_ms=5000,cleanup_ms=45000;
#define CONFIG_WQN_EPD_IDLE_POWER_OFF_MS ::off_ms
#define CONFIG_WQN_EPD_IDLE_CLEANUP_MS ::cleanup_ms
int checks=0;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();}++checks;}while(0)
template<class... T>void HostLog(T...){}
#define ESP_LOGD(...) HostLog(__VA_ARGS__)
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
int64_t now_ms=100;
int64_t esp_timer_get_time(){return now_ms*1000;}
TickType_t xTaskGetTickCount(){return static_cast<TickType_t>(now_ms*configTICK_RATE_HZ/1000);}
void* xTaskGetCurrentTaskHandle(){return nullptr;}
int wdt_balance=0,wdt_entries=0,live_leases=0,lease_admissions=0;
int esp_task_wdt_add(void*){++wdt_entries;++wdt_balance;return ESP_OK;}
void esp_task_wdt_delete(void*){--wdt_balance;}
bool lease_allowed=true,operation_allowed=true;
std::function<void()> on_lease;
namespace wqn::runtime {
enum class SleepBlocker {kDisplay};
struct SleepLease {
    bool held=false;
    SleepLease(bool admitted):held(admitted){if(held){++live_leases;++lease_admissions;}}
    SleepLease(const SleepLease&)=delete;
    ~SleepLease(){if(held)--live_leases;}
    explicit operator bool()const{return held;}
    static SleepLease TryAcquire(SleepBlocker,const char*,const char*,int){
        if(on_lease){auto callback=std::move(on_lease);on_lease={};callback();}
        return SleepLease(lease_allowed);
    }
};
}
namespace wqn {
constexpr char kTag[]="host-epd";
@@DEBT_LIMITS@@
bool g_initialized=true,g_epd_rail_powered=true,g_previous_framebuffer_synced=true;
uint8_t byte=0;
uint8_t* g_framebuffer=&byte;
uint32_t g_partial_refreshes_since_full=0,g_heavy_partials_since_full=0;
std::atomic<int64_t> g_last_epd_activity_ms{0};
std::atomic<uint32_t> g_epd_activity_generation{0};
std::atomic<bool> g_epd_idle_cut{false},g_epd_cleanup_deferred_logged{false};
std::vector<int64_t> cuts,cleans;
int off_result=ESP_OK,cleanup_result=ESP_OK;
struct EpdOperationGuard {explicit EpdOperationGuard(int){}bool locked()const{return operation_allowed;}};
int RefreshEpdFull(bool local,bool full){
    CHECK(!local&&full);cleans.push_back(now_ms);
    CHECK(live_leases==1);
    if(cleanup_result!=ESP_OK)return cleanup_result;
    g_partial_refreshes_since_full=g_heavy_partials_since_full=0;
    g_epd_rail_powered=false;return ESP_OK;
}
int TryPowerOffEpd(int timeout){
    CHECK(timeout==0);CHECK(live_leases==1);
    if(off_result!=ESP_OK||!operation_allowed)return ESP_ERR_TIMEOUT;
    cuts.push_back(now_ms);g_epd_rail_powered=false;return ESP_OK;
}
void NoteEpdActivity();uint32_t GetEpdActivityGeneration();void PowerOffEpdAfterIdleIfNeeded();
@@DRIVER@@
bool prep_success=true;
int prep_claims=0,prep_signals=0,shutdown_claims=0;
constexpr uint32_t kSleepPrepIdle=0,kSleepPrepPending=1,kSleepPrepClaimed=2;
std::atomic<uint32_t> g_sleep_prep_state{kSleepPrepIdle},g_sleep_prep_generation{31};
std::atomic<uint32_t> g_sleep_prep_result_generation{0};
std::atomic<bool> g_sleep_prep_clear{false};
std::atomic<esp_err_t> g_sleep_prep_result{ESP_FAIL};
void* SleepPrepDoneSemaphore(){return &prep_signals;}
void xSemaphoreGive(void* semaphore){
    CHECK(semaphore==&prep_signals);CHECK(g_sleep_prep_state==kSleepPrepClaimed);
    CHECK(g_sleep_prep_result_generation==g_sleep_prep_generation);
    CHECK(g_sleep_prep_result==(prep_success?ESP_OK:ESP_FAIL));++prep_signals;
}
int RunSleepPrepLocal(int64_t deadline){
    CHECK(deadline==0);++prep_claims;
    if(prep_success)g_epd_rail_powered=false;
    return prep_success?ESP_OK:ESP_FAIL;
}
int RunShutdownClearLocal(int64_t deadline){++shutdown_claims;return RunSleepPrepLocal(deadline);}
bool ServiceDisplaySleepPrepCommand(bool* prepared=nullptr);
}
@@GLOBAL_DRIVER@@
@@SLEEP_PREP@@
namespace device_ui_internal {
constexpr char kTag[]="host-owner";
enum class RefreshSchedule{kNone,kSelection};
RefreshSchedule g_refresh_schedule=RefreshSchedule::kNone;
bool g_refresh_pending=false,g_refresh_busy=false;
struct {bool pending=false;}g_secondary;
TickType_t g_refresh_due_tick=0;
int lock_depth=0,release_calls=0;
void* g_refresh_mutex=nullptr;
void xSemaphoreTake(void*,TickType_t){++lock_depth;}
void xSemaphoreGive(void*){CHECK(lock_depth==1);--lock_depth;}
void MaybeReleaseDisplayLeaseLocked(){CHECK(lock_depth==1);++release_calls;}
@@TICKS@@
struct Event {int64_t at;std::function<void()> apply;};
std::vector<Event> events;
std::vector<TickType_t> waits;
int ulTaskNotifyTake(int clear,TickType_t wait){
    CHECK(clear==pdTRUE);CHECK(lock_depth==0);CHECK(wait>0);
    CHECK(live_leases==0);CHECK(wdt_balance==0);waits.push_back(wait);
    CHECK(waits.size()<200); // An accidental zero/short busy loop must fail.
    const int64_t target=wait==portMAX_DELAY?INT64_MAX:now_ms+(static_cast<int64_t>(wait)*1000+configTICK_RATE_HZ-1)/configTICK_RATE_HZ;
    if(!events.empty()&&events.front().at<=target){
        auto event=std::move(events.front());events.erase(events.begin());CHECK(event.at>=now_ms);now_ms=event.at;event.apply();return 1;
    }
    CHECK(target!=INT64_MAX);now_ms=target;return 0;
}
@@SCHEDULER@@
@@BASELINE@@
}
using namespace wqn;
using namespace device_ui_internal;
void Reset(){
    now_ms=100;off_ms=5000;cleanup_ms=45000;
    g_initialized=true;g_framebuffer=&byte;g_epd_rail_powered=true;
    g_partial_refreshes_since_full=g_heavy_partials_since_full=0;
    g_epd_idle_cut=false;g_epd_cleanup_deferred_logged=false;g_epd_activity_generation=0;
    NoteEpdActivity();g_refresh_pending=g_refresh_busy=g_secondary.pending=false;
    g_sleep_prep_state=kSleepPrepIdle;g_sleep_prep_clear=false;g_sleep_prep_result_generation=0;prep_success=true;prep_claims=prep_signals=shutdown_claims=0;lease_allowed=operation_allowed=true;on_lease={};
    off_result=cleanup_result=ESP_OK;cuts.clear();cleans.clear();events.clear();waits.clear();
    CHECK(live_leases==0&&wdt_balance==0&&lock_depth==0);
    lease_admissions=wdt_entries=0;
}
void Frame(){g_refresh_pending=true;g_refresh_due_tick=xTaskGetTickCount();}
void At(int64_t at,std::function<void()> callback){events.push_back({at,std::move(callback)});}
void ExpectDelay(int64_t wanted){CHECK(EpdIdleMaintenanceDelayMs()==wanted);}
void TestSleepPrepMailbox(){
    Reset();bool prepared=true;
    CHECK(!ServiceDisplaySleepPrepCommand(&prepared));CHECK(!prepared);CHECK(prep_signals==0);
    g_sleep_prep_state=kSleepPrepPending;
    CHECK(ServiceDisplaySleepPrepCommand(&prepared));CHECK(prepared);
    CHECK(g_sleep_prep_state==kSleepPrepIdle);CHECK(prep_signals==1&&prep_claims==1);
    prepared=true;CHECK(!ServiceDisplaySleepPrepCommand(&prepared));CHECK(!prepared);CHECK(prep_signals==1);
    prep_success=false;g_sleep_prep_state=kSleepPrepPending;g_sleep_prep_clear=true;
    CHECK(ServiceDisplaySleepPrepCommand(&prepared));CHECK(!prepared);
    CHECK(g_sleep_prep_state==kSleepPrepIdle);CHECK(!g_sleep_prep_clear);CHECK(shutdown_claims==1);
    CHECK(g_sleep_prep_result==ESP_FAIL);CHECK(prep_signals==2);
}
void TestDeadlines(){
    Reset();ExpectDelay(5000);now_ms=5099;ExpectDelay(1);now_ms=5100;ExpectDelay(0);
    g_epd_idle_cut=true;g_epd_rail_powered=false;ExpectDelay(-1);
    g_heavy_partials_since_full=kIdleCleanupHeavyPartials;ExpectDelay(40000);
    now_ms=45100;ExpectDelay(0);g_heavy_partials_since_full=0;ExpectDelay(-1);
    g_partial_refreshes_since_full=kIdleCleanupFramePartials;ExpectDelay(0);
    cleanup_ms=0;now_ms=5100;ExpectDelay(0);off_ms=0;ExpectDelay(-1);
    cleanup_ms=45000;ExpectDelay(40000); // cleanup independent of rail-cut option
    g_framebuffer=nullptr;ExpectDelay(-1);g_framebuffer=&byte;
    g_initialized=false;ExpectDelay(-1);g_initialized=true;
    g_last_epd_activity_ms=0;ExpectDelay(-1);
    Reset();cleanup_ms=2000;g_heavy_partials_since_full=kIdleCleanupHeavyPartials;ExpectDelay(2000);
    Reset();g_epd_rail_powered=false;ExpectDelay(-1); // full refresh already cut it
    Reset();now_ms=static_cast<int64_t>(UINT32_MAX)*1000/configTICK_RATE_HZ+100;NoteEpdActivity();ExpectDelay(5000);
    EpdIdleSchedule idle;CHECK(EpdIdleWaitTicks(idle)==5*configTICK_RATE_HZ);
    g_last_epd_activity_ms=now_ms+static_cast<int64_t>(UINT32_MAX)*1000;CHECK(EpdIdleWaitTicks(idle)==portMAX_DELAY-1);
}
void TestNoUiPolling(){
    Reset();g_heavy_partials_since_full=kIdleCleanupHeavyPartials;
    At(60100,Frame);EpdIdleSchedule idle;WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(cuts==std::vector<int64_t>({5100}));CHECK(cleans==std::vector<int64_t>({45100}));
    CHECK(waits.size()==3);CHECK(waits.back()==portMAX_DELAY);CHECK(lease_admissions==2);
    Reset();At(60100,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(cuts.size()==1&&cleans.empty());CHECK(waits.size()==2&&waits.back()==portMAX_DELAY);
    Reset();g_epd_rail_powered=false;At(60100,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(waits==std::vector<TickType_t>({portMAX_DELAY}));CHECK(lease_admissions==0);
    Reset();cleanup_ms=0;g_heavy_partials_since_full=kIdleCleanupHeavyPartials;
    At(6100,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cleans==std::vector<int64_t>({5100}));
    Reset();off_ms=0;g_heavy_partials_since_full=kIdleCleanupHeavyPartials;
    At(46100,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts.empty());CHECK(cleans==std::vector<int64_t>({45100}));
}
void TestActivityAndFrames(){
    Reset();At(5000,[]{NoteEpdActivity();});At(11000,Frame);
    EpdIdleSchedule idle;WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts==std::vector<int64_t>({10000}));
    Reset();At(5100,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts.empty());
    Reset();now_ms=5100;g_refresh_pending=true;g_refresh_due_tick=xTaskGetTickCount()+100;
    idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts.empty());CHECK(waits.size()==1);CHECK(waits[0]==100);
    Reset();now_ms=5100;g_sleep_prep_state=kSleepPrepPending;Frame();idle={};
    WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(prep_claims==0);CHECK(cuts.empty());
    Reset();now_ms=5100;const auto old=GetEpdActivityGeneration();NoteEpdActivity();RunIdleMaintenanceIfStillValid(old);CHECK(cuts.empty());
    now_ms=10100;g_secondary.pending=true;RunIdleMaintenanceIfStillValid(GetEpdActivityGeneration());CHECK(cuts.empty());
    g_secondary.pending=false;g_refresh_busy=true;RunIdleMaintenanceIfStillValid(GetEpdActivityGeneration());CHECK(cuts.empty());
    g_refresh_busy=false;g_refresh_pending=true;RunIdleMaintenanceIfStillValid(GetEpdActivityGeneration());CHECK(cuts.empty());
    g_refresh_pending=false;on_lease=[]{NoteEpdActivity();};
    RunIdleMaintenanceIfStillValid(GetEpdActivityGeneration());CHECK(cuts.empty());CHECK(live_leases==0);
}
void TestRetryAndSleep(){
    Reset();off_result=ESP_FAIL;At(6200,Frame);EpdIdleSchedule idle;WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(cuts.empty());CHECK(lease_admissions==3);CHECK(waits.size()==4);
    CHECK(waits[1]==configTICK_RATE_HZ/2&&waits[2]==configTICK_RATE_HZ/2);
    Reset();off_result=ESP_FAIL;At(5200,[]{});At(5250,[]{});At(6200,Frame);
    idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(lease_admissions==3);
    // Notifications do not restart or bypass the absolute retry delay.
    Reset();lease_allowed=false;At(6200,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(cuts.empty()&&lease_admissions==0&&waits.size()==4);
    Reset();operation_allowed=false;At(5700,[]{operation_allowed=true;});At(6300,Frame);
    idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts==std::vector<int64_t>({6100}));
    Reset();off_result=ESP_FAIL;At(5250,[]{NoteEpdActivity();off_result=ESP_OK;});At(11000,Frame);
    idle={};WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts==std::vector<int64_t>({10250}));
    // Sleep prep wins when both deadlines are due, then even spurious wakes
    // cannot restart a cleanup until an accepted frame requests re-init.
    Reset();now_ms=45100;g_heavy_partials_since_full=kIdleCleanupHeavyPartials;g_sleep_prep_state=kSleepPrepPending;
    At(46100,[]{});At(47000,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(prep_claims==1);CHECK(cleans.empty()&&cuts.empty()&&lease_admissions==0);
    CHECK(waits==std::vector<TickType_t>({portMAX_DELAY,portMAX_DELAY}));CHECK(!idle.sleep_prepared);
    g_refresh_pending=false;g_epd_rail_powered=true;NoteEpdActivity();At(53000,Frame);
    WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(cuts==std::vector<int64_t>({52000}));
    Reset();now_ms=5100;g_sleep_prep_state=kSleepPrepPending;prep_success=false;lease_allowed=false;
    At(5700,[]{lease_allowed=true;});At(6300,Frame);idle={};
    WaitForEpdFrameOrIdleMaintenance(&idle);CHECK(prep_claims==1);CHECK(cuts==std::vector<int64_t>({6100}));
    Reset();cleanup_result=ESP_FAIL;g_heavy_partials_since_full=kIdleCleanupHeavyPartials;
    At(46200,Frame);idle={};WaitForEpdFrameOrIdleMaintenance(&idle);
    CHECK(cuts==std::vector<int64_t>({5100}));CHECK(cleans==std::vector<int64_t>({45100,45600,46100}));
}
int main(){TestSleepPrepMailbox();TestDeadlines();TestNoUiPolling();TestActivityAndFrames();TestRetryAndSleep();
@@BASELINE_TEST@@
    CHECK(live_leases==0&&wdt_balance==0&&lock_depth==0);
    std::printf("EPD idle scheduler: %d checks PASS at %d Hz (production virtual-time ports, NOT HIL)\n",checks,configTICK_RATE_HZ);
}
