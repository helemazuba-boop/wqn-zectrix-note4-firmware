#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "button_event.h"
#include "display/display_types.h"
#include "display_recovery.h"

using TickType_t=uint32_t;
using UBaseType_t=unsigned;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1,pdFALSE=0;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>((ms)*configTICK_RATE_HZ/1000))
#define portTICK_PERIOD_MS (1000/configTICK_RATE_HZ)
#define CONFIG_ESP_TASK_WDT_EN 1
const char* esp_err_to_name(int) { return "host"; }
template<class... A> void HostLog(A...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
#define ESP_LOGD(...) HostLog(__VA_ARGS__)
int64_t now_ms=100;
int64_t esp_timer_get_time() { return now_ms*1000; }
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(now_ms*configTICK_RATE_HZ/1000); }
void* xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(1); }
int lock_depth=0,wdt_balance=0,live_leases=0,lease_admissions=0,error_records=0;
void xSemaphoreTake(void*,TickType_t) { if (lock_depth++!=0) std::abort(); }
void xSemaphoreGive(void*) { if (--lock_depth!=0) std::abort(); }
int esp_task_wdt_add(void*) { ++wdt_balance; return ESP_OK; }
void esp_task_wdt_delete(void*) { --wdt_balance; }
bool lease_allowed=true;
namespace wqn {
enum class UiScreen { kHome,kAi,kNote };
struct UiFrame { UiScreen screen=UiScreen::kHome; struct { bool visible=false; } navigation; bool prefer_full_refresh=false; int content=0; };
struct AppState { uint64_t revision=1; };
bool g_force_full_refresh_next=false;
@@GLOBAL_FORCE@@
void NoteEpdActivity() {}
uint32_t GetEpdActivityGeneration() { return 1; }
int64_t EpdIdleMaintenanceDelayMs() { return -1; }
bool ServiceDisplaySleepPrepCommand(bool*) { return false; }
void RegisterEpdOwnerTask(void*) {}
void RecordError(const char*,const char*,...) { ++error_records; }
namespace runtime {
enum class SleepBlocker { kDisplay };
struct SleepLease {
    bool held=false;
    SleepLease()=default;
    SleepLease(const SleepLease&)=delete;
    SleepLease(SleepLease&& other):held(other.held) { other.held=false; }
    SleepLease& operator=(SleepLease&& other) { Reset(); held=other.held;other.held=false;return *this; }
    ~SleepLease() { Reset(); }
    void Reset() { if (held) { --live_leases;held=false; } }
    explicit operator bool() const { return held; }
    static SleepLease TryAcquire(SleepBlocker,const char*,const char*,int) {
        SleepLease lease;if (lease_allowed) {lease.held=true;++live_leases;++lease_admissions;}return lease;
    }
};
}
}
namespace device_ui_internal {
constexpr char kTag[]="display-test";
@@SCHEDULE@@
@@DELAYS@@
@@REFRESH_HELPERS@@
@@EVENT@@
@@UPDATE@@
const char* AppEventKindName(AppEventKind) { return "host"; }
class UiRuntime {
public:
    wqn::AppState state_;
    uint64_t event_sequence_=0;
    uint64_t revision() const { return state_.revision; }
    uint64_t NextRevision();
    UiUpdate FinishEvent(AppEventKind,RefreshSchedule,bool,bool=false);
    UiUpdate DispatchDisplayResult(const wqn::display::DisplayResult&);
    UiUpdate DispatchDisplayRecovery();
};
@@RUNTIME@@
using namespace wqn::display;
@@SECONDARY@@
void* g_refresh_mutex=reinterpret_cast<void*>(1);
void* g_refresh_task=reinterpret_cast<void*>(2);
void* g_display_result_queue=reinterpret_cast<void*>(3);
wqn::UiFrame g_pending_frames[2];
std::string g_pending_signatures[2];
DisplayIntent g_pending_intents[2];
std::atomic<int> g_consumer_index{0};
bool g_refresh_pending=false,g_refresh_busy=false;
TickType_t g_refresh_due_tick=0;
RefreshSchedule g_refresh_schedule=RefreshSchedule::kNone;
SecondarySlot g_secondary;
wqn::UiScreen g_last_rendered_screen=wqn::UiScreen::kHome;
bool g_last_rendered_navigation_visible=false;
DisplayRevision g_last_presented_revision=0;
uint32_t g_outstanding_display_intents=0;
std::array<DisplayRevision,kDisplayResultQueueDepth> g_outstanding_display_revisions{};
wqn::runtime::SleepLease g_display_sleep_lease;
DisplayRecoveryPolicy g_display_recovery;
std::deque<DisplayResult> results;
std::vector<DisplayResult> all_results;
int xQueueSend(void*,const DisplayResult* result,TickType_t) { results.push_back(*result);all_results.push_back(*result);return pdTRUE; }
int xQueueReceive(void*,DisplayResult* result,int) { if(results.empty())return pdFALSE;*result=results.front();results.pop_front();return pdTRUE; }
void xTaskNotifyGive(void*) {}
void taskYIELD() {}
@@LOWER@@
unsigned uxTaskGetStackHighWaterMark(void*) { return 1000; }
using StackType_t=uint32_t;
void RunIdleMaintenanceIfStillValid(uint32_t) {}
int ulTaskNotifyTake(int,TickType_t);
int RenderFrameToEpd(const wqn::UiFrame&,RefreshSchedule);
@@WAIT@@
@@OWNER@@
}
using namespace device_ui_internal;
@@TRACKING_TYPES@@
void RecordPresentedRefresh(wqn::UiScreen,const char*,const std::string&) {}
@@UPPER@@
@@GATE@@
@@WAVEFORM@@

UiRuntime ui_runtime;
DisplayTrackingState tracking;
RefreshSchedule ui_pending=RefreshSchedule::kNone;
int latest_content=1;
size_t request_calls=0,checks=0,failures=0,idle_parks=0,wait_steps=0;
bool observing_limit=false,force_fail=true,render_hook_once=false;
std::vector<int64_t> render_times;
std::vector<RefreshSchedule> render_schedules;
std::vector<int> rendered_content;
std::function<void()> render_hook;
struct Action { int64_t at;std::function<void()> run; };
std::deque<Action> actions;
void Check(bool pass,const char* requirement)
{
    ++checks;if(!pass){if(failures<20)std::fprintf(stderr,"FAIL %s\n",requirement);++failures;}
}
void Submit(RefreshSchedule schedule=RefreshSchedule::kImmediate,bool track=true)
{
    ++request_calls;
    const bool consumed=wqn::ConsumeForceFullRefresh();
    const bool full=tracking.force_next_submission||consumed;
    wqn::UiFrame frame;frame.content=latest_content;frame.prefer_full_refresh=full;
    const std::string signature=std::to_string(latest_content);
    const auto submission=RequestEpdUiRefresh(frame,signature,ui_runtime.revision(),schedule,
        RequestedWaveform(frame,schedule,full||schedule==RefreshSchedule::kCommit));
    if(submission.accepted && track) {
        Check(TrackDisplaySubmission(&tracking,submission,signature,wqn::UiScreen::kHome,"12:00",schedule),"accepted revision fits upper tracking ledger");
        tracking.force_next_submission=false;
    } else if(!submission.accepted) ui_pending=StrongerSchedule(ui_pending,schedule);
}
void PumpUi()
{
    RefreshSchedule schedule=ui_pending;ui_pending=RefreshSchedule::kNone;
    DrainDisplayResults(&ui_runtime,&tracking,&schedule);
    if(render_times.size()>=20) { observing_limit=true;return; } // finite red baseline observation
    ApplyDisplayRecoveryGate(&ui_runtime,&tracking,&schedule);
    if(schedule!=RefreshSchedule::kNone) Submit(schedule);
}
void BusinessUpdate()
{
    ++latest_content;const auto update=ui_runtime.FinishEvent(AppEventKind::kClockMinute,RefreshSchedule::kClock,true);
    ui_pending=StrongerSchedule(ui_pending,update.refresh);PumpUi();
}
struct Parked {};
namespace device_ui_internal {
int RenderFrameToEpd(const wqn::UiFrame& frame,RefreshSchedule schedule)
{
    Check(lock_depth==0 && live_leases==1 && wdt_balance==1,"hardware runs outside refresh lock with one lease/watchdog");
    render_times.push_back(now_ms);render_schedules.push_back(schedule);rendered_content.push_back(frame.content);
    if(render_hook) {
        if(render_hook_once) {auto hook=std::move(render_hook);render_hook={};render_hook_once=false;hook();}
        else render_hook();
    }
    return force_fail?ESP_FAIL:ESP_OK;
}
int ulTaskNotifyTake(int,TickType_t owner_wait)
{
    const bool was_pending=g_refresh_pending;
    PumpUi();
    if(g_refresh_pending && (!was_pending || TicksUntil(xTaskGetTickCount(),g_refresh_due_tick)==0)) return pdTRUE;
    if(++wait_steps>2000) { Check(false,"no retry busy loop");throw Parked{}; }
    const TickType_t fallback=ui_pending==RefreshSchedule::kNone?portMAX_DELAY:pdMS_TO_TICKS(50);
    const auto ui_wait=observing_limit?portMAX_DELAY:DisplayRecoveryPollDelay(tracking,fallback);
    const auto wait=std::min(owner_wait,ui_wait);
    const int64_t until=wait==portMAX_DELAY?INT64_MAX:now_ms+(static_cast<int64_t>(wait)*1000+configTICK_RATE_HZ-1)/configTICK_RATE_HZ;
    if(!actions.empty() && actions.front().at<=until) {
        auto action=std::move(actions.front());actions.pop_front();now_ms=std::max(now_ms,action.at);action.run();PumpUi();return pdTRUE;
    }
    if(wait==portMAX_DELAY) {++idle_parks;throw Parked{};}
    if(!g_refresh_busy&&!g_refresh_pending&&!g_secondary.pending) Check(live_leases==0,"backoff waits hold no display lease");
    now_ms=until;PumpUi();return pdFALSE;
}
}
void RunOwner() {try {EpdRefreshTask(nullptr);}catch(const Parked&) {}}
void Reset(int64_t started=100)
{
    g_display_sleep_lease.Reset();Check(live_leases==0 && lock_depth==0 && wdt_balance==0,"previous owner invocation left balanced ports");
    g_display_recovery={};g_refresh_pending=g_refresh_busy=false;g_secondary={};
    g_refresh_schedule=RefreshSchedule::kNone;g_refresh_due_tick=0;g_consumer_index=0;
    g_outstanding_display_intents=0;g_outstanding_display_revisions={};g_last_presented_revision=0;
    results.clear();all_results.clear();tracking={};ui_runtime.state_={};ui_runtime.event_sequence_=0;
    now_ms=started;latest_content=1;ui_pending=RefreshSchedule::kNone;
    request_calls=idle_parks=wait_steps=0;observing_limit=false;force_fail=true;render_hook_once=false;lease_allowed=true;error_records=0;
    render_times.clear();render_schedules.clear();rendered_content.clear();render_hook={};actions.clear();wqn::g_force_full_refresh_next=false;
}
void FailedCycle(bool traffic=false)
{
    Reset();
    if(traffic) render_hook=[] {
        ++latest_content;ui_runtime.NextRevision();Submit(RefreshSchedule::kSelection);
        ++latest_content;ui_runtime.NextRevision();Submit(RefreshSchedule::kSelection);
    };
    Submit();RunOwner();
    std::printf("persistent failure traffic=%d attempts=%zu last_gap_ms=%lld paused=%d observer_limited=%d\n",
        traffic?1:0,render_times.size(),static_cast<long long>(render_times.size()>1?render_times.back()-render_times[render_times.size()-2]:0),
        g_display_recovery.status().paused?1:0,observing_limit?1:0);
    Check(render_times==std::vector<int64_t>({100,1100,6100,21100}),"initial render plus three auto retries obey 1/5/15 second failure backoff");
    Check(!observing_limit && g_display_recovery.status().paused,"persistent failure stops without the observation guard");
    Check(g_display_recovery.status().consecutive_failures==4 && error_records==1,"only physical failures consume budget and record one pause");
    Check(g_outstanding_display_intents==0 && !g_refresh_busy && !g_refresh_pending && !g_secondary.pending && live_leases==0,
          "every accepted revision drains to terminal/ACK and releases display lease");
    Check(ui_pending==RefreshSchedule::kNone && idle_parks>0,"paused UI has no immediate refresh spin and owner parks");
    bool full=true;for(size_t i=1;i<render_schedules.size();++i) full=full&&render_schedules[i]==RefreshSchedule::kCommit;
    Check(full,"recovery always uses the full drawing path even for pre-failure secondary frames");
    if(traffic) Check(all_results.size()>render_times.size(),"superseded/blocked work receives terminal results without extra hardware");
}
void Press(int64_t at,bool repeat=false,wqn::ButtonEventType kind=wqn::ButtonEventType::kPress)
{
    wqn::ButtonEvent event;event.button=wqn::ButtonId::kUp;event.type=kind;event.repeat=repeat;event.occurred_at_ms=at;
    RefreshSchedule schedule=RefreshSchedule::kNone;
    MaybeRecoverDisplayForButton(event,&ui_runtime,&tracking,&schedule);
    ui_pending=StrongerSchedule(ui_pending,schedule);PumpUi();
}
int main()
{
    FailedCycle();FailedCycle(true);
    const auto renders=render_times.size(),requests_before=request_calls;
    const int admissions_before=lease_admissions;
    now_ms+=1000;
    for(int i=0;i<1000;++i) BusinessUpdate();
    Check(render_times.size()==renders && request_calls==requests_before && ui_pending==RefreshSchedule::kNone,
          "automatic clock/content revisions cannot reopen paused hardware or poll a rejected request");
    for(int i=0;i<30;++i) {ui_runtime.NextRevision();Submit();}
    Check(g_outstanding_display_intents==0 && lease_admissions==admissions_before,"direct producer cannot bypass paused admission or acquire a lease");
    ui_pending=RefreshSchedule::kNone;
    const int64_t paused=g_display_recovery.status().paused_at_ms;
    Press(paused);Press(paused-1);Press(now_ms,true);Press(now_ms,false,wqn::ButtonEventType::kLongPress);
    Check(!g_display_recovery.status().probe_pending && request_calls==requests_before+30,"stale presses and held repeats cannot grant recovery");
    render_hook={};Press(now_ms);RunOwner();
    Check(render_times.size()==renders+1 && g_display_recovery.status().paused,"fresh key grants exactly one failed probe with no renewed auto budget");
    Check(error_records==2 && live_leases==0,"failed manual probe records pause and releases resources");
    now_ms+=1;force_fail=false;Press(now_ms);
    Check(g_display_recovery.status().probe_pending && render_times.size()==renders+1,"manual probe respects cooldown without immediate hardware");
    RunOwner();
    Check(render_times.size()==renders+2 && !g_display_recovery.status().RequiresFull(),"successful real full probe clears the failure streak");
    Check(rendered_content.back()==latest_content,"recovery presents newest paused-state pixels");
    Check(live_leases==0 && g_outstanding_display_intents==0 && wdt_balance==0,"successful recovery drains ACK and watchdog ownership");
    const auto recovered=render_times.size();BusinessUpdate();RunOwner();
    Check(render_times.size()==recovered+1,"ordinary presentation resumes after verified recovery");

    FailedCycle();now_ms+=1001;force_fail=false;
    render_hook_once=true;render_hook=[] {BusinessUpdate();};
    Press(now_ms);RunOwner();
    Check(render_times.size()==6 && rendered_content.back()==latest_content,
          "content arriving during a manual probe is presented after that probe succeeds");
    Check(!g_display_recovery.status().RequiresFull() && live_leases==0 && g_outstanding_display_intents==0,
          "latest-state convergence retires all recovery ownership");

    Reset();render_hook=[] {if(render_times.size()==2) force_fail=false;};Submit();RunOwner();
    Check(render_times.size()==2 && !g_display_recovery.status().RequiresFull(),"transient failure recovers at first delayed automatic retry");
    Check(error_records==0,"transient success does not report exhausted recovery");
    Reset((1LL<<40));Submit();RunOwner();
    Check(render_times.size()==4 && render_times[1]-render_times[0]==1000,"monotonic retry deadlines remain 64-bit at long uptime");
    Check(live_leases==0 && g_outstanding_display_intents==0,"long-uptime pause has no leaked admission");

#if HAS_RECOVERY
    DisplayRecoveryPolicy policy;
    int64_t at=100;
    for(int i=0;i<4;++i) { Check(policy.TryBegin(at),"policy permits the scheduled attempt");policy.Failed(at);at=policy.status().retry_at_ms; }
    Check(!policy.RequestProbe(policy.status().paused_at_ms,at),"policy rejects pre-pause input identity");
    Check(!policy.RequestProbe(at+1,at),"future input timestamps cannot authorize a probe");
    for(int i=0;i<300;++i) {at+=1001;Check(policy.RequestProbe(at,at)&&policy.TryBegin(at),"one fresh probe token is consumable");Check(!policy.TryBegin(at),"consumed probe cannot start another attempt");policy.Failed(at);}
    Check(policy.status().consecutive_failures==UINT8_MAX && policy.status().paused,"manual failure counter saturates without reopening automatic retries");
#endif
    std::printf("Display recovery: %zu checks, %zu failures at %d Hz (real owner/UI gates/submit/ACK; render/RTOS ports; NOT HIL)\n",checks,failures,configTICK_RATE_HZ);
    return failures==0?0:1;
}
