// Whole button producer plus real stale/PTT/navigation/activity/sleep decisions.
// GPIO, task scheduling, audio commands and retained scalars are host ports.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "button_input.cpp"
#include "ui/navigation_model.h"

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d (%d Hz): %s\n",__LINE__,configTICK_RATE_HZ,#x); std::abort(); } } while (0)
static int64_t clock_us = 1000000;
static std::array<int,40> levels;
static std::array<bool,40> enabled;
static std::array<void(*)(void*),40> handlers;
static std::array<void*,40> handler_args;
static int button_handle, ui_handle, power_handle;
static unsigned notifications = 0, ui_notifications = 0, power_notifications = 0, reads = 0;
static unsigned reenables = 0;
static bool stepping = false;
static std::function<void()> before_ring, during_delay, during_enable;
struct StepFinished {};
struct Parked {};
struct SleepEntered {};
int64_t esp_timer_get_time() { return clock_us; }
void FixtureLog(const char*, const char*, ...) {}
void FixtureEnter(portMUX_TYPE* lock) {
    if (lock == &g_ring_lock && before_ring) {
        auto hook = std::move(before_ring); before_ring = nullptr; hook();
    }
    CHECK(lock->depth == 0); ++lock->depth;
}
void FixtureExit(portMUX_TYPE* lock) { CHECK(lock->depth == 1); --lock->depth; }
int gpio_config(const gpio_config_t* config) {
    CHECK(config->pin_bit_mask == ((1ULL<<39)|(1ULL<<18)|1));
    CHECK(config->intr_type == GPIO_INTR_LOW_LEVEL); enabled.fill(true); return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { ++reads; return levels.at(pin); }
int gpio_intr_disable(gpio_num_t pin) { enabled.at(pin) = false; return ESP_OK; }
int gpio_intr_enable(gpio_num_t pin) {
    ++reenables; enabled.at(pin) = true;
    if (during_enable) { auto hook=std::move(during_enable); during_enable=nullptr; hook(); }
    if (!levels.at(pin) && handlers.at(pin)) handlers.at(pin)(handler_args.at(pin));
    return ESP_OK;
}
int gpio_install_isr_service(int) { return ESP_OK; }
int gpio_isr_handler_add(gpio_num_t pin, void(*handler)(void*), void* arg) {
    handlers.at(pin)=handler; handler_args.at(pin)=arg; return ESP_OK;
}
int gpio_wakeup_enable(gpio_num_t, int mode) { CHECK(mode==GPIO_INTR_LOW_LEVEL); return ESP_OK; }
int esp_sleep_enable_gpio_wakeup() { return ESP_OK; }
void xTaskNotifyGive(TaskHandle_t task) {
    if (task==&ui_handle) ++ui_notifications;
    else if (task==&power_handle) ++power_notifications;
    else { CHECK(task==&button_handle); ++notifications; }
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t*) { xTaskNotifyGive(task); }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    CHECK(clear==pdTRUE && wait==portMAX_DELAY);
    if (!notifications) throw Parked{};
    const auto count=notifications; notifications=0; return count;
}
void vTaskDelay(TickType_t ticks) {
    CHECK(ticks>0 && ticks<portMAX_DELAY);
    constexpr int64_t tick_us=1000000/configTICK_RATE_HZ;
    clock_us=(clock_us/tick_us+ticks)*tick_us;
    if (during_delay) { auto hook=std::move(during_delay); during_delay=nullptr; hook(); }
    if (stepping) throw StepFinished{};
}
BaseType_t xTaskCreate(void(*task)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t* out) {
    CHECK(task==ButtonInputTask); *out=&button_handle; return pdPASS;
}
unsigned uxTaskGetStackHighWaterMark(void*) { return 1024; }

static std::atomic<bool> g_user_interacted_current_boot{false}, g_deep_sleep_clock_yield{false};
static std::atomic<uint32_t> g_user_activity_generation{0};
static portMUX_TYPE g_activity_gate;
static uint32_t retained_cycles=0, g_unattended_sync_wakes=0;
static int64_t activity_ms=0;
static TaskHandle_t g_power_coordinator_task=&power_handle;
static auto UserActivityMsRef() { return std::atomic_ref<int64_t>(activity_ms); }
static auto ConsecutiveSleepCyclesRef() { return std::atomic_ref<uint32_t>(retained_cycles); }
namespace wqn {
@@ACTIVITY@@
}
namespace wqn::power {
struct PrepareSleepCommand { uint32_t generation=1; int mode=0; };
static const char* SleepModeName(int) { return "port"; }
}
namespace wqn::runtime {
enum class SleepDiagnosticEventKind { kCommit };
struct SleepDiagnosticEvent { uint32_t generation=0; uint8_t sleep_mode=0; int battery_mv=0; };
static void RecordSleepDiagnosticEvent(const SleepDiagnosticEvent&) {}
}
static auto MakeSleepDiagnosticEvent(wqn::runtime::SleepDiagnosticEventKind) { return wqn::runtime::SleepDiagnosticEvent{}; }
namespace wqn {
uint16_t GetBatteryVoltageMv() { return 3900; }
static unsigned GetWifiRadioOnTotalMs() { return 0; }
static bool usb_power=false;
bool IsUsbPowered() { return usb_power; }
static std::atomic<DeepSleepUiPolicy> g_deep_sleep_ui_policy{DeepSleepUiPolicy::kDeepSleepNoDisplayTimer};
static bool DeepSleepAllowedByUiPolicy(DeepSleepUiPolicy policy) { return policy==DeepSleepUiPolicy::kDeepSleepNoDisplayTimer; }
}
using uart_port_t=int;
static void uart_wait_tx_idle_polling(uart_port_t) {}
[[noreturn]] static void esp_deep_sleep_start() { throw SleepEntered{}; }
@@COMMIT@@

namespace wqn {
@@UI_TYPES@@
struct AgentSessionState { bool recording=false; };
struct UiState {
    UiScreen screen=UiScreen::kAi;
    struct { AiTier tier=AiTier::kFlash; } ai;
    UiGestureState gestures;
    AgentSessionState agent;
};
static unsigned starts=0, stops=0;
static bool recording=false;
void OnFlashButtonPressed() { ++starts; recording=true; }
void OnFlashButtonReleased(bool submit) { CHECK(submit && recording); ++stops; recording=false; }
esp_err_t StartOpenCodeVoiceInput() { ++starts; recording=true; return ESP_OK; }
esp_err_t StopOpenCodeVoiceInput() { CHECK(recording); ++stops; recording=false; return ESP_OK; }
bool CopyOpenCodeSessionToUi(AgentSessionState* out) { out->recording=recording; return true; }
}
enum class RefreshSchedule { kNone, kAi };
@@STALE@@
static RefreshSchedule ApplyPtt(const wqn::ButtonEvent& event, wqn::UiState* state) {
    const bool is_agent_ptt=state->ai.tier==wqn::AiTier::kAgent && event.button==wqn::ButtonId::kConfirm;
@@AGENT@@
@@FLASH@@
    return RefreshSchedule::kNone;
}

using Id=wqn::ButtonId;
using Type=wqn::ButtonEventType;
static void Reset(int initially_held=-1) {
    CHECK(g_ring_lock.depth==0 && g_activity_gate.depth==0);
    for (auto& button:g_buttons) {
        const auto id=button.id; const auto pin=button.pin;
        std::destroy_at(&button); std::construct_at(&button, id, pin);
    }
    g_ring_head=g_ring_tail=g_ring_count=0; g_event_seq=0;
    g_dropped_critical=0; g_coalesced_repeat=0; g_ring_high_water=0;
    g_last_short_release_at_ms=0; g_last_short_release_button=Id::kNone;
    g_button_task=nullptr; g_ui_task_to_notify=nullptr;
    clock_us=1000000; levels.fill(1); enabled.fill(false); handlers.fill(nullptr);
    if (initially_held>=0) levels.at(initially_held)=0;
    notifications=ui_notifications=power_notifications=reads=reenables=0;
    before_ring=during_delay=during_enable=nullptr; stepping=false;
    g_user_activity_generation=0; activity_ms=0; retained_cycles=3; g_unattended_sync_wakes=5;
    g_user_interacted_current_boot=false; g_deep_sleep_clock_yield=true;
    wqn::usb_power=false; wqn::g_deep_sleep_ui_policy=wqn::DeepSleepUiPolicy::kDeepSleepNoDisplayTimer;
    wqn::starts=wqn::stops=0; wqn::recording=false;
    CHECK(wqn::InitButtonInput()==ESP_OK);
    CHECK(wqn::StartButtonInputTask(&ui_handle)==ESP_OK);
}
static void Set(int pin,bool pressed) {
    levels.at(pin)=pressed ? 0 : 1;
    if (pressed && enabled.at(pin) && handlers.at(pin)) handlers.at(pin)(handler_args.at(pin));
}
static bool Step() {
    stepping=true;
    try { ButtonInputTask(nullptr); }
    catch (const StepFinished&) { stepping=false; return true; }
    catch (const Parked&) { stepping=false; return false; }
    std::abort();
}
static void RunFor(int64_t duration_ms) {
    const int64_t end=clock_us+duration_ms*1000;
    while (clock_us<end) if (!Step()) { clock_us=end; break; }
}
static std::vector<wqn::ButtonEvent> Drain() {
    std::vector<wqn::ButtonEvent> events; wqn::ButtonEvent event;
    while (wqn::ReceiveButtonEvent(&event)) events.push_back(event);
    CHECK(!wqn::ReceiveButtonEvent(nullptr)); return events;
}
static void CheckOrder(const std::vector<wqn::ButtonEvent>& events) {
    for (size_t i=0;i<events.size();++i) {
        CHECK(events[i].occurred_at_ms>0 && events[i].seq>0);
        if (i) { CHECK(events[i].seq>events[i-1].seq); CHECK(events[i].occurred_at_ms>=events[i-1].occurred_at_ms); }
    }
}
static void Tap(int pin,int64_t hold=100,int64_t gap=100) { Set(pin,true); RunFor(hold); Set(pin,false); RunFor(gap); }
static void Gestures() {
    Reset(); Tap(39); auto events=Drain(); CHECK(events.size()==3);
    CHECK(events[0].type==Type::kPress && events[1].type==Type::kShortPress && events[2].type==Type::kRelease); CheckOrder(events);
    Reset(); for (int i=0;i<3;++i) { Set(0,true); RunFor(10); Set(0,false); RunFor(10); }
    RunFor(400); CHECK(Drain().empty()); CHECK(!AnyButtonWindowActive()); CHECK(Step()==false);
    const auto old_reads=reads; RunFor(60000); CHECK(reads==old_reads); // no idle polling
    Reset(); Tap(0,100,60); Tap(0,100,80); events=Drain(); CHECK(events.size()==6);
    CHECK(events[1].type==Type::kShortPress && events[4].type==Type::kDoublePress); CheckOrder(events);
    Reset(); Tap(18,300,100); events=Drain(); CHECK(events.size()==4);
    CHECK(events[0].type==Type::kPress && events[1].type==Type::kHoldPress && events[2].type==Type::kShortPress && events[3].type==Type::kRelease);
    Reset(); Tap(0,2000,400); events=Drain(); CheckOrder(events);
    unsigned firsts=0,repeats=0,releases=0;
    for (const auto& e:events) {
        if (e.type==Type::kLongPress) { firsts+=!e.repeat; repeats+=e.repeat; CHECK(e.duration_ms>=650); }
        releases+=e.type==Type::kRelease;
    }
    CHECK(firsts==1 && repeats>=1 && releases==1);
    CHECK(events[events.size()-2].type==Type::kLongRelease && events.back().type==Type::kRelease);
    Reset(); Set(39,true); Set(18,true); RunFor(150); Set(39,false); Set(18,false); RunFor(150); events=Drain();
    CHECK(events.size()==6); CheckOrder(events);
    for (Id id:{Id::kUp,Id::kDownPower}) {
        CHECK(std::count_if(events.begin(),events.end(),[&](const auto& e){return e.button==id && e.type==Type::kPress;})==1);
        CHECK(std::count_if(events.begin(),events.end(),[&](const auto& e){return e.button==id && e.type==Type::kRelease;})==1);
    }
    // Exact classification thresholds, independently of RTOS tick quantization.
    Reset(); auto& b=g_buttons[0]; levels[39]=0; b.raw_pressed=b.stable_pressed=true; b.stable_changed_at_ms=1000;
    clock_us=1199000; CHECK(!ClassifyButtonsOnce().HasEvent());
    clock_us=1200000; CHECK(ClassifyButtonsOnce().type==Type::kHoldPress);
    clock_us=1649000; CHECK(!ClassifyButtonsOnce().HasEvent());
    clock_us=1650000; auto e=ClassifyButtonsOnce(); CHECK(e.type==Type::kLongPress && !e.repeat);
    clock_us=1909000; CHECK(!ClassifyButtonsOnce().HasEvent());
    clock_us=1910000; e=ClassifyButtonsOnce(); CHECK(e.type==Type::kLongPress && e.repeat);
    // Double press uses release-to-release production time, including its boundary.
    for (int64_t interval:{300,301}) {
        Reset(); auto& key=g_buttons[0]; key.stable_pressed=true; key.raw_pressed=false;
        key.raw_changed_at_ms=1000; key.stable_changed_at_ms=900;
        g_last_short_release_button=Id::kUp; g_last_short_release_at_ms=1040-interval;
        clock_us=1040000; e=ClassifyButtonsOnce();
        CHECK(e.type==(interval==300 ? Type::kDoublePress : Type::kShortPress));
    }
    Reset(0); RunFor(1000); Set(0,false); RunFor(400); events=Drain();
    CHECK(std::count_if(events.begin(),events.end(),[](const auto& event){return event.type==Type::kHoldPress;})==1);
    CHECK(events.back().type==Type::kRelease); // a held boot key does not park forever
}
static void Capacity() {
    Reset(); for (int i=0;i<20;++i) Tap(0,300,150);
    auto events=Drain();
    std::fprintf(stderr,"20 complete short holds: events=%zu dropped=%u peak=%u\n",events.size(),wqn::ButtonEventsDroppedCritical(),wqn::ButtonEventRingHighWater());
    CHECK(events.size()==80); CHECK(wqn::ButtonEventsDroppedCritical()==0); CheckOrder(events);
}
static auto Event(Type type,bool repeat=false,int64_t label=0) {
    wqn::ButtonEvent e; e.button=Id::kUp; e.type=type; e.repeat=repeat; e.occurred_at_ms=clock_us/1000; e.duration_ms=label; return e;
}
static void Ring() {
    Reset(); PushButtonEvent(Event(Type::kLongPress)); const auto gen=g_user_activity_generation.load();
    for (int i=0;i<100;++i) PushButtonEvent(Event(Type::kLongPress,true,i));
    auto events=Drain(); CHECK(events.size()==2 && events.back().duration_ms==99);
    CHECK(wqn::ButtonEventsCoalescedRepeat()==99 && g_user_activity_generation.load()==gen);
    Reset(); // wrap both head and tail before an eviction in the middle
    for (size_t i=0;i<kEventRingCapacity-5;++i) PushButtonEvent(Event(Type::kPress));
    Drain();
    for (size_t i=0;i<kEventRingCapacity;++i) PushButtonEvent(Event(Type::kLongPress,i==3,i));
    PushButtonEvent(Event(Type::kRelease,false,999)); events=Drain();
    CHECK(events.size()==kEventRingCapacity && events.back().duration_ms==999);
    CHECK(wqn::ButtonEventsCoalescedRepeat()==1 && wqn::ButtonEventsDroppedCritical()==0);
    for (const auto& e:events) CHECK(e.duration_ms!=3);
    Reset(); for (size_t i=0;i<kEventRingCapacity;++i) PushButtonEvent(Event(Type::kPress));
    const auto before=g_user_activity_generation.load(); PushButtonEvent(Event(Type::kLongPress,true));
    CHECK(g_user_activity_generation.load()==before && wqn::ButtonEventsCoalescedRepeat()==1);
    PushButtonEvent(Event(Type::kRelease)); CHECK(wqn::ButtonEventsDroppedCritical()==1);
    CHECK(g_user_activity_generation.load()==before+1); // dropped input can only over-cancel sleep
    CHECK(Drain().size()==kEventRingCapacity); Tap(39); CHECK(Drain().size()==3);
}
static void ConsumePtt(wqn::UiState* state,wqn::NavigationState* nav=nullptr) {
    const auto generation=g_user_activity_generation.load();
    for (const auto& e:Drain()) {
        if (ShouldDropStaleButtonEvent(e,clock_us/1000)) continue;
        if (nav) wqn::ReduceNavigationInput(nav,state->screen,e,e.occurred_at_ms,false);
        ApplyPtt(e,state);
    }
    CHECK(g_user_activity_generation.load()==generation); // consume cannot re-publish activity
}
static void Stale() {
    for (auto tier:{wqn::AiTier::kFlash,wqn::AiTier::kAgent}) {
        Reset(); wqn::UiState state; state.ai.tier=tier;
        Set(0,true); RunFor(300); ConsumePtt(&state); CHECK(wqn::recording && wqn::starts==1);
        Set(0,false); RunFor(3100); ConsumePtt(&state);
        CHECK(!wqn::recording && wqn::stops==1);
        Reset(); state={}; state.ai.tier=tier;
        Tap(0,300,3100); ConsumePtt(&state);
        CHECK(wqn::starts==0 && wqn::stops==0 && !wqn::recording);
    }
    Reset(); wqn::UiState state; wqn::NavigationState nav;
    Set(0,true); RunFor(100); ConsumePtt(&state,&nav);
    const auto bit=1U<<static_cast<unsigned>(Id::kConfirm); CHECK(nav.held_mask==bit);
    nav.blocked_mask=bit; Set(0,false); RunFor(3100); ConsumePtt(&state,&nav);
    CHECK(nav.held_mask==0 && nav.blocked_mask==0);
    auto e=Event(Type::kLongPress,true); e.occurred_at_ms=1000;
    CHECK(!ShouldDropStaleButtonEvent(e,1500) && ShouldDropStaleButtonEvent(e,1501));
    e.button=Id::kConfirm; e.type=Type::kHoldPress;
    CHECK(!ShouldDropStaleButtonEvent(e,3000) && ShouldDropStaleButtonEvent(e,3001));
    e.type=Type::kRelease; CHECK(!ShouldDropStaleButtonEvent(e,9000));
}
static bool TrySleep(uint32_t baseline) {
    const wqn::power::PrepareSleepCommand command;
    try { CHECK(wqn::CommitDeepSleep(command,&baseline)==DeepSleepCommitAbortReason::kUserActivity); return false; }
    catch (const SleepEntered&) { g_activity_gate.depth=0; return true; }
}
static void Sleep() {
    Reset(); CHECK(TrySleep(0)); // no-input control enters the port
    Reset(); const uint32_t baseline=g_user_activity_generation.load(); Tap(39);
    CHECK(g_user_activity_generation.load()>baseline); CHECK(!TrySleep(baseline));
    CHECK(activity_ms<clock_us/1000 && retained_cycles==0 && g_unattended_sync_wakes==0);
    Reset(); bool checked=false;
    before_ring=[&] { CHECK(g_ring_count==0); CHECK(!TrySleep(0)); checked=true; };
    PushButtonEvent(Event(Type::kPress)); CHECK(checked && g_ring_count==1);
    Reset(); // input lands after UART flush while the actual commit is settling
    during_delay=[] { PushButtonEvent(Event(Type::kRelease)); };
    CHECK(!TrySleep(0)); CHECK(g_ring_count==1 && g_activity_gate.depth==0);
    Reset(); clock_us=5000000; auto e=Event(Type::kShortPress); e.occurred_at_ms=1234;
    PushButtonEvent(e); CHECK(activity_ms==1234 && power_notifications==1);
}
static void Irq() {
    Reset(); CHECK(!Step()); Set(39,true); CHECK(!enabled[39] && notifications==1);
    RunFor(300); Set(39,false); RunFor(400); CHECK(enabled[39] && reenables>0);
    CHECK(!Step()); const auto old_reads=reads; RunFor(5000); CHECK(reads==old_reads);
    Drain(); Set(39,true); RunFor(100); Set(39,false); RunFor(400); CHECK(Drain().size()==3);
    Reset(); // An asserted line immediately re-enters the ISR during enable.
    Set(18,true); Set(18,false); during_enable=[] { levels[18]=0; };
    ReenableIdleButtonInterrupts(); CHECK(!enabled[18] && g_buttons[1].isr_disabled);
    RunFor(100); Set(18,false); RunFor(400); CHECK(Drain().size()==3);
}
int main(int argc,char** argv) {
    const std::string which=argc>1 ? argv[1] : "all";
    if(which=="all" || which=="gestures") Gestures();
    if(which=="all" || which=="capacity") Capacity();
    if(which=="all" || which=="stale") Stale();
    if(which=="all" || which=="ring") Ring();
    if(which=="all" || which=="sleep") Sleep();
    if(which=="all" || which=="irq") Irq();
    std::printf("Button GPIO/task/ring/PTT/sleep %d Hz: %u checks PASS (host ports; not HIL)\n",configTICK_RATE_HZ,checks);
}
