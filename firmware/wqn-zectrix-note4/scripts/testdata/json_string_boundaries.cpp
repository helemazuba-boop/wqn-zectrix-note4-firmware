// Complete real domain parsers and SSE decoder in separate TUs. Only HTTP,
// token/readiness ports and log output are replaced for actual API functions.
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "wqn_api.h"
#include "sse_chunk.h"
#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"
#include "esp_check.h"

constexpr char kTag[]="json-boundary";
void HostLog(const char*,const char*,...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
const char* esp_err_to_name(int) { return "host"; }
std::string http_body;
int clear_token_calls=0;
esp_err_t ValidateTokenOrClear(const std::string&,const char*) { return ESP_OK; }
esp_err_t WaitForNetworkReadyForHttps() { return ESP_OK; }
std::string BuildUrl(const char* path) { return std::string("https://example.invalid")+path; }
void NormalizeProtocolHttpError(int,wqn::protocol::v3::Error*) {}
esp_err_t ClearTokenOnUnauthorized(const char*) { ++clear_token_calls; return ESP_ERR_INVALID_STATE; }
esp_err_t ClearAccessToken() { ++clear_token_calls; return ESP_OK; }
bool IsValidAccessToken(const std::string& token) { return !token.empty(); }
@@SUCCESS@@
esp_err_t HttpRequest(const char*,const std::string&,const std::string*,const std::string*,
    int* status,std::string* body,const char*,const std::string*,uint32_t =0)
{ *status=200; *body=http_body; return ESP_OK; }
namespace wqn {
@@WORD_CALLER@@
@@BOOTSTRAP_CALLER@@
@@REPLY_CALLER@@
}
struct Case { const char* parser; const char* name; std::string request,body; bool valid; };
const std::vector<Case> cases={@@CASES@@};
size_t checks=0,failures=0;
void Check(bool pass,const std::string& name)
{
    ++checks;
    if (!pass) { if (failures<24) std::fprintf(stderr,"FAIL %s\n",name.c_str()); ++failures; }
}
esp_err_t Parse(const Case& c)
{
    namespace v3=wqn::protocol::v3;
    namespace word=wqn::protocol::word_study_v1;
    namespace note=wqn::protocol::note_study_v1;
    namespace problem=wqn::protocol::problem_study_v1;
    v3::Error error;
#define PARSE(name,type,fn) if (std::string(c.parser)==name) { type data; return fn(c.body,c.request,&data,&error); }
    PARSE("v3.claim-start",v3::ClaimStartData,v3::ParseClaimStartResponse)
    PARSE("v3.claim-poll",v3::ClaimPollData,v3::ParseClaimPollResponse)
    PARSE("v3.bootstrap",v3::BootstrapData,v3::ParseBootstrapResponse)
    PARSE("v3.sync",v3::SyncData,v3::ParseSyncResponse)
    PARSE("word.session",word::SessionData,word::ParseSessionResponse)
    PARSE("word.page",word::CandidatePageData,word::ParseCandidatePageResponse)
    PARSE("word.observation",word::ObservationData,word::ParseObservationResponse)
    PARSE("word.manifest",word::ManifestData,word::ParseManifestResponse)
    PARSE("note.session",note::SessionData,note::ParseSessionResponse)
    PARSE("note.page",note::CandidatePageData,note::ParseCandidatePageResponse)
    PARSE("note.observation",note::ObservationData,note::ParseObservationResponse)
    PARSE("note.manifest",note::ManifestData,note::ParseManifestResponse)
    PARSE("problem.manifest",problem::ManifestData,problem::ParseManifestResponse)
    PARSE("problem.observation",problem::ObservationData,problem::ParseObservationResponse)
#undef PARSE
    return ESP_ERR_INVALID_ARG;
}

// Real provisioning submit method; only HTTP receive/reply and credential
// persistence callback are ports. All SSIDs/passwords below are synthetic.
constexpr int HTTPD_400_BAD_REQUEST=400,HTTPD_500_INTERNAL_SERVER_ERROR=500;
constexpr int HTTPD_RESP_USE_STRLEN=-1,HTTPD_SOCK_ERR_TIMEOUT=-2;
struct httpd_req_t {
    std::string body,response;
    size_t content_len=0,offset=0,chunk=0;
    int status=200;
};
int httpd_req_recv(httpd_req_t* req,char* dest,size_t length)
{
    const auto count=std::min({length,req->body.size()-req->offset,req->chunk});
    std::memcpy(dest,req->body.data()+req->offset,count); req->offset+=count;
    return static_cast<int>(count);
}
esp_err_t httpd_resp_send_err(httpd_req_t* req,int status,const char* text)
{ req->status=status; req->response=text; return ESP_OK; }
esp_err_t httpd_resp_send_408(httpd_req_t* req) { req->status=408; return ESP_OK; }
void httpd_resp_set_type(httpd_req_t*,const char*) {}
void httpd_resp_set_hdr(httpd_req_t*,const char*,const char*) {}
esp_err_t httpd_resp_send(httpd_req_t* req,const char* text,int) { req->response=text; return ESP_OK; }
namespace wqn::provision {
constexpr char kTag[]="portal-boundary";
constexpr size_t kMaxSubmitBodySize=512,kMaxJsonNestingDepth=16;
@@PORTAL_HELPER@@
enum class NetworkRole { kPrimary,kBackup };
class WifiProvisionPortal {
public:
    std::function<esp_err_t(NetworkRole,const std::string&,const std::string&,bool)> credentials_callback_;
    std::mutex mutex_;
    std::string primary_ssid_,backup_ssid_;
    bool credentials_saved_=false;
    esp_err_t HandleSubmit(httpd_req_t*);
};
@@PORTAL_SUBMIT@@
}
void CheckPortal()
{
    for(size_t chunk: {size_t(1),size_t(17),size_t(512)}) {
        for(const std::string& bad: {
            std::string(R"({"ssid":"test\u0000shadow","password":"pw","role":"primary"})"),
            std::string(R"({"ssid\u0000shadow":"test","password":"pw","role":"primary"})"),
            std::string(R"({"ssid":"test","password":"pw\u0000shadow","role":"primary"})"),
            std::string(R"({"ssid":"test","password":"pw","role":"primary\u0000shadow"})"),
            std::string(R"({"ssid":"test","password":"pw","role\u0000shadow":"primary"})"),
            std::string(R"({"ssid":"test","role":"primary"})")+'\0'+"tail"}) {
            wqn::provision::WifiProvisionPortal portal; unsigned saves=0;
            portal.credentials_callback_=[&](auto,const auto&,const auto&,bool) { ++saves; return ESP_OK; };
            httpd_req_t req; req.body=bad; req.content_len=bad.size(); req.chunk=chunk;
            Check(portal.HandleSubmit(&req)==ESP_OK && req.status==400 && saves==0 &&
                  !portal.credentials_saved_ && portal.primary_ssid_.empty(),
                  "portal rejects NUL keys/values/raw tail before credential save");
        }
        for(const std::string& ssid: {std::string("中文网络"),std::string(R"(literal\u0000)")}) {
            wqn::provision::WifiProvisionPortal portal; unsigned saves=0;
            portal.credentials_callback_=[&](auto role,const auto& name,const auto& password,bool keep) {
                Check(role==wqn::provision::NetworkRole::kPrimary && name==ssid && password=="synthetic" && !keep,
                      "portal callback preserves representable SSID/password/role");
                ++saves; return ESP_OK;
            };
            cJSON* doc=cJSON_CreateObject(); cJSON_AddStringToObject(doc,"ssid",ssid.c_str());
            cJSON_AddStringToObject(doc,"password","synthetic"); cJSON_AddStringToObject(doc,"role","primary");
            char* wire=cJSON_PrintUnformatted(doc); httpd_req_t req; req.body=wire;
            cJSON_free(wire); cJSON_Delete(doc); req.content_len=req.body.size(); req.chunk=chunk;
            Check(portal.HandleSubmit(&req)==ESP_OK && req.status==200 && saves==1 &&
                  portal.credentials_saved_ && portal.primary_ssid_==ssid &&
                  req.response.find("\"success\":true")!=std::string::npos,"portal valid credentials still saved");
        }
    }
}
int main()
{
    CheckPortal();
    for (const auto& c:cases) {
        Check((Parse(c)==ESP_OK)==c.valid,c.name);
        if (std::string(c.parser)=="word.manifest" || std::string(c.parser)=="v3.bootstrap") {
            wqn::protocol::v3::RequestMetadata metadata;
            metadata.request_id=c.request; metadata.boot_id="boot_probe_000001"; metadata.firmware_version="host-probe";
            http_body=c.body;
            if (std::string(c.parser)=="word.manifest") {
                wqn::WqnWordPackManifest manifest; manifest.cursor=999; manifest.packs.resize(1);
                const auto result=wqn::FetchWordPackManifest("synthetic-token",metadata,0,&manifest);
                Check((result==ESP_OK)==c.valid,c.name+std::string(" actual HTTP result delivery"));
                Check(c.valid ? manifest.cursor==17 && manifest.packs.size()==1 : manifest.cursor==0 && manifest.packs.empty(),
                      c.name+std::string(" published manifest identity/cursor"));
            } else {
                wqn::protocol::v3::BootstrapData data; wqn::protocol::v3::Error error;
                const auto result=wqn::BootstrapDeviceControlV3("synthetic-token",metadata,&data,&error);
                Check((result==ESP_OK)==c.valid,c.name+std::string(" actual HTTP bootstrap result"));
                Check(c.valid ? !data.device_id.empty() : data.device_id.empty(),c.name+std::string(" published bootstrap identity"));
            }
        }
    }
    for (const char* wire: {R"({"request_id":"req\u0000shadow"})",R"({"request_id\u0000shadow":"req"})"}) {
        wqn::WqnAiSseEvent event;
        Check(!wqn::DecodeSseEvent("turn.released",1,wire,&event),"SSE release cannot normalize a NUL identity/key");
    }
    wqn::WqnAiSseEvent event;
    Check(wqn::DecodeSseEvent("asr.delta",2,R"({"delta":"literal\\u0000"})",&event) && event.delta==R"(literal\u0000)",
          "SSE literal backslash-u0000 survives unchanged");
    Check(wqn::DecodeSseEvent("asr.delta",3,R"({"delta":"\u005cu0000"})",&event) && event.delta==R"(\u0000)",
          "escaped Unicode backslash does not create a second Unicode escape");
    for (const char* wire: {R"({"success\u0000shadow":true})",R"({"success":true,"message":"x\u0000y"})"}) {
        http_body=wire; std::string detail;
        Check(wqn::PostAiInteractionReply("synthetic-token","11111111-1111-4111-8111-111111111111","yes",&detail)!=ESP_OK && !detail.empty(),
              "actual interaction reply never confirms a truncated response field");
    }
    http_body=R"({"success":true})"; std::string detail;
    Check(wqn::PostAiInteractionReply("synthetic-token","11111111-1111-4111-8111-111111111111","yes",&detail)==ESP_OK && detail.empty(),
          "normal interaction reply still confirms");
    const auto input_guard=[](const char* bytes,size_t length) {
#if HAS_JSON_INPUT_GUARD
        return wqn::protocol::JsonInputWithinLimits(bytes,length);
#else
        return wqn::protocol::JsonNestingWithinLimit(bytes,length);
#endif
    };
    for(size_t offset: {size_t(0),size_t(1),size_t(15),size_t(511),size_t(1024)}) {
        std::string bytes(offset,' '); bytes+='\0'; bytes+="{}";
        Check(!input_guard(bytes.data(),bytes.size()),"bounded raw NUL at any offset rejected before cJSON");
    }
    for(unsigned slash_count=1;slash_count<=8;++slash_count) {
        const auto bytes=std::string("{\"s\":\"")+std::string(slash_count,'\\')+"u0000\"}";
        Check(input_guard(bytes.data(),bytes.size())==(slash_count%2==0),"JSON escape parity distinguishes NUL and literal backslashes");
    }
    Check(input_guard(nullptr,0) && !input_guard(nullptr,1),"guard null span bounds are explicit");
    Check(clear_token_calls==0,"malformed HTTP 200 does not clear pairing state");
    std::printf("JSON string boundaries: %zu protocol cases, %zu checks, %zu failures; real parsers/API/SSE + host HTTP; NOT HIL\n",cases.size(),checks,failures);
    return failures?1:0;
}
