// Production ASR pipe is a separate translation unit. This unit extracts the
// complete production owner root loop, actual turn commit, SSE consumer,
// teardown, PCM/FINAL APIs and terminal ledger. Only RTOS/SDK I/O are ports.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "agent_voice_pipe.h"
#include "stdpro_ws_transport.h"
#include "sse_chunk.h"
#include "wflv_frame.h"
#include "voice_diagnostics.h"
#include "network_response_limits.h"
#include "turn_release_deadline.h"
#include "device_protocol/json_depth_guard.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

using namespace wqn::stdpro_ws;
constexpr char kTag[]="release-test";
constexpr int eSetBits=1, kEvtBitResetRequired=1, kEvtBitCtrlPending=2, kEvtBitPcmPending=4;
@@CONSTANTS@@
@@TYPES@@
int64_t clock_ms=0, drive_end=0, stopped_at=-1;
bool driving=false;
size_t checks=0, failures=0;
int stops=0, destroys=0, finals=0, pcm_sends=0, starts=0, aborts=0, disconnects=0, connects=0;
std::atomic<ConnectionState> g_conn_state{ConnectionState::kReady};
std::atomic<TurnState> g_turn_state{TurnState::kIdle};
std::atomic<bool> g_is_connected{true}, g_session_ready{true};
std::atomic<bool> g_prefinal_pcm_lost{false}, g_pcm_preroll_pending{false};
std::atomic<uint32_t> g_seq{0}, g_turn_gen{0}, g_sync_op_gen{0};
uint32_t g_active_turn_gen=0, g_active_conn_op_gen=0;
int64_t g_conn_deadline_ms=0;
char g_active_turn_req_id[64]={};
bool g_active_turn_asr_only=false;
TurnReleaseDeadline g_turn_release_deadline;
TurnStreamObservation g_active_turn_stream_obs=TurnStreamObservation::kNone;
FinalHandoffResult g_final_result=FinalHandoffResult::kFinalSent;
std::string g_current_req_id, g_control_reassembly_buf, input;
wqn::SseFrameBuffer g_sse_parser;
wqn::WqnAiSseCallback g_sse_callback=nullptr;
void* g_sse_user_ctx=nullptr;
TurnTerminalRecord g_terminal_turn_record;
portMUX_TYPE g_terminal_turn_mux=portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t mutex_sem{true,true}, sync_sem, final_sem, terminal_sem;
SemaphoreHandle_t g_transport_mutex=&mutex_sem, g_sync_op_sem=&sync_sem;
SemaphoreHandle_t g_final_sem=&final_sem, g_turn_complete_sem=&terminal_sem, drive_wake=nullptr;
int pcm_handle, ctrl_handle, completion_handle, stream_handle, task_handle, ws_handle;
void* g_pcm_queue=&pcm_handle;
void* g_ctrl_queue=&ctrl_handle;
void* g_ctrl_completion_queue=&completion_handle;
void* g_text_stream=&stream_handle;
void* g_transport_task=&task_handle;
void* g_ws_client=&ws_handle;
std::deque<PcmBlock> pcm;
std::deque<CtrlCmd> commands;
std::deque<CtrlCompletion> completions;
std::vector<int16_t> capture(240,123);
size_t packet_limit=512;
struct Event { int64_t at; std::function<void()> apply; };
std::vector<Event> events;
struct Yield {};

void Check(bool pass, const char* message)
{
    ++checks;
    if (!pass) {
        if (failures<18) std::fprintf(stderr,"FAIL %s at %lld ms\n",message,static_cast<long long>(clock_ms));
        ++failures;
    }
}
void HostLog(const char*, const char*, ...) {}
void HostEnter(portMUX_TYPE* mux) { mux->lock(); }
void HostExit(portMUX_TYPE* mux) { mux->unlock(); }
int64_t esp_timer_get_time() { return clock_ms*1000; }
void LogWsPhase(const char*, const char*, uint32_t, esp_err_t) {}
void LogTransportMemory(const char*, const char* =nullptr, uint32_t =0) {}
void LogSendFailure(const char*, int, size_t, uint32_t, int64_t) { std::abort(); }
void Drive(int64_t end, SemaphoreHandle_t wake=nullptr);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* p) { *p={}; return p; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p) { *p={true,true}; return p; }
int xSemaphoreGive(SemaphoreHandle_t p) { p->available=true; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t p, TickType_t ticks)
{
    if (!p->available && p->mutex) std::abort();
    if (!p->available && ticks>0) {
        if (driving) std::abort();
        Drive(clock_ms+static_cast<int64_t>(ticks)*1000/configTICK_RATE_HZ,p);
    }
    if (!p->available) return pdFALSE;
    p->available=false; return pdTRUE;
}
int xQueueSend(void* q, const void* payload, TickType_t)
{
    if (q==g_pcm_queue) {
        if (pcm.size()>=kPcmQueueLength) return pdFALSE;
        pcm.push_back(*static_cast<const PcmBlock*>(payload));
    } else if (q==g_ctrl_queue) commands.push_back(*static_cast<const CtrlCmd*>(payload));
    else std::abort();
    return pdTRUE;
}
int xQueueReceive(void* q, void* payload, TickType_t ticks)
{
    if (q==g_pcm_queue) {
        if (pcm.empty()) return pdFALSE;
        *static_cast<PcmBlock*>(payload)=pcm.front(); pcm.pop_front();
    } else if (q==g_ctrl_queue) {
        if (commands.empty()) return pdFALSE;
        *static_cast<CtrlCmd*>(payload)=commands.front(); commands.pop_front();
    } else if (q==g_ctrl_completion_queue) {
        if (completions.empty()) { clock_ms+=static_cast<int64_t>(ticks)*1000/configTICK_RATE_HZ; return pdFALSE; }
        *static_cast<CtrlCompletion*>(payload)=completions.front(); completions.pop_front();
    } else std::abort();
    return pdTRUE;
}
size_t uxQueueMessagesWaiting(void*) { return pcm.size(); }
void PublishCompletion(uint32_t gen, esp_err_t result, CmdDisposition disposition)
{ completions.push_back({gen,result,disposition}); }
void xStreamBufferReset(void*) { input.clear(); }
size_t xStreamBufferReceive(void*, char* dst, size_t size, TickType_t)
{
    const size_t count=std::min({input.size(),size,packet_limit});
    std::memcpy(dst,input.data(),count); input.erase(0,count); return count;
}
void esp_websocket_client_stop(void*) { ++stops; stopped_at=clock_ms; }
void esp_websocket_client_destroy(void*) { ++destroys; }
bool esp_websocket_client_is_connected(void*) { return g_is_connected.load(); }
int esp_websocket_client_send_text(void*, const char* data, size_t size, TickType_t)
{
    Check(std::string(data,size).find("voice.turn.start")!=std::string::npos,"only new-turn control sent");
    ++starts; return static_cast<int>(size);
}
int esp_websocket_client_send_bin(void*, const char* data, size_t size, TickType_t)
{
    Check(size>=wqn::kWflvHeaderBytes && std::memcmp(data,"WFLV",4)==0,"real WFLV encoder used");
    if (static_cast<unsigned char>(data[6]) & wqn::kWflvFlagFinal) ++finals;
    else ++pcm_sends;
    return static_cast<int>(size);
}
int transport_users=0;
struct TransportUseRegistration {
    TransportUseRegistration() { ++transport_users; }
    ~TransportUseRegistration() { --transport_users; }
};
esp_err_t InitPrimitives(const char*) { return ESP_OK; }
namespace wqn {
size_t PeekAudioCaptureSamples(const int16_t** out) { *out=capture.data(); return capture.size(); }
}

void RetireWsEvents() {}  // Callback lifetime is exercised by test_ws_event_lifetime.
@@OWNER@@

void ExecuteCtrlCmdSingleStep(const CtrlCmd& cmd)
{
    switch(cmd.type) {
@@START_CASE@@
        default: std::abort();
    }
}
void xTaskNotify(void*, int bits, int)
{
    if (bits==kEvtBitCtrlPending) {
        const auto cmd=commands.front(); commands.pop_front(); ExecuteCtrlCmdSingleStep(cmd);
    } else if (bits==kEvtBitResetRequired) TeardownFailedTurn();
}
void TryIdleDisconnect() {} // No idle-disconnect request in these schedules.
uint32_t ulTaskNotifyTake(int, TickType_t) { return 1; }
int xTaskNotifyWait(uint32_t, uint32_t, uint32_t* bits, TickType_t ticks)
{
    if ((drive_wake && drive_wake->available) || clock_ms>=drive_end) throw Yield{};
    int64_t next=std::min(drive_end,clock_ms+static_cast<int64_t>(ticks)*1000/configTICK_RATE_HZ);
    if (!events.empty()) next=std::min(next,std::max(clock_ms,events.front().at));
    clock_ms=next;
    while (!events.empty() && events.front().at<=clock_ms) {
        auto event=std::move(events.front()); events.erase(events.begin()); event.apply();
    }
    *bits=input.empty()?0:8;
    return *bits?pdTRUE:pdFALSE;
}
void HandleAllNotificationBits(uint32_t bits) { if (bits&8) ProcessTextStream(); }
@@ROOT@@
void Drive(int64_t end, SemaphoreHandle_t wake)
{
    if (driving || end<clock_ms) std::abort();
    drive_end=end; drive_wake=wake; driving=true;
    try { VoiceWsTransportTask(nullptr); } catch(const Yield&) {}
    driving=false; drive_wake=nullptr;
}
namespace wqn::stdpro_ws {
esp_err_t EnsureConnected(const std::string&, uint32_t, const char*)
{
    if (!g_session_ready) {
        ++connects; g_ws_client=&ws_handle; g_is_connected=g_session_ready=true;
        g_conn_state=ConnectionState::kReady;
    }
    return ESP_OK;
}
void AbortTurn(const std::string&, uint32_t) { ++aborts; }
void Disconnect() { ++disconnects; TeardownFailedTurn(TeardownReason::kLocalDisconnect); }
@@PUBLIC@@
}

void Schedule(int64_t at, std::function<void()> apply)
{
    events.push_back({at,std::move(apply)});
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b) { return a.at<b.at; });
}
std::string Frame(const char* name, const char* request=nullptr)
{
    std::string data="{\"text\":\"recognized\"";
    if (request) data+=",\"request_id\":\""+std::string(request)+"\"";
    return "event: "+std::string(name)+"\ndata: "+data+"}\n\n";
}
void At(int64_t at, const std::string& frame) { Schedule(at,[frame] { input+=frame; }); }
void Reset()
{
    wqn::AgentVoiceTurnAbort();
    DoDestroyWs();
    clock_ms=0; stopped_at=-1; events.clear(); input.clear();
    pcm.clear(); commands.clear(); completions.clear();
    stops=destroys=finals=pcm_sends=starts=aborts=disconnects=connects=0;
    g_turn_state=TurnState::kIdle; g_active_turn_gen=0; g_active_turn_req_id[0]='\0';
    g_current_req_id.clear(); g_terminal_turn_record={};
    final_sem=terminal_sem=sync_sem={}; mutex_sem={true,true};
    g_ws_client=&ws_handle; g_is_connected=g_session_ready=true; g_conn_state=ConnectionState::kReady;
    wqn::AgentVoiceRegisterDeltaCallback(nullptr,nullptr);
}
uint32_t Begin(const char* id="req-current", const char* tier="agent")
{
    uint32_t gen=0;
    const auto result=std::strcmp(tier,"agent")==0
        ? wqn::AgentVoiceTurnStart("synthetic-token",id,&gen)
        : StartTurn(id,tier,"",false,nullptr,&gen);
    Check(result==ESP_OK && gen!=0,"real public API and owner commit accept turn");
    PushPcm(capture.data(),capture.size());
    return gen;
}
void Handoff()
{
    Check(SendFinalAndWait("req-current",1000,2000)==FinalHandoffResult::kFinalSent,"real FINAL handoff confirmed");
    Check(g_turn_state==TurnState::kRemoteHandoff && finals==1,"owner committed FINAL once");
}
void Abandoned(uint32_t gen, const char* id="req-current")
{
    TurnWaitResult terminal;
    Check(g_turn_state==TurnState::kIdle && !g_session_ready && stops==1 && destroys==1,
          "owner closes missing-release connection and returns idle");
    Check(TryGetTurnTerminal(gen,id,&terminal) && terminal.release_state==TurnReleaseState::kLocallyAbandoned,
          "identity-bound ledger records local abandonment, never forged remote release");
    Check(!g_turn_release_deadline.Expired(gen,id,clock_ms+1000000),"teardown retires expired timer");
    Check(finals==1 && pcm_sends==1 && starts==1 && aborts==0 && disconnects==0,
          "autonomous expiry does not replay PCM/FINAL or send remote abort/caller disconnect");
}
int main()
{
    for (size_t chunk: {size_t(1),size_t(17),size_t(512)}) {
        packet_limit=chunk;
        Reset(); const auto gen=Begin(); At(100,Frame("asr.complete"));
        std::string transcript;
        Check(wqn::AgentVoiceTurnFinalize(1000,15000,&transcript)==ESP_OK && transcript=="recognized",
              "real ASR callback/finalize returns recognized text before release");
        Check(clock_ms==100 && stops==0,"no added ASR wait latency");
        Drive(3099); Check(stops==0,"terminal grace not expired early");
        Drive(3150); Abandoned(gen);
        Check(stopped_at>=3100 && stopped_at<=3150 && transcript=="recognized","bounded owner cleanup preserves returned transcript");
        uint32_t next=0;
        Check(wqn::AgentVoiceTurnStart("synthetic-token","next-turn",&next)==ESP_OK && next!=gen && connects==1,
              "next real pipe turn reconnects and commits after missing release");
        Check(!g_turn_release_deadline.Expired(next,"next-turn",999999),"new turn does not inherit expired timer");
    }
    Reset(); auto gen=Begin(); Handoff(); At(100,Frame("asr.complete"));
    At(3100,Frame("turn.released","req-current")); Drive(4000);
    TurnWaitResult terminal;
    Check(stops==0 && g_session_ready && g_turn_state==TurnState::kIdle,"matching release at deadline wins over expiry");
    Check(TryGetTurnTerminal(gen,"req-current",&terminal) && terminal.release_state==TurnReleaseState::kReleased,
          "matching remote release is retained in actual ledger");
    Check(!g_turn_release_deadline.Expired(gen,"req-current",999999),"release clears deadline");
    Check(Begin("reuse-turn")!=gen && connects==0,"healthy released connection reused");

    Reset(); gen=Begin(); Handoff();
    for (int64_t at: {100,900,1900,2900}) At(at,Frame("asr.complete"));
    At(3000,": keepalive\n\n"+Frame("turn.done"));
    At(3050,Frame("turn.released","old-turn")); Drive(3150); Abandoned(gen);
    Check(stopped_at>=3100 && stopped_at<=3150,"duplicate terminals, comments and foreign release never extend grace");
    Check(TryGetTurnTerminal(gen,"req-current",&terminal) && terminal.stream_observation==TurnStreamObservation::kTurnDoneObserved,
          "local abandonment preserves actual business terminal observation");

    Reset(); gen=Begin(); Handoff(); At(100,Frame("asr.complete","foreign")); Drive(4000);
    Check(stops==0 && g_turn_state==TurnState::kRemoteHandoff,"explicit foreign terminal cannot arm current short grace");
    At(5000,Frame("asr.complete","req-current")); Drive(8050); Abandoned(gen);
    Check(stopped_at>=8000 && stopped_at<=8050,"matching terminal arms grace for correct identity");

    for (const char* name: {"asr.failed","error"}) {
        Reset(); gen=Begin(); At(100,Frame(name)); std::string text;
        Check(wqn::AgentVoiceTurnFinalize(1000,15000,&text)==ESP_ERR_INVALID_RESPONSE && clock_ms==100,
              "ASR error result still returns promptly");
        Drive(3150); Abandoned(gen);
    }
    Reset(); gen=Begin(); At(10000,Frame("asr.complete")); std::string text;
    Check(wqn::AgentVoiceTurnFinalize(1000,120000,&text)==ESP_OK && text=="recognized" && clock_ms==10000,
          "slow ASR survives beyond short grace measured from FINAL");
    Drive(13050); Abandoned(gen);

    for (const char* tier: {"std","pro"}) {
        Reset(); gen=Begin("req-current",tier); Handoff(); At(100,Frame("asr.complete")); Drive(200000);
        Check(stops==0 && g_turn_state==TurnState::kRemoteHandoff,"full dialogue survives ASR terminal and 150s ASR-only cap");
        At(200100,Frame("turn.done")); Drive(203150); Abandoned(gen);
    }
    Reset(); gen=Begin(); Schedule(100,[] { wqn::AgentVoiceRequestCancel(); });
    Check(wqn::AgentVoiceTurnFinalize(1000,120000,&text)==ESP_OK && text.empty() && clock_ms==100,
          "soft cancel returns without requiring a remote terminal");
    Drive(149999); Check(stops==0,"ASR-only absolute cap not expired early");
    Drive(150050); Abandoned(gen);
    Check(stopped_at>=150000 && stopped_at<=150050,"detached owner cleans up even without any result or release");
    Reset(); gen=Begin(); Handoff(); At(149000,Frame("asr.complete")); Drive(150050); Abandoned(gen);
    Check(stopped_at>=150000 && stopped_at<=150050,"late terminal cannot extend absolute cap");
    Reset(); gen=Begin();
    Check(wqn::AgentVoiceTurnFinalize(1000,15000,&text)==ESP_ERR_TIMEOUT,"normal no-result timeout preserved");
    Check(clock_ms>=18000 && clock_ms<=18100 && disconnects==1 && finals==1,"existing no-result release observation still recovers without replay");

    // Independent identity/reset/monotonicity assertions for the exact owner helper.
    TurnReleaseDeadline deadline;
    constexpr int64_t far=INT64_C(5000000000);
    deadline.Arm(7,"req",far,3000);
    Check(!deadline.Expired(7,"req",far+2999) && deadline.Expired(7,"req",far+3000),"64-bit uptime boundary inclusive");
    Check(!deadline.Expired(8,"req",far+4000) && !deadline.Expired(7,"other",far+4000),"deadline requires generation and request identity");
    deadline.Arm(7,"req",far+2000,3000);
    Check(deadline.Expired(7,"req",far+3000),"repeated observations never renew deadline");
    deadline.Reset(); Check(!deadline.Expired(7,"req",far+99999),"reset retires timer");
    for (const char* id: {"",static_cast<const char*>(nullptr)}) deadline.Arm(7,id,far,3000);
    deadline.Arm(0,"req",far,3000); deadline.Arm(7,"req",far,0);
    const std::string long_id(64,'x'); deadline.Arm(7,long_id.c_str(),far,3000);
    Check(!deadline.Expired(7,"req",far+99999),"invalid identities and budgets cannot arm deadline");
    deadline.Arm(7,"req",far,150000); deadline.Arm(7,"req",far+100,3000);
    Check(deadline.Expired(7,"req",far+3100),"business terminal shortens absolute cap");
    deadline.Arm(8,"req",far+10000,3000);
    Check(!deadline.Expired(7,"req",far+20000) && deadline.Expired(8,"req",far+20000),"new generation with reused request retires old identity");
    Check(transport_users==0,"public transport registration balanced");
    std::printf("ASR handoff owner deadline: %zu checks, %zu failures; %d Hz virtual RTOS, real pipe/SSE/owner/ledger; NOT HIL\n",
                checks,failures,configTICK_RATE_HZ);
    return failures?1:0;
}
