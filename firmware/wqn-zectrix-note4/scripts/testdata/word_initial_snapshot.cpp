#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "device_protocol/word_study.h"
constexpr int ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_INVALID_STATE=3;
constexpr int ESP_ERR_INVALID_RESPONSE=4, ESP_ERR_INVALID_SIZE=5, ESP_ERR_TIMEOUT=6;
constexpr char kTag[]="host";
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(call, ...) do { const auto e=(call); if(e!=ESP_OK) return e; } while(false)
const char* esp_err_to_name(int e) { return e==ESP_OK?"ESP_OK":"host-error"; }
int64_t esp_timer_get_time() { static int64_t ticks=0; return ++ticks; }
@@API_LIMITS@@
@@LIMITS@@
namespace wqn {
template<typename T> using WordStorePsramAllocator=std::allocator<T>;
enum class WordPresentationPhase : uint8_t { kFront=0,kBack=1 };
struct WordPackIndex {};
@@STRUCTS@@
}
using S=wqn::PersistedWordSession;
using Page=wqn::protocol::word_study_v1::CandidatePageData;
using Request=wqn::protocol::word_study_v1::CandidatePageRequest;
using Error=wqn::protocol::v3::Error;
@@CODEC@@
bool token_valid=true, clock_ready=true;
int network_result=ESP_OK, http_result=ESP_OK, status=200, parse_result=ESP_OK;
int save_result=ESP_OK, build_result=ESP_OK;
int http_calls=0, saves=0, token_clears=0, long_readiness=0, short_readiness=0, timeout_used=0;
Page page_reply;
Error protocol_reply;
S saved;
Request captured_request;
std::vector<std::string> calls;
std::string BuildUrl(const std::string& path) { return path; }
bool IsClockReasonable() { return clock_ready; }
int WaitForNetworkReadyForHttps() { ++long_readiness; return network_result; }
int ValidateTokenOrClear(const std::string&, const char*) { return ESP_OK; }
int ClearTokenOnUnauthorized(const char*) { ++token_clears; token_valid=false; return ESP_ERR_INVALID_STATE; }
void NormalizeProtocolHttpError(int code,Error* e) { if(code==429 || code>=500) e->retryable=true; }
int HttpRequest(const char*,const std::string&,const std::string*,const std::string*,int* code,
    std::string* body,const char*,const std::string*,int timeout) {
    ++http_calls; timeout_used=timeout; *code=status; *body="host-body";
    calls.push_back("http"); return http_result;
}
bool LoadValidTokenForTodo(std::string* token) { *token=token_valid?"host-not-a-real-token":""; return token_valid; }
namespace wqn::services {
int WaitForConnectivity(int ticks) { if(ticks!=0) return ESP_FAIL; ++short_readiness; return network_result; }
protocol::v3::RequestMetadata MakeDeviceRequestMetadata() {
    protocol::v3::RequestMetadata metadata; metadata.request_id="host-page-id"; return metadata;
}
}
namespace wqn::protocol::word_study_v1 {
@@POLICY_NAME@@
int BuildCandidatePageRequest(const Request& request,std::string* body) {
    captured_request=request; *body="host-request"; return build_result;
}
int ParseCandidatePageResponse(const std::string&,const std::string&,Page* page,Error* error) {
    *page=page_reply; *error=protocol_reply; return parse_result;
}
}
namespace wqn {
@@EXTEND@@
int SavePersistedWordSession(const S& snapshot) {
    ++saves; saved=snapshot; calls.push_back("save"); return save_result;
}
@@API@@
}
namespace device_ui_internal {
@@RESULT@@
@@COALESCE@@
}
using Result=device_ui_internal::WordCloudResult;
int passes=0,failures=0;
void Check(bool okay,const char* name) { std::printf("%s: %s\n",okay?"PASS":"FAIL",name); okay?++passes:++failures; }
void Reset() {
    token_valid=true; clock_ready=true; network_result=ESP_OK; http_result=ESP_OK;
    status=200; parse_result=ESP_OK; save_result=ESP_OK; build_result=ESP_OK;
    http_calls=0; saves=0; token_clears=0; long_readiness=0; short_readiness=0; timeout_used=0;
    saved={}; protocol_reply={}; captured_request={}; calls.clear();
}
Result Initial(size_t count=32) {
    Result r; r.result=ESP_OK; auto& s=r.persisted_session; auto& remote=s.remote;
    s.active=true; s.deck_scope_generation=2; s.start_index=7;
    remote.session_id="aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    remote.seed="snapshot-seed"; remote.cursor="initial-cursor"; remote.has_more=true;
    remote.mode=wqn::protocol::word_study_v1::Mode::kReview;
    remote.ordering=wqn::protocol::word_study_v1::Ordering::kDueQueueV1;
    remote.progress_revision=8;
    wqn::StoredWordPackSnapshot snapshot;
    std::snprintf(snapshot.deck_id,sizeof(snapshot.deck_id),"%s","bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
    std::snprintf(snapshot.sha256,sizeof(snapshot.sha256),"%s",std::string(64,'c').c_str());
    snapshot.content_revision=4; snapshot.pack_revision=5; remote.snapshot.push_back(snapshot);
    wqn::StoredWordDeckId deck;
    std::snprintf(deck.value,sizeof(deck.value),"%s",snapshot.deck_id); remote.deck_ids.push_back(deck);
    for(size_t i=0;i<count;++i) {
        wqn::StoredWordSessionItem item;
        std::snprintf(item.item_id,sizeof(item.item_id),"%08u-aaaa-aaaa-aaaa-aaaaaaaaaaaa",static_cast<unsigned>(i));
        std::snprintf(item.deck_id,sizeof(item.deck_id),"%s",snapshot.deck_id);
        item.ordinal=i; remote.items.push_back(item);
    }
    page_reply={}; page_reply.session_id=remote.session_id; page_reply.ordering=remote.ordering;
    page_reply.candidate_policy_version=wqn::protocol::word_study_v1::CandidatePolicyVersionName(remote.ordering);
    page_reply.seed=remote.seed; page_reply.progress_revision=remote.progress_revision;
    page_reply.cursor=remote.cursor; page_reply.next_cursor="extended-cursor"; page_reply.has_more=true;
    wqn::protocol::word_study_v1::PackSnapshot pack;
    pack.deck_id=snapshot.deck_id; pack.sha256=snapshot.sha256;
    pack.content_revision=4; pack.pack_revision=5; page_reply.snapshot.push_back(pack);
    for(size_t i=count;i<count+64;++i) {
        wqn::protocol::word_study_v1::SessionItem item;
        char id[37]; std::snprintf(id,sizeof(id),"%08u-aaaa-aaaa-aaaa-aaaaaaaaaaaa",static_cast<unsigned>(i));
        item.item_id=id; item.deck_id=snapshot.deck_id; item.ordinal=i; page_reply.items.push_back(item);
    }
    return r;
}
void Run(Result& r) { device_ui_internal::PersistInitialWordSessionSnapshot("host-not-a-real-token",&r); }
int main() {
    Reset(); Result r=Initial(); std::vector<uint8_t> base_bytes,final_bytes;
    Check(EncodeSession(r.persisted_session,&base_bytes),"initial snapshot uses the actual encoder");
    Run(r);
    Check(saves==1 && saved.remote.items.size()==96 && r.persisted_session.remote.items.size()==96,
        "32 initial plus 64 prefetch candidates reach one final snapshot save");
    Check(calls==std::vector<std::string>({"http","save"}),"no intermediate snapshot is saved before page validation");
    Check(EncodeSession(saved,&final_bytes) && final_bytes.size()>base_bytes.size() && final_bytes.size()>8000,
        "coalesced state contains actual multi-KB encoded payload, not equal-state suppression");
    Check(saved.remote.cursor=="extended-cursor" && saved.start_index==7 && saved.deck_scope_generation==2 &&
        saved.remote.session_id==page_reply.session_id && saved.remote.items[32].ordinal==32,
        "extended cursor/order/SID/start index/scope retained");
    Check(captured_request.limit==64 && captured_request.cursor=="initial-cursor" &&
        !captured_request.metadata.request_id.empty(),"existing page cursor/limit and fresh request identity used");
    Check(short_readiness==1 && long_readiness==0 && timeout_used==1000 && http_calls==1,
        "opportunistic page uses zero readiness wait and short socket timeout, only once");

    for(unsigned mode=0;mode<7;++mode) {
        Reset(); r=Initial();
        r.persisted_session.remote.mode=static_cast<wqn::protocol::word_study_v1::Mode>(mode);
        Run(r);
        Check(saves==1 && saved.remote.items.size()==96 && static_cast<unsigned>(saved.remote.mode)==mode,
            "all seven mode identities survive the same bounded coalescing path");
    }
    Reset(); r=Initial(); page_reply.items.clear(); page_reply.has_more=false; page_reply.next_cursor.clear(); Run(r);
    Check(saves==1 && saved.remote.items.size()==32 && !saved.remote.has_more && saved.remote.cursor.empty(),
        "valid empty terminal page publishes one final control state, not a second snapshot");
    Reset(); r=Initial(); page_reply.items.resize(7); page_reply.has_more=false; Run(r);
    Check(saves==1 && saved.remote.items.size()==39 && !saved.remote.has_more,
        "short last page is coalesced without inventing further candidates");

    Reset(); r=Initial(); save_result=ESP_FAIL; Run(r);
    Check(r.session_persist_result==ESP_FAIL && saves==1 && saved.remote.items.size()==96,
        "final save failure stays visible without fallback write or false saved state");
    Reset(); r=Initial(); save_result=ESP_ERR_INVALID_STATE; Run(r);
    Check(r.session_persist_result==ESP_ERR_INVALID_STATE && saves==1 && saved.deck_scope_generation==2,
        "store scope/fence rejection propagated unchanged");
    for(int code:{429,503}) {
        Reset(); r=Initial(); status=code; Run(r);
        Check(r.result==ESP_OK && r.session_persist_result==ESP_OK && saves==1 && saved.remote.items.size()==32 &&
            token_clears==0 && token_valid,"HTTP 429/5xx falls back to original valid window, keeps identity");
    }
    Reset(); r=Initial(); http_result=ESP_ERR_TIMEOUT; Run(r);
    Check(saves==1 && saved.remote.items.size()==32 && r.result==ESP_OK && token_clears==0,
        "transport timeout does not lose initial data or clear identity");
    Reset(); r=Initial(); network_result=ESP_ERR_TIMEOUT; Run(r);
    Check(saves==1 && saved.remote.items.size()==32 && http_calls==0 && long_readiness==0,
        "lost readiness never adds reconnect/SNTP wait and preserves initial state");
    Reset(); r=Initial(); clock_ready=false; Run(r);
    Check(saves==1 && saved.remote.items.size()==32 && http_calls==0 && long_readiness==0,
        "clock regression skips speculative HTTPS rather than starting SNTP");
    Reset(); r=Initial(); status=401; Run(r);
    Check(token_clears==1 && !token_valid && r.result==ESP_ERR_INVALID_STATE && saves==0,
        "real API 401 path is not hidden by initial-window fallback");
    for(const char* code:{"SESSION_NOT_FOUND","SESSION_NOT_ACTIVE","WORD_SESSION_SNAPSHOT_INCOMPLETE"}) {
        Reset(); r=Initial(); status=409; protocol_reply.code=code; Run(r);
        Check(r.result!=ESP_OK && saves==0 && r.protocol_error.code==code && token_clears==0,
            "server-invalid session is not persisted by opportunistic fallback");
    }
    for(int field=0;field<12;++field) {
        Reset(); r=Initial();
        switch(field) {
            case 0: page_reply.session_id="different"; break;
            case 1: page_reply.cursor="wrong-cursor"; break;
            case 2: ++page_reply.progress_revision; break;
            case 3: page_reply.seed="wrong-seed"; break;
            case 4: ++page_reply.snapshot[0].pack_revision; break;
            case 5: ++page_reply.items[0].ordinal; break;
            case 6: page_reply.items.resize(65); break;
            case 7: page_reply.ordering=wqn::protocol::word_study_v1::Ordering::kSequential; break;
            case 8: page_reply.candidate_policy_version="wrong-policy"; break;
            case 9: page_reply.snapshot[0].sha256=std::string(64,'d'); break;
            case 10: page_reply.items[0].item_id="invalid"; break;
            default: page_reply.items[0].deck_id="invalid"; break;
        }
        Run(r);
        Check(saves==1 && saved.remote.items.size()==32 && saved.remote.cursor=="initial-cursor",
            "actual page validator rejects identity/order/window mismatch without mutating initial state");
    }
    for(int guard=0;guard<7;++guard) {
        Reset(); r=Initial();
        switch(guard) {
            case 0: r.persisted_session.paused=true; break;
            case 1: r.persisted_session.position=1; break;
            case 2: r.persisted_session.phase=wqn::WordPresentationPhase::kBack; break;
            case 3: r.persisted_session.remote.items.clear(); break;
            case 4: r.persisted_session.remote.items.resize(33); break;
            case 5: r.persisted_session.remote.has_more=false; break;
            default: r.persisted_session.remote.cursor.clear(); break;
        }
        Run(r); Check(http_calls==0 && saves==1,"ineligible/advanced window never prefetches or rewrites its control state");
    }
    for(int guard=0;guard<3;++guard) {
        Reset(); r=Initial();
        if(guard==0) r.result=ESP_FAIL;
        else if(guard==1) r.session_compact_result=ESP_ERR_INVALID_SIZE;
        else r.persisted_session.active=false;
        Run(r); Check(http_calls==0 && saves==0,"failed create/compaction or inactive session has no save");
    }
    Reset(); device_ui_internal::PersistInitialWordSessionSnapshot("",nullptr);
    Check(saves==0 && http_calls==0,"null result is harmless");
    Reset(); r=Initial(); Request request; request.cursor="regular-cursor"; Page page; Error error;
    Check(wqn::FetchWordStudyCandidatePageV1("host",r.persisted_session.remote.session_id,request,&page,&error)==ESP_OK &&
        timeout_used==10000 && long_readiness==1 && short_readiness==0,
        "existing normal page API keeps original readiness and timeout semantics");
    Reset(); r=Initial(); build_result=ESP_ERR_INVALID_ARG; Run(r);
    Check(http_calls==0 && saves==1 && saved.remote.items.size()==32,"request build failure retains valid initial snapshot");
    Reset(); r=Initial(); parse_result=ESP_ERR_INVALID_RESPONSE; Run(r);
    Check(saves==1 && saved.remote.items.size()==32 && token_clears==0,"response parse failure does not publish malformed extension");
    std::printf("%d PASS / %d FAIL (production coalescer/API/page validator/codec; host seams, NOT HIL)\n",passes,failures);
    return failures?1:0;
}
