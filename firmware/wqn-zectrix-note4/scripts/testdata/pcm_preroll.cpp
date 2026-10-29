#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "cJSON.h"

using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=257, ESP_ERR_INVALID_ARG=258;
constexpr int ESP_ERR_INVALID_STATE=259, ESP_ERR_INVALID_SIZE=260, ESP_ERR_TIMEOUT=263;
using TickType_t=uint32_t;
constexpr int pdTRUE=1, pdFALSE=0, eSetBits=0;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
void HostLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
constexpr char kTag[]="preroll-test";
constexpr int kEvtBitResetRequired=1, kEvtBitPcmPending=2, kEvtBitCtrlPending=4;
namespace wqn {
constexpr uint16_t kWflvFlagStream=1, kWflvFlagFinal=2;
namespace stdpro_ws {
enum class TurnStreamObservation { kNone };
enum class FinalHandoffResult { kFinalSent, kDefinitelyNotSent, kAmbiguous };
void PushPcm(const int16_t*, size_t);
}
namespace voice_diagnostics { const char* SafeKey(const char* key) { return key; } }
}
@@CONSTANTS@@
@@TYPES@@
enum class ConnectionState { kReady, kDisconnected };
std::atomic<ConnectionState> g_conn_state{ConnectionState::kReady};
std::atomic<TurnState> g_turn_state{TurnState::kIdle};
std::atomic<bool> g_session_ready{true}, g_prefinal_pcm_lost{false};
std::atomic<bool> g_pcm_preroll_pending{false}, g_preroll_pending{false};
std::atomic<uint32_t> g_seq{0}, g_sync_op_gen{0}, g_turn_gen{0};
uint32_t g_active_turn_gen=0;
char g_active_turn_req_id[64]={};
std::string g_current_req_id;
wqn::stdpro_ws::TurnStreamObservation g_active_turn_stream_obs{};
wqn::stdpro_ws::FinalHandoffResult g_final_result=wqn::stdpro_ws::FinalHandoffResult::kFinalSent;
struct Parser { void reset() {} } g_sse_parser;
int pcm_handle, ctrl_handle, completion_handle, mutex_handle, sync_handle, final_handle, complete_handle, task_handle, ws_handle;
void* g_pcm_queue=&pcm_handle;
void* g_ctrl_queue=&ctrl_handle;
void* g_ctrl_completion_queue=&completion_handle;
void* g_transport_mutex=&mutex_handle;
void* g_sync_op_sem=&sync_handle;
void* g_final_sem=&final_handle;
void* g_turn_complete_sem=&complete_handle;
void* g_transport_task=&task_handle;
void* g_ws_client=&ws_handle;
int transport_users=0;
struct TransportUseRegistration {
    TransportUseRegistration() { ++transport_users; }
    ~TransportUseRegistration() { --transport_users; }
};
int64_t clock_ms=0;
int64_t esp_timer_get_time() { return clock_ms*1000; }
esp_err_t InitPrimitives(const char*) { return ESP_OK; }
bool final_signal=true;
int xSemaphoreTake(void* sem, TickType_t ticks) { return sem==g_final_sem && ticks>0 && !final_signal ? pdFALSE : pdTRUE; }
int xSemaphoreGive(void*) { return pdTRUE; }
void LogWsPhase(const char*, const char*, uint32_t, int) {}

std::vector<int16_t> captured;
std::deque<PcmBlock> pcm;
std::deque<CtrlCmd> commands;
std::deque<CtrlCompletion> completions;
std::vector<PcmBlock> delivered;
std::function<void()> before_owner, during_send, after_commit, after_rejected;
bool connected=true, missing_capture=false, short_capture=false, drain_each=false;
int wire_starts=0, wire_result_delta=0, reset_requests=0, destroys=0;
size_t queue_attempts=0, fail_queue_at=0, checks=0, failures=0;
void Check(bool pass, const char* message)
{
    ++checks;
    if (!pass) {
        if (failures < 20) std::fprintf(stderr,"FAIL %s\n",message);
        ++failures;
    }
}
namespace wqn {
size_t PeekAudioCaptureSamples(const int16_t** output)
{
    *output=missing_capture ? nullptr : captured.data();
    return short_capture ? 1 : captured.size();
}
}
void Drain()
{
    while (!pcm.empty()) { delivered.push_back(pcm.front()); pcm.pop_front(); }
}
int xQueueSend(void* queue, const void* payload, TickType_t)
{
    if (queue==g_pcm_queue) {
        if (++queue_attempts==fail_queue_at || pcm.size()>=kPcmQueueLength) return pdFALSE;
        pcm.push_back(*static_cast<const PcmBlock*>(payload));
    } else commands.push_back(*static_cast<const CtrlCmd*>(payload));
    return pdTRUE;
}
int xQueueReceive(void* queue, void* payload, TickType_t ticks)
{
    if (queue==g_pcm_queue) {
        if (pcm.empty()) return pdFALSE;
        *static_cast<PcmBlock*>(payload)=pcm.front(); pcm.pop_front();
    } else {
        if (completions.empty()) { clock_ms+=ticks; return pdFALSE; }
        *static_cast<CtrlCompletion*>(payload)=completions.front(); completions.pop_front();
    }
    return pdTRUE;
}
size_t uxQueueMessagesWaiting(void*) { return pcm.size(); }
bool esp_websocket_client_is_connected(void*) { return connected; }
int esp_websocket_client_send_text(void*, const char*, size_t count, TickType_t)
{
    ++wire_starts;
    if (during_send) during_send();
    return static_cast<int>(count)+wire_result_delta;
}
void DoDestroyWs()
{
    ++destroys; connected=false; g_session_ready=false; g_conn_state=ConnectionState::kDisconnected;
}
void PublishCompletion(uint32_t generation, esp_err_t result, CmdDisposition disposition)
{
    if (result==ESP_OK && after_commit) after_commit();
    if (result!=ESP_OK && after_rejected) after_rejected();
    completions.push_back({generation,result,disposition});
}
void ExecuteStart(const CtrlCmd& cmd)
{
    switch (cmd.type) {
@@START_CASE@@
        default: std::abort();
    }
}
void xTaskNotify(void*, int bits, int)
{
    if (bits==kEvtBitResetRequired) ++reset_requests;
    else if (bits==kEvtBitPcmPending && drain_each) Drain();
    else if (bits==kEvtBitCtrlPending) {
        if (before_owner) before_owner();
        const auto cmd=commands.front(); commands.pop_front(); ExecuteStart(cmd);
    }
}
namespace wqn::stdpro_ws {
@@START@@
@@PUSH@@
@@FINAL@@
}
namespace wqn { @@LEGACY_ARM@@ }
@@TAP@@

void Capture(size_t count=240)
{
    const size_t begin=captured.size();
    for (size_t i=0;i<count;++i) captured.push_back(static_cast<int16_t>((begin+i)%30000));
    AudioCaptureTapHandler(captured.data()+begin,count,nullptr);
}
void Reset()
{
    captured.clear(); captured.reserve(100000);
    pcm.clear(); commands.clear(); completions.clear(); delivered.clear();
    before_owner=during_send=after_commit=after_rejected={};
    connected=final_signal=true; missing_capture=short_capture=drain_each=false;
    g_session_ready=true; g_conn_state=ConnectionState::kReady; g_turn_state=TurnState::kIdle;
    g_pcm_preroll_pending=g_preroll_pending=g_prefinal_pcm_lost=false;
    g_seq=g_sync_op_gen=g_turn_gen=0; g_active_turn_gen=0;
    g_current_req_id.clear(); std::memset(g_active_turn_req_id,0,sizeof(g_active_turn_req_id));
    g_pcm_queue=&pcm_handle; g_ws_client=&ws_handle;
    wire_starts=wire_result_delta=reset_requests=destroys=0;
    queue_attempts=fail_queue_at=0; clock_ms=0;
}
esp_err_t Start(const char* id="test-turn")
{
    wqn::ArmAiVoicePreroll(); // Real legacy API only when testing the baseline.
    uint32_t generation=0;
    const auto result=wqn::stdpro_ws::StartTurn(id,"agent","",false,nullptr,&generation);
    if (result==ESP_OK) Check(generation==g_active_turn_gen && generation!=0,"committed turn identity returned");
    return result;
}
void OrderedCapture()
{
    Drain();
    std::vector<int16_t> output;
    bool sequence=true, sizes=true;
    for (size_t i=0;i<delivered.size();++i) {
        const auto& block=delivered[i];
        sequence=sequence && block.seq==i && block.flags==wqn::kWflvFlagStream;
        sizes=sizes && block.sample_count>0 && block.sample_count<=kPcmFramesPerBlock;
        output.insert(output.end(),block.samples,block.samples+block.sample_count);
    }
    Check(output==captured,"every captured sample appears exactly once in original order");
    Check(sequence,"prefix/live blocks have continuous sequence numbers from zero");
    Check(sizes,"all PCM blocks obey the actual transport block limit");
    Check(!g_prefinal_pcm_lost && reset_requests==0,"ordered replay does not spuriously fail the turn");
}
size_t allocations=0, fail_allocation=0;
void* Allocate(size_t size) { return ++allocations==fail_allocation ? nullptr : std::malloc(size); }

int main()
{
    // Interleave taps before owner dispatch, inside the blocking start send,
    // and after commit but before completion reaches the waiting caller.
    for (int before : {0,1,3,8}) for (int sending : {0,1,3}) for (bool preempt : {false,true}) {
        Reset();
        before_owner=[before] { for (int i=0;i<before;++i) Capture(113); Check(pcm.empty(),"no PCM admitted before start send"); };
        during_send=[sending] { for (int i=0;i<sending;++i) Capture(239); Check(pcm.empty(),"no PCM admitted during unconfirmed start send"); };
        after_commit=[preempt] { if (preempt) Capture(31); };
        Check(Start()==ESP_OK,"valid start commits");
        Capture(240); Capture(17);
        OrderedCapture();
    }
    Reset(); drain_each=true;
    before_owner=[] { for (int i=0;i<8;++i) Capture(); };
    after_commit=[] { Capture(); };
    Check(Start()==ESP_OK,"owner may drain replay while capture enqueues");
    Capture(); OrderedCapture();

    Reset();
    before_owner=[] { Capture(); Capture(); };
    Check(Start()==ESP_OK,"first delayed tap starts with full backlog");
    Capture(); OrderedCapture();
    // A new successful commit resets replay and sequence after an unused or
    // completed old turn; stale queue contents are drained before publication.
    g_turn_state=TurnState::kIdle; captured.clear(); delivered.clear();
    PcmBlock old{}; old.seq=99; old.sample_count=1; old.samples[0]=-1; pcm.push_back(old);
    Check(Start("next-turn")==ESP_OK && g_active_turn_gen==2,"new turn commits after prior recording");
    Capture(61); OrderedCapture();

    for (int delta : {-1,-10000}) {
        Reset(); wire_result_delta=delta;
        during_send=[] { Capture(); Check(pcm.empty(),"failed send cannot admit provisional PCM"); };
        Check(Start()!=ESP_OK && g_turn_state==TurnState::kIdle,"short or failed wire send cannot commit recording");
        Capture(); Check(pcm.empty() && destroys==1,"rejected start leaves no prefix/live PCM to send");
    }
    Reset(); connected=false;
    Check(Start()!=ESP_OK && wire_starts==0,"owner rejects a disconnected session before sending start");
    Capture(); Check(pcm.empty(),"rejected owner start does not arm replay");

    for (bool short_view : {false,true}) {
        Reset(); Check(Start()==ESP_OK,"start for invalid capture view");
        missing_capture=!short_view; short_capture=short_view;
        Capture();
        Check(g_prefinal_pcm_lost && reset_requests==1 && pcm.empty(),"unavailable prefix fails closed without truncated live success");
        missing_capture=short_capture=false; Capture();
        Check(pcm.empty() && reset_requests==1,"failed prefix cannot resume as a live-only turn");
    }
    // Fail every enqueue site in a prefix spanning three blocks, then live.
    for (size_t site=1;site<=4;++site) {
        Reset();
        before_owner=[] { Capture(); Capture(); Capture(); };
        Check(Start()==ESP_OK,"start for queue failure injection");
        fail_queue_at=site;
        Capture();
        Check(g_prefinal_pcm_lost && reset_requests==1,"prefix/live queue failure latches pre-FINAL failure");
        Check(queue_attempts==site && pcm.size()==site-1,"queue rejection stops both prefix and live spans immediately");
        Capture(); Check(queue_attempts==site,"later taps cannot resume after PCM loss");
    }
    Reset();
    before_owner=[] { for (size_t i=0;i<kPcmQueueLength+1;++i) Capture(); };
    Check(Start()==ESP_OK,"start after a backlog beyond queue capacity");
    Capture();
    Check(g_prefinal_pcm_lost && reset_requests==1 && pcm.size()==kPcmQueueLength,
          "real queue capacity rejects excess backlog instead of claiming continuous audio");

    // Malformed input never consumes the first valid capture block's replay.
    Reset(); Check(Start()==ESP_OK,"start for empty tap control");
    wqn::stdpro_ws::PushPcm(nullptr,240); wqn::stdpro_ws::PushPcm(nullptr,0);
    Capture(); OrderedCapture();
    Reset(); g_pcm_queue=nullptr; Capture();
    Check(pcm.empty() && queue_attempts==0,"uninitialized transport drops early tap safely");

    using Final=wqn::stdpro_ws::FinalHandoffResult;
    Reset(); before_owner=[] { Capture(); Capture(); };
    Check(Start()==ESP_OK,"start when capture stops before its next tap");
    const auto original=captured;
    Check(wqn::stdpro_ws::SendFinalAndWait("test-turn",30,2000)==Final::kDefinitelyNotSent,
          "no admitted tap cannot send an empty FINAL");
    Check(pcm.empty() && queue_attempts==0 && reset_requests==1 && captured==original,
          "no-tap recovery retires receiving transport and preserves full clip for batch");
    Reset(); Check(Start()==ESP_OK,"start for normal ordered FINAL");
    Capture();
    const size_t pcm_count=pcm.size();
    Check(wqn::stdpro_ws::SendFinalAndWait("test-turn",15,2000)==Final::kFinalSent,
          "admitted capture preserves normal confirmed FINAL result");
    Check(pcm.size()==pcm_count+1 && pcm.back().flags==wqn::kWflvFlagFinal &&
          pcm.back().seq==pcm_count && reset_requests==0,"FINAL is queued after prefix/live with the next sequence");
    Reset(); Check(Start()==ESP_OK,"start for ambiguous FINAL control"); Capture(); final_signal=false;
    Check(wqn::stdpro_ws::SendFinalAndWait("test-turn",15,2000)==Final::kAmbiguous,
          "lost send confirmation remains ambiguous and cannot enable batch replay");
    Reset(); Check(Start()==ESP_OK,"start for rejected FINAL enqueue"); Capture(); fail_queue_at=queue_attempts+1;
    Check(wqn::stdpro_ws::SendFinalAndWait("test-turn",15,2000)==Final::kDefinitelyNotSent && reset_requests==1,
          "rejected FINAL enqueue preserves safe pre-FINAL recovery");

    // Real start JSON allocation failures cannot publish Recording or replay.
    cJSON_Hooks hooks{Allocate,std::free}; cJSON_InitHooks(&hooks);
    Reset(); allocations=0; fail_allocation=0;
    Check(Start()==ESP_OK,"allocation profile reaches successful start");
    const size_t sites=allocations;
    for (size_t site=1;site<=sites;++site) {
        Reset(); allocations=0; fail_allocation=site;
        Check(Start()==ESP_ERR_NO_MEM,"start allocation failure returns NO_MEM");
        Check(wire_starts==0 && g_turn_state==TurnState::kIdle,"allocation failure never commits partial start");
        Capture(); Check(pcm.empty(),"allocation failure does not admit capture");
    }
    cJSON_InitHooks(nullptr);
    std::printf("PCM commit/preroll: %zu checks, %zu failures; 24 schedules, %zu start JSON allocation sites (RTOS/WS ports; NOT HIL)\n",checks,failures,sites);
    return failures==0 ? 0 : 1;
}
