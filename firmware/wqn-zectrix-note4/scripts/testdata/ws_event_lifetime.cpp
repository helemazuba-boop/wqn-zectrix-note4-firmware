#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include "esp_err.h"
#include "turn_release_deadline.h"
#include "ws_event_mailbox.h"
using TickType_t=uint32_t;
using TaskHandle_t=void*;
constexpr int pdTRUE=1,pdFALSE=0,eSetBits=1;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
#define CONFIG_WQN_AUDIO_RESOURCE_DIAGNOSTICS 0
template<class... A>void Log(A...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_LOGD(...) Log(__VA_ARGS__)
using portMUX_TYPE=std::mutex;
#define portENTER_CRITICAL(p) ((p)->lock())
#define portEXIT_CRITICAL(p) ((p)->unlock())
#define portMUX_INITIALIZER_UNLOCKED {}
constexpr char kTag[]="lifetime-host",TAG[]="sdk-host";
namespace wqn::stdpro_ws {@@PUBLIC_ENUMS@@}
namespace wqn {
std::string BuildVoiceWebSocketUrl(){return "wss://fixture.invalid/ws";}
namespace voice_diagnostics {const char* SafeKey(const char* p){return p?p:"";}}
}
@@CONSTANTS@@
@@TYPES@@
using esp_event_base_t=const char*;
using esp_event_handler_t=void(*)(void*,esp_event_base_t,int32_t,void*);
constexpr esp_event_base_t WEBSOCKET_EVENTS="ws";
enum esp_websocket_event_id_t {WEBSOCKET_EVENT_ANY=-1,WEBSOCKET_EVENT_ERROR,
    WEBSOCKET_EVENT_CONNECTED,WEBSOCKET_EVENT_DISCONNECTED,WEBSOCKET_EVENT_DATA,
    WEBSOCKET_EVENT_CLOSED,WEBSOCKET_EVENT_BEFORE_CONNECT,WEBSOCKET_EVENT_BEGIN,WEBSOCKET_EVENT_FINISH};
enum esp_websocket_error_type_t {WEBSOCKET_ERROR_TYPE_NONE,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT};
enum {WEBSOCKET_STATE_UNKNOW,WEBSOCKET_STATE_INIT,WEBSOCKET_STATE_CONNECTED,WEBSOCKET_STATE_WAIT_TIMEOUT,WEBSOCKET_STATE_CLOSING};
struct esp_websocket_client;
using esp_websocket_client_handle_t=esp_websocket_client*;
struct Error {int error_type=0,esp_tls_last_esp_err=0,esp_tls_stack_err=0,esp_tls_cert_verify_flags=0,esp_transport_sock_errno=0,esp_ws_handshake_status_code=0;};
struct esp_websocket_event_data_t {
    esp_websocket_client_handle_t client=nullptr;void* user_context=nullptr;const char* data_ptr=nullptr;
    int data_len=0,fin=0,op_code=0,payload_len=0,payload_offset=0,close_status_code=0;Error error_handle;
};
struct Loop {esp_event_handler_t callback=nullptr;void* arg=nullptr;std::deque<std::pair<int,esp_websocket_event_data_t>> queue;};
struct Group {uint32_t bits=0;esp_websocket_client* client=nullptr;};
struct ClientConfig {bool auto_reconnect=true;void* user_context=nullptr;};
struct esp_websocket_client {
    ClientConfig own_config;ClientConfig* config=&own_config;Group group;Loop loop;
    Group* status_bits=&group;Loop* event_handle=&loop;void* transport=nullptr;void* task_handle=reinterpret_cast<void*>(2);
    bool run=true,selected_for_destroying=false,destroyed=false,read_in_progress=false;
    int state=WEBSOCKET_STATE_CONNECTED,last_fin=0,last_opcode=0,payload_len=0,payload_offset=0,close_status_code=0;
    int reconnect_tick_ms=0,wait_timeout_ms=2000;Error error_handle;char* errormsg_buffer=nullptr;size_t errormsg_size=0;
    unsigned id=0;
};
constexpr uint32_t STOPPED_BIT=1,REQUESTED_STOP_BIT=4;
#define ESP_WS_CLIENT_STATE_CHECK(tag,p,action) if((p)->state<WEBSOCKET_STATE_INIT){action;}
std::vector<std::unique_ptr<esp_websocket_client>> clients;
std::vector<std::string> trace;
std::atomic<uint32_t> notification{0};
int register_result=ESP_OK,start_result=ESP_OK;
size_t nodelay_calls=0,stream_sends=0,stream_capacity=65536;
std::function<void()> before_command;
std::vector<std::string> processed;
void* g_transport_task=reinterpret_cast<void*>(1);
void* xTaskGetCurrentTaskHandle(){return g_transport_task;}
int64_t esp_timer_get_time(){return 1000000;}
int _tick_get_ms(){return 1000;}
unsigned esp_get_free_heap_size(){return 0;}
const char* esp_err_to_name(int){return "host";}
void esp_transport_close(void*){}
void* esp_transport_get_error_handle(void*){return nullptr;}
int esp_transport_get_errno(void*){return 0;}
int esp_tls_get_and_clear_last_error(void*,int*,int*){return 0;}
int esp_event_post_to(Loop* loop,esp_event_base_t,int id,const void* data,size_t,TickType_t){loop->queue.emplace_back(id,*static_cast<const esp_websocket_event_data_t*>(data));return ESP_OK;}
int esp_event_loop_run(Loop* loop,int){while(!loop->queue.empty()){auto e=loop->queue.front();loop->queue.pop_front();loop->callback(loop->arg,WEBSOCKET_EVENTS,e.first,&e.second);}return ESP_OK;}
int esp_event_handler_register_with(Loop* loop,esp_event_base_t,int,esp_event_handler_t fn,void* arg){if(register_result!=ESP_OK)return register_result;loop->callback=fn;loop->arg=arg;return ESP_OK;}
uint32_t xEventGroupGetBits(Group* group){return group->bits;}
void xEventGroupSetBits(Group* group,uint32_t bits){group->bits|=bits;}
uint32_t xEventGroupWaitBits(Group*,uint32_t,bool,bool,TickType_t);
void destroy_and_free_resources(esp_websocket_client_handle_t client){trace.push_back("destroy:"+std::to_string(client->id));client->destroyed=true;}
void vTaskDelete(void*){}
@@SDK@@
void esp_websocket_free_buf(esp_websocket_client_handle_t,bool){}
@@STOPPED_RECV@@
void SdkWorkerUnwind(esp_websocket_client_handle_t client) {
    if(client->read_in_progress){trace.push_back("read-unwind:"+std::to_string(client->id));@@WORKER_READ@@}
    trace.push_back("finish:"+std::to_string(client->id));
@@WORKER_FINISH@@
uint32_t xEventGroupWaitBits(Group* group,uint32_t wanted,bool,bool,TickType_t) {
    if(!(group->bits&wanted))SdkWorkerUnwind(group->client);
    return group->bits;
}
struct esp_websocket_client_config_t {
    const char* uri=nullptr;const char* subprotocol=nullptr;const char* headers=nullptr;
    int network_timeout_ms=0,task_stack=0,buffer_size=0,reconnect_timeout_ms=0,ping_interval_sec=0;
    void* crt_bundle_attach=nullptr;
};
constexpr char WQN_VOICE_WS_SUBPROTOCOL[]="fixture";
void* esp_crt_bundle_attach=nullptr;
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t*) {
    clients.emplace_back(std::make_unique<esp_websocket_client>());auto* p=clients.back().get();
    p->id=clients.size();p->group.client=p;p->group.bits=STOPPED_BIT;
    trace.push_back("create:"+std::to_string(p->id));return p;
}
int esp_websocket_client_start(esp_websocket_client_handle_t p){if(start_result!=ESP_OK)return start_result;p->group.bits=0;return esp_websocket_client_dispatch_event(p,WEBSOCKET_EVENT_CONNECTED,nullptr,0);}
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t p){return p&&!p->destroyed&&p->state==WEBSOCKET_STATE_CONNECTED;}
int esp_websocket_client_set_tcp_nodelay(esp_websocket_client_handle_t,bool){++nodelay_calls;return ESP_OK;}
int esp_websocket_client_send_text(esp_websocket_client_handle_t,const char*,size_t,int){return 0;}
std::atomic<ConnectionState> g_conn_state{ConnectionState::kDisconnected};
std::atomic<TurnState> g_turn_state{TurnState::kIdle};
std::atomic<bool> g_is_connected{false},g_session_ready{false};
esp_websocket_client_handle_t g_ws_client=nullptr;
uint32_t g_active_conn_op_gen=0,g_active_turn_gen=0;
int64_t g_conn_deadline_ms=0;
char g_active_turn_req_id[64]={};
bool g_active_turn_asr_only=false;
wqn::stdpro_ws::TurnReleaseDeadline g_turn_release_deadline;
wqn::stdpro_ws::TurnStreamObservation g_active_turn_stream_obs{};
wqn::stdpro_ws::FinalHandoffResult g_final_result{};
portMUX_TYPE g_emergency_abort_mux;EmergencyAbortTarget g_emergency_abort_target;
std::string g_current_token,g_current_req_id,g_control_reassembly_buf,input;
void xTaskNotify(void*,uint32_t,int);
@@CALLBACK_STATE@@
struct Parser {void reset(){}} g_sse_parser;
void* g_text_stream=reinterpret_cast<void*>(3);void* g_pcm_queue=nullptr;void* g_final_sem=nullptr;
void* g_ctrl_queue=reinterpret_cast<void*>(4);
std::deque<CtrlCmd> commands;std::vector<CtrlCompletion> completions;
void PublishCompletion(uint32_t gen,int result,CmdDisposition disposition){completions.push_back({gen,result,disposition});}
std::vector<wqn::stdpro_ws::TurnReleaseState> terminal_releases;
void PublishTurnTerminalRecord(uint32_t,const char*,wqn::stdpro_ws::TurnReleaseState release,wqn::stdpro_ws::TurnStreamObservation){terminal_releases.push_back(release);}
void LogWsPhase(const char*,const char*,uint32_t,int){}
void LogTransportMemory(const char*,const char* =nullptr,uint32_t=0){}
void LogWsSocketEvent(const char*,const esp_websocket_event_data_t*){}
void xSemaphoreGive(void*){}
void xStreamBufferReset(void*){input.clear();}
size_t xStreamBufferSend(void*,const char* p,size_t n,int){++stream_sends;n=std::min(n,stream_capacity-input.size());input.append(p,n);return n;}
int xQueueReceive(void* queue,void* out,int){if(queue==g_ctrl_queue&&!commands.empty()){*static_cast<CtrlCmd*>(out)=commands.front();commands.pop_front();if(before_command){auto hook=std::move(before_command);before_command={};hook();}return pdTRUE;}return pdFALSE;}
void xTaskNotify(void*,uint32_t bits,int){notification|=bits;}
int xTaskNotifyWait(uint32_t,uint32_t clear,uint32_t* out,TickType_t){*out=notification.fetch_and(~clear);return *out?pdTRUE:pdFALSE;}
void ProcessTextStream(){processed.push_back(input);input.clear();}
@@TRANSPORT@@
@@CONNECT@@
@@STEP@@
size_t checks=0,failures=0;
void Check(bool okay,const char* text){++checks;if(!okay){++failures;std::fprintf(stderr,"FAIL %s\n",text);}}
void Reset() {
    if(g_ws_client){DoDestroyWs();}
    clients.clear();trace.clear();notification=0;commands.clear();completions.clear();
    processed.clear();terminal_releases.clear();before_command={};stream_capacity=65536;stream_sends=nodelay_calls=0;
    register_result=start_result=ESP_OK;g_active_turn_gen=0;g_active_turn_req_id[0]='\0';
    g_ws_client=nullptr;g_current_token.clear();g_active_conn_op_gen=0;g_conn_deadline_ms=0;
    g_conn_state=ConnectionState::kDisconnected;g_turn_state=TurnState::kIdle;g_is_connected=g_session_ready=false;
}
void Case(bool read_in_progress,bool pending_before_destroy) {
    Reset();
    CtrlCmd first={};first.type=CtrlCmdType::kConnect;first.gen=1;first.deadline_ms=10000;
    std::strcpy(first.token,"old-fixture-token");commands.push_back(first);
    OwnerStep();OwnerStep();auto* old=g_ws_client;
    g_active_conn_op_gen=0;completions.clear(); // Prior successful handshake state; parsing is a separate fixture.
    old->read_in_progress=read_in_progress;g_current_token="old-fixture-token";
    g_conn_state=ConnectionState::kReady;g_is_connected=g_session_ready=true;
    CtrlCmd cmd={};cmd.type=CtrlCmdType::kConnect;cmd.gen=42;cmd.deadline_ms=10000;
    std::strcpy(cmd.token,"new-fixture-token");commands.push_back(cmd);
    if(pending_before_destroy)before_command=[old]{trace.push_back("fault-before-destroy:"+std::to_string(old->id));esp_websocket_client_abort_connection(old,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT);};
    OwnerStep();auto* fresh=g_ws_client;
    Check(old->destroyed && (old->group.bits&STOPPED_BIT),"old SDK worker is stopped/destroyed before the new client is used");
    Check(fresh && fresh!=old && !fresh->destroyed,"new client is created after old SDK lifecycle barrier");
    Check(g_active_conn_op_gen==42 && g_conn_state==ConnectionState::kConnecting,"new handshake is pending after the command step");
    OwnerStep();
    Check(g_ws_client==fresh && fresh && !fresh->destroyed && g_active_conn_op_gen==42 &&
          g_conn_state==ConnectionState::kConnecting,"old stop notifications cannot cancel the new handshake");
    Check(completions.empty(),"new connect does not receive a cancellation from the old client");
    std::printf("read_in_progress=%d pending_before_destroy=%d trace:",read_in_progress?1:0,pending_before_destroy?1:0);for(const auto& t:trace)std::printf(" %s",t.c_str());std::puts("");
}
void Fresh() {
    Reset();CtrlCmd cmd={};cmd.type=CtrlCmdType::kConnect;cmd.gen=42;cmd.deadline_ms=10000;
    std::strcpy(cmd.token,"fixture-token");commands.push_back(cmd);OwnerStep();OwnerStep();
    Check(g_is_connected && g_ws_client && !g_ws_client->destroyed,"current CONNECTED callback still reaches the owner");
    g_active_conn_op_gen=0;g_conn_state=ConnectionState::kReady;g_session_ready=true;completions.clear();
}
void Data(esp_websocket_client_handle_t client,const char* text) {
    client->last_opcode=1;esp_websocket_client_dispatch_event(client,WEBSOCKET_EVENT_DATA,text,std::strlen(text));
}
void CurrentFailures() {
    for(int event:{int(WEBSOCKET_EVENT_ERROR),int(WEBSOCKET_EVENT_DISCONNECTED)}) {
        Fresh();g_active_turn_gen=99;std::strcpy(g_active_turn_req_id,"turn-99");g_turn_state=TurnState::kRemoteHandoff;
        Data(g_ws_client,"current-wire");esp_websocket_client_dispatch_event(g_ws_client,static_cast<esp_websocket_event_id_t>(event),nullptr,0);
        OwnerStep();
        Check(processed==std::vector<std::string>{"current-wire"},"current text is processed before its coalesced terminal fault");
        Check(g_ws_client==nullptr && terminal_releases==std::vector<wqn::stdpro_ws::TurnReleaseState>{wqn::stdpro_ws::TurnReleaseState::kTransportLost},
              "current fault still closes and terminalizes the matching handoff once");
    }
    Fresh();stream_capacity=1;Data(g_ws_client,"too-large-for-port");OwnerStep();
    Check(g_ws_client==nullptr,"current callback stream overflow still requests transport reset");
    for(uint32_t bits:{kEvtBitResetRequired,kEvtBitEmergencyDisconnect}) {
        Fresh();xTaskNotify(g_transport_task,bits|kEvtBitPcmPending|kEvtBitCtrlPending,eSetBits);OwnerStep();
        Check(g_ws_client==nullptr,"non-callback PCM reset and emergency disconnect retain their meaning");
    }
}
#if HAS_CALLBACK_CONTEXT
void BindingCases() {
    Fresh();auto context=g_ws_callback_context;
    esp_websocket_client foreign;
    esp_websocket_event_data_t event;event.client=&foreign;
    const auto calls=nodelay_calls;
    WebsocketEventHandler(&context,WEBSOCKET_EVENTS,WEBSOCKET_EVENT_CONNECTED,&event);
    Check(notification==0 && nodelay_calls==calls,"foreign client cannot post events or change socket options");
    event.client=g_ws_client;
    WebsocketEventHandler(nullptr,WEBSOCKET_EVENTS,WEBSOCKET_EVENT_ERROR,&event);
    WebsocketEventHandler(&context,WEBSOCKET_EVENTS,WEBSOCKET_EVENT_ERROR,nullptr);
    Check(notification==0,"null callback provenance is ignored");
    RetireWsEvents();Data(g_ws_client,"retired-data");
    Check(stream_sends==0 && notification==0 && input.empty(),"retired callback cannot append data before join completes");
    Fresh();g_is_connected=false;
    xTaskNotify(g_transport_task,kEvtBitWsConnected|kEvtBitWsDisconnected|kEvtBitTextData,eSetBits);OwnerStep();
    Check(g_ws_client && !g_ws_client->destroyed && !g_is_connected,"raw wake bits without active mailbox payload cannot mutate a new client");
    for(int failure:{1,2}) {
        Reset();if(failure==1)register_result=ESP_ERR_NO_MEM;else start_result=ESP_FAIL;
        CtrlCmd cmd={};cmd.type=CtrlCmdType::kConnect;cmd.gen=99;cmd.deadline_ms=10000;commands.push_back(cmd);OwnerStep();
        Check(g_ws_client==nullptr && completions.size()==1 && completions[0].op_gen==99 && completions[0].result!=ESP_OK,
              "registration/start failure retires callback state and completes only its own connect");
        Check(TakeWsEvents()==0,"failed startup leaves no callback payload for a future connection");
    }
    wqn::stdpro_ws::WsEventMailbox mailbox;
    Check(!mailbox.Post(0,1) && mailbox.Take()==0,"inactive mailbox refuses events");
    auto old=mailbox.Begin();Check(old!=0 && mailbox.Post(old,1) && mailbox.Post(old,2) && mailbox.Take()==3 && mailbox.Take()==0,
          "same-lifetime event bits coalesce and consume once");
    mailbox.Post(old,4);mailbox.Retire();auto next=mailbox.Begin();
    Check(next!=old && next!=0 && !mailbox.Post(old,8) && mailbox.Take()==0,"new lifetime discards old backlog and rejects old generation");
    Check(mailbox.Post(next,16) && mailbox.Take()==16,"new generation remains usable after a stale post");
    for(unsigned i=0;i<20;++i) {
        Fresh();const auto producer_context=g_ws_callback_context;std::atomic<bool> started{false};
        std::thread producer([&]{PostWsEvent(&producer_context,kEvtBitWsError);started=true;for(int j=0;j<5000;++j)PostWsEvent(&producer_context,kEvtBitWsError);});
        while(!started.load())std::this_thread::yield();
        RetireWsEvents();producer.join();
        Check(TakeWsEvents()==0,"concurrent post/retire leaves no retired event payload");
    }
}
#endif
int main(){for(bool pending:{false,true})for(bool reading:{false,true})Case(reading,pending);CurrentFailures();
#if HAS_CALLBACK_CONTEXT
BindingCases();
#endif
Reset();std::printf("WS event lifetime: %zu checks, %zu failures; real SDK lifecycle/owner steps, host ports; NOT HIL\n",checks,failures);return failures?1:0;}
