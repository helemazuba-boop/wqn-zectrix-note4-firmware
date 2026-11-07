// Actual storage functions with NVS ports; no flash, credentials or devices.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>
#include "storage.h"
#include "device_protocol/word_study.h"
using nvs_handle_t = unsigned;
constexpr int NVS_READONLY=0, NVS_READWRITE=1;
enum nvs_type_t {NVS_TYPE_U32=4,NVS_TYPE_U64=8,NVS_TYPE_STR=33,NVS_TYPE_BLOB=66,NVS_TYPE_ANY=255};
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102, ESP_ERR_NVS_TYPE_MISMATCH=0x1103;
constexpr char WQN_NVS_NAMESPACE[]="wqn";
constexpr char kTag[]="maintenance-port";
void Log(const char*,const char*,...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr,...) do { const auto e=(expr); if(e!=ESP_OK)return e; } while(false)
struct Value {
    std::vector<uint8_t> bytes;
    esp_err_t error=ESP_OK, read_error=ESP_OK;
    bool change_size=false;
    nvs_type_t type=NVS_TYPE_BLOB;
};
std::map<std::string,Value> values;
bool owner=true, namespace_present=true, scope_present=true, pending=false, cache_drift=false;
esp_err_t open_error=ESP_OK, scope_error=ESP_OK, pending_error=ESP_OK;
uint64_t scope=7;
nvs_type_t scope_type=NVS_TYPE_U64,pending_type=NVS_TYPE_STR;
uint32_t cache=7;
unsigned opens=0, closes=0, write_opens=0, cache_reads=0, direct_output_writes=0;
std::vector<std::string> read_keys;
uintptr_t output_begin=0, output_end=0;
void CheckDestination(const void* p) {
    auto at=reinterpret_cast<uintptr_t>(p);
    if(p && at>=output_begin && at<output_end)++direct_output_writes;
}
esp_err_t nvs_open(const char* ns,int mode,nvs_handle_t* out) {
    ++opens;
    if(mode!=NVS_READONLY)++write_opens;
    if(std::strcmp(ns,WQN_NVS_NAMESPACE)!=0) return ESP_FAIL;
    if(open_error!=ESP_OK)return open_error;
    if(!namespace_present)return ESP_ERR_NVS_NOT_FOUND;
    *out=1;return ESP_OK;
}
void nvs_close(nvs_handle_t) {++closes;}
esp_err_t nvs_find_key(nvs_handle_t,const char* key,nvs_type_t* out) {
    read_keys.push_back(key);CheckDestination(out);
    if(std::strcmp(key,"word_deck_gen")==0){
        if(scope_error!=ESP_OK)return scope_error;
        if(!scope_present)return ESP_ERR_NVS_NOT_FOUND;
        *out=scope_type;return ESP_OK;
    }
    if(std::strcmp(key,"word_deck_chg")==0){
        if(pending_error!=ESP_OK)return pending_error;
        if(!pending)return ESP_ERR_NVS_NOT_FOUND;
        *out=pending_type;return ESP_OK;
    }
    auto found=values.find(key);if(found==values.end())return ESP_ERR_NVS_NOT_FOUND;
    if(found->second.error!=ESP_OK)return found->second.error;
    *out=found->second.type;return ESP_OK;
}
esp_err_t nvs_get_u64(nvs_handle_t,const char* key,uint64_t* out) {
    read_keys.push_back(key);CheckDestination(out);
    if(std::strcmp(key,"word_deck_gen")!=0)return ESP_FAIL;
    if(scope_error!=ESP_OK)return scope_error;
    if(scope_type!=NVS_TYPE_U64)return ESP_ERR_NVS_NOT_FOUND;
    if(!scope_present)return ESP_ERR_NVS_NOT_FOUND;
    *out=scope;return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t,const char* key,char* out,size_t* size) {
    read_keys.push_back(key);CheckDestination(size);CheckDestination(out);
    if(std::strcmp(key,"word_deck_chg")!=0 || out!=nullptr)return ESP_FAIL;
    if(pending_error!=ESP_OK)return pending_error;
    if(pending_type!=NVS_TYPE_STR)return ESP_ERR_NVS_NOT_FOUND;
    if(!pending)return ESP_ERR_NVS_NOT_FOUND;
    *size=12;return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size) {
    read_keys.push_back(key);CheckDestination(out);CheckDestination(size);
    auto found=values.find(key);if(found==values.end())return ESP_ERR_NVS_NOT_FOUND;
    auto& value=found->second;
    if(value.type!=NVS_TYPE_BLOB)return ESP_ERR_NVS_NOT_FOUND;
    if(value.error!=ESP_OK)return value.error;
    if(!out){*size=value.bytes.size();return ESP_OK;}
    if(value.read_error!=ESP_OK)return value.read_error;
    const size_t actual=value.change_size && !value.bytes.empty()?value.bytes.size()-1:value.bytes.size();
    if(*size<actual)return ESP_ERR_INVALID_SIZE;
    if(actual)std::memcpy(out,value.bytes.data(),actual);
    *size=actual;return ESP_OK;
}
#include "runtime/nvs_typed_value.h"
namespace wqn::services {bool IsStorageServiceTask(){return owner;}}
@@LOOKUP@@
namespace wqn {
uint32_t GetDeckScopeGeneration() {return cache_drift && cache_reads++?cache+1:cache;}
@@CONSTANTS@@
@@HANDLE@@
@@LOAD@@
@@INSPECT@@
}
static_assert(std::is_trivially_copyable_v<wqn::WordRecoveryNvsSnapshot>);
static_assert(sizeof(wqn::WordRecoveryNvsSnapshot)<=1024);
unsigned passes=0, failures=0;
void Check(bool ok,const char* name) {
    std::printf("%s maintenance-nvs: %s\n",ok?"PASS":"FAIL",name);ok?++passes:++failures;
}
void Reset() {
    values.clear();owner=namespace_present=scope_present=true;pending=cache_drift=false;
    open_error=scope_error=pending_error=ESP_OK;scope=cache=7;
    scope_type=NVS_TYPE_U64;pending_type=NVS_TYPE_STR;
    opens=closes=write_opens=cache_reads=direct_output_writes=0;read_keys.clear();output_begin=output_end=0;
}
esp_err_t Inspect(wqn::WordRecoveryNvsSnapshot* out) {
    output_begin=reinterpret_cast<uintptr_t>(out);output_end=output_begin+sizeof(*out);
    const auto result=wqn::InspectWordRecoveryNvsOnStorageOwner(out);
    output_begin=output_end=0;return result;
}
int main() {
    wqn::WordRecoveryNvsSnapshot snapshot;
    Reset();owner=false;
    Check(Inspect(&snapshot)==ESP_ERR_INVALID_STATE && opens==0 && !snapshot.read_complete,
          "non-owner has no NVS side effects");
    Check(wqn::InspectWordRecoveryNvsOnStorageOwner(nullptr)==ESP_ERR_INVALID_ARG,"null output is rejected");
    Reset();namespace_present=false;
    Check(Inspect(&snapshot)==ESP_OK && snapshot.read_complete && !snapshot.namespace_found &&
        !snapshot.scope_found && snapshot.cursors[0].error==ESP_OK && !snapshot.cursors[0].found &&
        opens==1 && closes==0,"missing namespace remains explicit absence without creation");
    Check(write_opens==0,"inspection uses NVS_READONLY");
    Reset();open_error=ESP_FAIL;
    Check(Inspect(&snapshot)==ESP_FAIL && snapshot.namespace_error==ESP_FAIL &&
        !snapshot.read_complete && read_keys.empty(),"unreadable namespace is not absence");
    Reset();
    for(size_t i=0;i<7;++i) {
        const auto key=GetSessionCursorNvsKey(static_cast<wqn::protocol::word_study_v1::Mode>(i));
        Check(key!=nullptr,"producer and capture mode counts agree");
        if(key)values[key].bytes=std::vector<uint8_t>(52,static_cast<uint8_t>(i+1));
    }
    const auto before=values;
    Check(Inspect(&snapshot)==ESP_OK && snapshot.read_complete && snapshot.namespace_found &&
        snapshot.scope_found && snapshot.committed_scope_generation==7,"complete bounded raw snapshot");
    for(size_t i=0;i<7;++i)Check(snapshot.cursors[i].found && snapshot.cursors[i].stored_bytes==52 &&
        snapshot.cursors[i].copied_bytes==52 && snapshot.cursors[i].bytes[51]==i+1,"all seven known raw cursors are retained");
    Check(write_opens==0 && direct_output_writes==0 && closes==1,
          "NVS writes only internal scratch, then copies POD; handle closes");
    Check(std::set<std::string>(read_keys.begin(),read_keys.end())==std::set<std::string>{
        "word_deck_gen","word_deck_chg","cur_seq","cur_rnd","cur_dic","cur_rev","cur_int","cur_shf","cur_mis"},
        "only the exact recovery allowlist is queried");
    for(const auto& item:before)Check(values[item.first].bytes==item.second.bytes,"raw stored values unchanged");
    Reset();values["cur_seq"].bytes={};
    Check(Inspect(&snapshot)==ESP_OK && snapshot.cursors[0].found && snapshot.cursors[0].stored_bytes==0 &&
        snapshot.cursors[0].copied_bytes==0,"present empty blob is not absence or a decoded valid cursor");
    Reset();values["cur_seq"].bytes=std::vector<uint8_t>(53,0xA5);
    Check(Inspect(&snapshot)==ESP_OK && snapshot.cursors[0].found && snapshot.cursors[0].stored_bytes==53 &&
        snapshot.cursors[0].copied_bytes==53,"unknown small layout preserved without normalization");
    Reset();values["cur_seq"].bytes=std::vector<uint8_t>(65,0xA5);values["cur_mis"].bytes={1,2,3};
    Check(Inspect(&snapshot)==ESP_ERR_INVALID_SIZE && !snapshot.read_complete && snapshot.cursors[0].found &&
        snapshot.cursors[0].stored_bytes==65 && snapshot.cursors[0].copied_bytes==0,
          "oversize raw data is not silently truncated or declared captured");
    Check(snapshot.cursors[6].copied_bytes==3,"remaining independent evidence is still captured after one fault");
    Reset();values["cur_seq"].type=NVS_TYPE_U64;
    Check(Inspect(&snapshot)==ESP_ERR_NVS_TYPE_MISMATCH && !snapshot.read_complete &&
        snapshot.cursors[0].error==ESP_ERR_NVS_TYPE_MISMATCH,"wrong NVS type is explicit unknown evidence");
    Reset();values["cur_seq"].bytes=std::vector<uint8_t>(52,1);values["cur_seq"].read_error=ESP_FAIL;
    Check(Inspect(&snapshot)==ESP_FAIL && snapshot.cursors[0].found && snapshot.cursors[0].stored_bytes==52 &&
        snapshot.cursors[0].copied_bytes==0,"data read failure preserves existence and size but no invented bytes");
    Reset();values["cur_seq"].bytes=std::vector<uint8_t>(52,1);values["cur_seq"].change_size=true;
    Check(Inspect(&snapshot)==ESP_ERR_INVALID_SIZE && !snapshot.read_complete && snapshot.cursors[0].copied_bytes==0,
          "changed measured length cannot become a successful snapshot");
    Reset();scope=(UINT64_C(1)<<40)+7;pending=true;
    Check(Inspect(&snapshot)==ESP_OK && snapshot.committed_scope_generation==scope &&
        snapshot.cached_scope_generation_after==7 && snapshot.pending_marker_found && snapshot.pending_marker_bytes==12,
          "raw wide generation and pending marker remain visible without replay or truncation");
    Reset();scope_present=false;
    Check(Inspect(&snapshot)==ESP_OK && !snapshot.scope_found && snapshot.cached_scope_generation_after==7,
          "missing committed generation is not guessed from RAM");
    Reset();scope_type=NVS_TYPE_U32;
    Check(Inspect(&snapshot)==ESP_ERR_NVS_TYPE_MISMATCH && snapshot.scope_error!=ESP_OK,"scope type error is retained");
    Reset();pending_error=ESP_FAIL;
    Check(Inspect(&snapshot)==ESP_FAIL && snapshot.pending_marker_error==ESP_FAIL,"pending marker error is not cleared");
    Reset();cache_drift=true;
    Check(Inspect(&snapshot)==ESP_ERR_INVALID_STATE && !snapshot.read_complete,
          "cache generation drift invalidates one-transaction consistency");
    for(size_t length:{0U,51U,53U,64U,65U}) {
        Reset();values["cur_seq"].bytes=std::vector<uint8_t>(length,1);
        std::array<uint8_t,52> raw{};bool found=true;
        Check(wqn::LoadWordSessionCursorNvs("cur_seq",raw.data(),raw.size(),&found)==ESP_ERR_INVALID_SIZE && !found,
              "typed cursor reader refuses present wrong size instead of legacy fallback");
    }
    Reset();std::array<uint8_t,52> raw{};bool found=true;
    Check(wqn::LoadWordSessionCursorNvs("cur_seq",raw.data(),raw.size(),&found)==ESP_OK && !found,
          "typed cursor reader distinguishes genuine missing key");
    values["cur_seq"].bytes=std::vector<uint8_t>(52,9);
    Check(wqn::LoadWordSessionCursorNvs("cur_seq",raw.data(),raw.size(),&found)==ESP_OK && found && raw[51]==9,
          "typed cursor reader still accepts exact current layout");
    values["cur_seq"].change_size=true;found=true;
    Check(wqn::LoadWordSessionCursorNvs("cur_seq",raw.data(),raw.size(),&found)==ESP_ERR_INVALID_SIZE && !found,
          "typed cursor reader validates the second read length");
    std::printf("Storage maintenance NVS: %u checks, %u failures (host ports; NOT HIL)\n",passes+failures,failures);
    return failures?1:0;
}
