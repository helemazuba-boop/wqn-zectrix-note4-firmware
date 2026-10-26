#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ai_history.h"
#include "wqn_api.h"
#include "agent_round_policy.h"
#include "voice_diagnostics.h"
#include "esp_log.h"

void HostLog(const char*,const char*,...) {}
int checks=0;
#define CHECK(c) do { ++checks;if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();} } while(0)
int64_t now_ms=1000;
int64_t esp_timer_get_time() {return now_ms*1000;}
namespace wqn {
@@MODEL@@
}
namespace {
constexpr const char* kTag="std-stream-host";
constexpr size_t kMaxStreamingAnswerBytes=wqn::kMaxAgentTextBytes;
StaticSemaphore_t lock_storage;
auto g_lock=xSemaphoreCreateMutexStatic(&lock_storage);
wqn::AiSessionState g_state;
std::string g_current_turn_req_id="fixture",g_conversation_id,g_pending_tool_label;
uint32_t g_current_turn_gen=0;
bool g_reply_pending=false,g_streaming_force_full_render=false,g_streaming_active=true,g_changed=false;
int64_t g_tool_clear_at_ms=0;
@@TURN@@
@@CONSUMER@@
using Kind=wqn::WqnAiSseEvent::Kind;
uint64_t event_id=0;
void emit(Kind kind,const std::string& delta={},const std::string& text={}) {
    now_ms+=60;wqn::WqnAiSseEvent event;event.kind=kind;event.delta=delta;event.text=text;
    event.event_id=++event_id;event.tool_name="fixture-tool";event.tool_display="done";event.tool_ok=true;
    OnSseEvent(event);
}
void begin() {
    wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro).Clear();g_state={};ResetTurnAssemblyLocked();
    wqn::GetAiHistory(wqn::AiHistoryChannel::kStdPro).AppendUser("confirmed question",now_ms);
    g_turn.user_committed=true;g_state.status=wqn::AiSessionStatus::kStreaming;
}
std::vector<std::string> text_of(wqn::ChatMessageKind kind) {
    std::vector<std::string> result;
    for(const auto& message:wqn::GetAiHistorySnapshot(wqn::AiHistoryChannel::kStdPro)->messages)
        if(message.kind==kind)result.push_back(message.text);
    return result;
}
}
int main() {
    using M=wqn::ChatMessageKind;
    begin();emit(Kind::kThinkingStart);emit(Kind::kThinkingDelta,"first");
    CHECK(text_of(M::kThinking)==std::vector<std::string>{"first"});
    emit(Kind::kThinkingDelta," + second");
    CHECK(text_of(M::kThinking)==std::vector<std::string>{"first + second"}); // visible before done
    CHECK(g_state.last_render_ms==now_ms);
    emit(Kind::kThinkingDone,{},"authoritative first");
    emit(Kind::kTextStart);emit(Kind::kTextDelta,"preface");emit(Kind::kTextEnd,{},"preface");
    emit(Kind::kToolStart);emit(Kind::kToolResult);
    emit(Kind::kThinkingStart);emit(Kind::kThinkingDelta,"new round");
    CHECK(text_of(M::kThinking)==(std::vector<std::string>{"authoritative first","new round"}));
    emit(Kind::kThinkingDelta," live");
    CHECK(text_of(M::kThinking).back()=="new round live");
    emit(Kind::kThinkingDone,{},"new round done");
    emit(Kind::kTextStart);emit(Kind::kTextDelta,"final answer");
    CHECK(text_of(M::kAssistant)==(std::vector<std::string>{"preface","final answer"}));
    emit(Kind::kTextEnd,{},"final answer");
    g_state.scroll_offset_lines=-7;g_state.user_moved=true;
    emit(Kind::kFinal,{},"prefacefinal answer");
    CHECK(text_of(M::kAssistant)==(std::vector<std::string>{"preface","final answer"}));
    CHECK(g_state.scroll_offset_lines==-7&&g_state.status==wqn::AiSessionStatus::kReplyReady);
    CHECK(text_of(M::kThinking)==(std::vector<std::string>{"authoritative first","new round done"}));
    auto snapshot=wqn::GetAiHistorySnapshot(wqn::AiHistoryChannel::kStdPro);
    const std::vector<M> order={M::kUser,M::kThinking,M::kAssistant,M::kToolResult,M::kThinking,M::kAssistant};
    CHECK(snapshot->messages.size()==order.size());
    for(size_t i=0;i<order.size();++i)CHECK(snapshot->messages[i].kind==order[i]);

    // A tool-only final must not replay aggregate pre-tool narration.
    begin();emit(Kind::kTextStart);emit(Kind::kTextDelta,"already said");emit(Kind::kTextEnd,{},"already said");
    emit(Kind::kToolStart);emit(Kind::kToolResult);emit(Kind::kFinal,{},"already said");
    CHECK(text_of(M::kAssistant)==std::vector<std::string>{"already said"});
    // An authoritative-only completion remains supported.
    begin();emit(Kind::kFinal,{},"whole answer");
    CHECK(text_of(M::kAssistant)==std::vector<std::string>{"whole answer"});
    begin();emit(Kind::kTextStart);emit(Kind::kTextDelta,"before");emit(Kind::kTextEnd,{},"before");
    emit(Kind::kToolStart);emit(Kind::kToolResult);emit(Kind::kTextStart);
    emit(Kind::kTextEnd,{},"authoritative after");emit(Kind::kFinal,{},"beforeauthoritative after");
    CHECK(text_of(M::kAssistant)==(std::vector<std::string>{"before","authoritative after"}));
    // Chunk cadence coalesces history writes, but a terminal flush preserves it.
    begin();emit(Kind::kThinkingStart);emit(Kind::kThinkingDelta,"a");
    now_ms-=59;emit(Kind::kThinkingDelta,"b");CHECK(text_of(M::kThinking).back()=="a");
    emit(Kind::kFinal);CHECK(text_of(M::kThinking).back()=="ab");
    begin();emit(Kind::kThinkingStart);emit(Kind::kThinkingDelta,std::string(3000,'x'));
    CHECK(text_of(M::kThinking).back().size()==wqn::kMaxThinkingBytes);
    emit(Kind::kThinkingDone,{},std::string(3000,'y'));
    CHECK(text_of(M::kThinking).back().size()==wqn::kMaxThinkingBytes);
    begin();emit(Kind::kThinkingStart);std::string chinese;
    for(int i=0;i<800;++i)chinese+="中";
    emit(Kind::kThinkingDelta,chinese);
    CHECK(text_of(M::kThinking).back().size()==2046);
    CHECK(text_of(M::kThinking).back().substr(2043)=="中");
    begin();emit(Kind::kTextStart);emit(Kind::kTextDelta,"partial");
    now_ms-=59;emit(Kind::kTextDelta," tail");emit(Kind::kError,{},"failed");
    CHECK(text_of(M::kAssistant).back()=="partial tail");
    std::printf("STD production SSE/history: %d assertions passed\n",checks);
}
