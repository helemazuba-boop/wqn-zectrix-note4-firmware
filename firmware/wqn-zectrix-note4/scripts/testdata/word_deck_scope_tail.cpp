// Actual production helpers/tail with NVS and session-removal fault ports.
// Models per-key staged commits, not NOR/SPIFFS durability, scheduling or HIL.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=0x102;
constexpr int ESP_ERR_INVALID_STATE=0x103, ESP_ERR_INVALID_SIZE=0x104;
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102, ESP_ERR_NVS_TYPE_MISMATCH=0x1103;
constexpr int NVS_READONLY=0, NVS_READWRITE=1;
enum nvs_type_t {NVS_TYPE_U64=8,NVS_TYPE_STR=33,NVS_TYPE_ANY=255};
constexpr char WQN_NVS_NAMESPACE[]="wqn", kTag[]="scope-port";
void Log(const char*, const char*, ...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const auto e=(expr); if(e!=ESP_OK)return e; } while(false)
struct Value {
    bool string=false;
    uint64_t number=0;
    std::vector<char> bytes;
    bool operator==(const Value& other) const {
        return string==other.string && number==other.number && bytes==other.bytes;
    }
};
Value Text(const std::string& text) {
    Value value; value.string=true; value.bytes.assign(text.begin(), text.end());
    value.bytes.push_back('\0'); return value;
}
Value Number(uint64_t number) { Value value; value.number=number; return value; }
struct Handle { int mode; std::map<std::string,Value> writes; std::string erased; };
std::map<std::string,Value> durable;
std::map<nvs_handle_t,Handle> handles;
std::map<std::string,esp_err_t> read_faults;
std::vector<std::string> events;
std::vector<unsigned> sessions;
bool namespace_present=true, fail_after=false, shorten_read=false, storage_owner=true;
unsigned next_handle=1, mutation_count=0, fail_step=0, ro_opens=0, rw_opens=0;
bool BeforeMutation(const std::string& name) {
    events.push_back(name); ++mutation_count;
    return fail_step==mutation_count && !fail_after;
}
bool AfterMutation() { return fail_step==mutation_count && fail_after; }
esp_err_t ReadFault(const std::string& operation) {
    events.push_back(operation);
    const auto found=read_faults.find(operation);
    return found==read_faults.end()?ESP_OK:found->second;
}
esp_err_t nvs_open(const char* ns,int mode,nvs_handle_t* output) {
    if(std::strcmp(ns,WQN_NVS_NAMESPACE)!=0) return ESP_FAIL;
    mode==NVS_READONLY?++ro_opens:++rw_opens;
    auto result=ReadFault(mode==NVS_READONLY?"open:ro":"open:rw");
    if(result!=ESP_OK)return result;
    if(!namespace_present && mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;
    namespace_present=true; *output=next_handle++;
    handles.emplace(*output,Handle{mode,{}, {}}); return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { handles.erase(handle); }
esp_err_t nvs_find_key(nvs_handle_t,const char* key,nvs_type_t* output) {
    auto error=ReadFault(std::string("find_key:")+key);if(error!=ESP_OK)return error;
    auto found=durable.find(key);if(found==durable.end())return ESP_ERR_NVS_NOT_FOUND;
    *output=found->second.string?NVS_TYPE_STR:NVS_TYPE_U64;return ESP_OK;
}
esp_err_t nvs_get_u64(nvs_handle_t,const char* key,uint64_t* output) {
    auto error=ReadFault(std::string("get_u64:")+key); if(error!=ESP_OK)return error;
    auto found=durable.find(key);if(found==durable.end())return ESP_ERR_NVS_NOT_FOUND;
    if(found->second.string)return ESP_ERR_NVS_NOT_FOUND; // Actual SDK typed lookup folds the mismatch.
    *output=found->second.number;return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t,const char* key,char* output,size_t* length) {
    auto error=ReadFault(std::string(output?"read_str:":"measure_str:")+key);
    if(error!=ESP_OK)return error;
    auto found=durable.find(key);if(found==durable.end())return ESP_ERR_NVS_NOT_FOUND;
    if(!found->second.string)return ESP_ERR_NVS_NOT_FOUND;
    size_t size=found->second.bytes.size();
    if(output && shorten_read && size) --size;
    if(output && *length<size){*length=size;return ESP_ERR_INVALID_SIZE;}
    if(output && size)std::memcpy(output,found->second.bytes.data(),size);
    *length=size;return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle,const char* key,const char* text) {
    if(handles.at(handle).mode!=NVS_READWRITE)return ESP_FAIL;
    if(BeforeMutation(std::string("stage_str:")+key))return ESP_FAIL;
    handles.at(handle).writes[key]=Text(text);return AfterMutation()?ESP_FAIL:ESP_OK;
}
esp_err_t nvs_set_u64(nvs_handle_t handle,const char* key,uint64_t value) {
    if(handles.at(handle).mode!=NVS_READWRITE)return ESP_FAIL;
    if(BeforeMutation(std::string("stage_u64:")+key))return ESP_FAIL;
    handles.at(handle).writes[key]=Number(value);return AfterMutation()?ESP_FAIL:ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle,const char* key) {
    if(handles.at(handle).mode!=NVS_READWRITE)return ESP_FAIL;
    if(BeforeMutation(std::string("stage_erase:")+key))return ESP_FAIL;
    handles.at(handle).erased=key;return AfterMutation()?ESP_FAIL:ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    auto& pending=handles.at(handle);
    const std::string key=pending.erased.empty()?pending.writes.begin()->first:pending.erased;
    if(BeforeMutation("commit:"+key))return ESP_FAIL;
    for(const auto& pair:pending.writes)durable[pair.first]=pair.second;
    if(!pending.erased.empty())durable.erase(pending.erased);
    return AfterMutation()?ESP_FAIL:ESP_OK;
}
#include "runtime/nvs_typed_value.h"
namespace wqn::protocol::word_study_v1 {
enum class Mode:uint8_t{kSequential,kRandom,kDictionary,kReview,kIntake,kShuffle,kMistakes};
}
using Mode=wqn::protocol::word_study_v1::Mode;
esp_err_t ClearPersistedWordSession(Mode mode) {
    if(BeforeMutation("clear_session:"+std::to_string(static_cast<unsigned>(mode))))return ESP_FAIL;
    sessions.erase(std::remove(sessions.begin(),sessions.end(),static_cast<unsigned>(mode)),sessions.end());
    return AfterMutation()?ESP_FAIL:ESP_OK;
}
struct StorageWriteGuard {
    StorageWriteGuard(const char*,const char*,int) {}
    explicit operator bool() const {return true;}
};
namespace wqn::services {
bool IsStorageServiceTask() { return storage_owner; }
esp_err_t ExecuteStorageTransactionNamed(esp_err_t(*run)(void*),void* context,const char*) {
    return run(context);
}
esp_err_t ExecuteForegroundStorageTransaction(esp_err_t(*run)(void*),void* context,const char*) {
    return run(context);
}
}
std::atomic<uint32_t> g_deck_scope_generation{0};
constexpr const char* kDeckScopeMarkerKey="word_deck_chg";
constexpr const char* kDeckScopeGenKey="word_deck_gen";
constexpr const char* kDefaultWordDeckKey="word_deck";
constexpr const char* kWordSequentialCursorKey="word_seq_idx";
@@SCOPE_STATE@@
namespace wqn {
bool IsWordDeckScopeReady();
@@PRODUCTION@@
}
bool Ready() {
#if WQN_SCOPE_HAS_READY
    return wqn::IsWordDeckScopeReady();
#else
    return true;
#endif
}
unsigned checks=0, failures=0;
void Check(bool ok,const char* name) {
    ++checks;if(!ok)++failures;
    std::printf("%s scope-recovery: %s\n",ok?"PASS":"FAIL",name);
}
const std::string deck="11111111-2222-3333-4444-555555555555";
const std::string old_deck="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
void Reset(uint64_t generation=5) {
    durable={{"word_deck_gen",Number(generation)}, {"word_seq_idx",Number(77)},
        {"word_deck",Text(old_deck)}, {"unrelated",Text("opaque preserved bytes")}};
    handles.clear();read_faults.clear();events.clear();sessions={0,3,5,6};
    namespace_present=true;fail_after=shorten_read=false;
    storage_owner=true;
    next_handle=1;mutation_count=fail_step=ro_opens=rw_opens=0;
    g_deck_scope_generation.store(777);
#if WQN_SCOPE_HAS_READY
    g_deck_scope_ready.store(true);g_deck_scope_tail_active=false;
#endif
}
void RejectPreserving(const char* name) {
    const auto bytes=durable;const auto pins=sessions;const auto cache=g_deck_scope_generation.load();
    const auto result=wqn::RecoverDeckScopeTransaction(nullptr);
    Check(result!=ESP_OK && durable==bytes && sessions==pins &&
        g_deck_scope_generation.load()==cache && mutation_count==0 && rw_opens==0 && handles.empty(),name);
}
bool Installed(uint32_t generation,const std::string& target_deck) {
    return durable.at("word_deck_gen")==Number(generation) &&
        durable.at("word_seq_idx")==Number(0) && !durable.count("word_deck_chg") &&
        (target_deck.empty()?!durable.count("word_deck"):durable.at("word_deck")==Text(target_deck)) &&
        sessions.empty() && g_deck_scope_generation.load()==generation;
}
int main() {
    Reset();namespace_present=false;durable.clear();
    Check(wqn::RecoverDeckScopeTransaction(nullptr)==ESP_OK && durable.empty() &&
        g_deck_scope_generation.load()==0 && rw_opens==0,"missing namespace is normal absence without creation");
    for(uint64_t generation:{UINT64_C(0),UINT64_C(9),UINT64_C(4294967295)}) {
        Reset(generation);const auto bytes=durable;
        Check(wqn::RecoverDeckScopeTransaction(nullptr)==ESP_OK && durable==bytes &&
            g_deck_scope_generation.load()==generation && mutation_count==0,"known committed generation with no marker seeds cache only");
    }
    for(uint64_t generation:{UINT64_C(4294967296),UINT64_C(4294967301),UINT64_MAX}) {
        Reset(generation);RejectPreserving("wide committed generation is preserved without truncating cache");
    }
    for(const auto& marker:std::vector<std::string>{"", "garbage", "0|", "-1|", "+6|", " 6|", "6 |",
        "4|", "7|", "4294967296|", "4294967297|", "99999999999999999999|",
        "6|"+std::string(36,'x'), "6|11111111x2222-3333-4444-555555555555", "6|"+deck+"|",
        "6|"+deck+std::string("\0trailer",8)}) {
        Reset();durable["word_deck_chg"]=Text(marker);
        RejectPreserving("malformed or ambiguous marker remains byte-for-byte without replay");
    }
    Reset(6);durable["word_deck_chg"]=Text("6|"+deck);
    RejectPreserving("same-generation marker cannot overwrite a conflicting committed deck");
    for(const auto& operation:std::vector<std::string>{"measure_str:word_deck","read_str:word_deck"}) {
        Reset(6);durable["word_deck_chg"]=Text("6|"+deck);durable["word_deck"]=Text(deck);
        read_faults[operation]=ESP_FAIL;
        RejectPreserving("unreadable committed deck cannot authorize same-generation replay");
    }
    for(unsigned kind=0;kind<4;++kind) {
        Reset(6);durable["word_deck_chg"]=Text("6|"+deck);
        if(kind==0)durable.erase("word_deck");
        if(kind==1)durable["word_deck"]=Number(7);
        if(kind==2)durable["word_deck"]=Text(std::string(36,'x'));
        if(kind==3)durable["word_deck"]=Text(deck+std::string("\0tail",5));
        RejectPreserving("missing malformed or opaque committed deck remains untouched");
    }
    for(const auto& key:std::vector<std::string>{"word_deck_gen","word_deck_chg"}) {
        Reset();durable[key]=key=="word_deck_gen"?Text("future"):Number(99);
        RejectPreserving("wrong metadata type is not absence or permission to clear");
    }
    for(const auto& operation:std::vector<std::string>{"open:ro","get_u64:word_deck_gen",
        "measure_str:word_deck_chg","read_str:word_deck_chg"}) {
        Reset();durable["word_deck_chg"]=Text("6|"+deck);read_faults[operation]=ESP_FAIL;
        RejectPreserving("read fault propagates without publishing cache or mutating evidence");
    }
    for(const auto& invalid:std::vector<std::string>{"short",std::string(36,'x'),
        "11111111x2222-3333-4444-555555555555",std::string(36,'\0')}) {
        Reset();const auto bytes=durable;const auto pins=sessions;
        Check(wqn::ChangeDefaultWordDeckForeground(invalid)==ESP_ERR_INVALID_ARG &&
            durable==bytes && sessions==pins && mutation_count==0,"invalid requested deck is refused before marker/session writes");
    }
#if WQN_SCOPE_FULL_PORTS
    Reset();durable["word_deck_chg"]=Text("6|"+deck);shorten_read=true;
    RejectPreserving("measure/read length drift is not a complete marker");
    Reset();durable["word_deck_chg"]=Text("6|"+deck);durable["word_deck_chg"].bytes.back()='X';
    RejectPreserving("missing NVS string terminator is refused within measured bounds");
    Reset();durable["word_deck_chg"]=Text("");durable["word_deck_chg"].bytes.clear();
    RejectPreserving("zero-length NVS string is not absence");
    Reset();durable["word_deck_chg"]=Text(std::string(256,'6'));
    RejectPreserving("oversize marker is refused before allocating from its length");
#endif
    for(unsigned test=0;test<5;++test) {
        Reset(test==3?UINT32_MAX:test==1?6:test==2?0:5);
        if(test==2)durable.erase("word_deck_gen");
        const std::string target=test==4?"":deck;
        const uint32_t generation=test==2 || test==3?1:6;
        durable["word_deck_chg"]=Text(std::to_string(generation)+"|"+target);
        if(test==1)durable["word_deck"]=Text(target);
        Check(wqn::RecoverDeckScopeTransaction(nullptr)==ESP_OK && Installed(generation,target),
              "valid interrupted change completes including cursor reset and generation wrap");
        const auto bytes=durable;
        Check(wqn::RecoverDeckScopeTransaction(nullptr)==ESP_OK && durable==bytes && handles.empty(),
              "completed recovery is idempotent");
    }
    Reset(UINT32_MAX-1);
    const std::string uppercase="ABCDEF12-3456-789A-BCDE-F0123456789A";
    durable["word_deck_chg"]=Text("4294967295|"+uppercase);
    Check(wqn::RecoverDeckScopeTransaction(nullptr)==ESP_OK && Installed(UINT32_MAX,uppercase),
          "maximum 48-byte marker and uppercase UUID stay compatible on the u32 boundary");
    Reset();g_deck_scope_generation.store(5);durable["word_deck_chg"]=Text("uninterpretable pending bytes");
    {
        const auto bytes=durable;const auto pins=sessions;
        Check(wqn::ChangeDefaultWordDeckForeground(deck)!=ESP_OK && durable==bytes && sessions==pins &&
            mutation_count==0 && !Ready(),"live switch cannot overwrite unknown pending marker");
    }
    Reset(UINT64_C(4294967301));g_deck_scope_generation.store(5);
    {
        const auto bytes=durable;
        Check(wqn::ChangeDefaultWordDeckForeground(deck)!=ESP_OK && durable==bytes && mutation_count==0 && !Ready(),
              "live switch must revalidate committed metadata instead of trusting cache");
    }
    Reset();g_deck_scope_generation.store(5);durable["word_deck_chg"]=Text("6|"+deck);
    Check(wqn::ChangeDefaultWordDeckForeground(deck)==ESP_OK && Installed(6,deck) && Ready() &&
        std::find(events.begin(),events.end(),"stage_str:word_deck_chg")==events.end(),
        "same-target retry finishes original marker without minting a replacement");
    Reset();g_deck_scope_generation.store(5);durable["word_deck_chg"]=Text("6|"+deck);
    {
        const std::string replacement="99999999-8888-7777-6666-555555555555";
        const auto result=wqn::ChangeDefaultWordDeckForeground(replacement);
        const auto cleared=std::find(events.begin(),events.end(),"commit:word_deck_chg");
        const auto replaced=std::find(events.begin(),events.end(),"stage_str:word_deck_chg");
        Check(result==ESP_OK && Installed(7,replacement) && Ready() && cleared<replaced,
              "different target waits for old tail before writing the next marker");
    }
    Reset();g_deck_scope_generation.store(5);fail_step=11;
    Check(wqn::ChangeDefaultWordDeckForeground(deck)!=ESP_OK &&
        durable.at("word_deck_gen")==Number(6) && durable.count("word_deck_chg") && !Ready(),
        "failed accepted switch keeps scope unavailable after durable generation changed");
    {
        fail_step=0;const auto bytes=durable;const auto pins=sessions;
        Check(wqn::WordSessionScopeResetTransaction(nullptr)!=ESP_OK && durable==bytes && sessions==pins && !Ready(),
              "temporary scope reset cannot bypass an unresolved default-deck switch");
    }
    Check(wqn::ChangeDefaultWordDeckForeground(deck)==ESP_OK && Installed(6,deck) && Ready(),
          "retry completes the failed generation and releases scope admission");
    for(const auto& invalid:std::vector<std::string>{"short",std::string(36,'x'),old_deck+std::string("\0tail",5)}) {
        Reset();durable["word_deck"]=Text(invalid);
        RejectPreserving("no-marker boot refuses malformed default deck instead of widening to all decks");
        Check(!Ready(),"invalid default deck leaves business admission closed");
    }
    Reset();
    {
        std::string selected;
        Check(wqn::RecoverDeckScopeTransaction(nullptr)==ESP_OK &&
            wqn::LoadDefaultWordDeckId(&selected)==ESP_OK && selected==old_deck && Ready(),
              "validated default-deck read uses owner and preserves exact UUID");
        durable["word_deck"]=Text(std::string(36,'x'));const auto bytes=durable;
        Check(wqn::LoadDefaultWordDeckId(&selected)!=ESP_OK && selected.empty() && !Ready() && durable==bytes,
              "later default metadata failure closes admission without rewriting it");
    }
    for(const std::string& target:std::vector<std::string>{deck,""}) {
        Reset();g_deck_scope_generation.store(5);
        Check(wqn::ChangeDefaultWordDeckForeground(target)==ESP_OK && Installed(6,target),
              "public default-deck switch retains marker-first recoverable protocol");
        const unsigned boundary_count=mutation_count;
        Check(boundary_count==14,"all session/staging/commit boundaries are covered by fault matrix");
        for(unsigned step=1;step<=boundary_count;++step)for(bool after:{false,true}) {
            Reset();g_deck_scope_generation.store(5);const auto bytes=durable;const auto pins=sessions;
            fail_step=step;fail_after=after;
            const auto result=wqn::ChangeDefaultWordDeckForeground(target);
            Check(result!=ESP_OK && handles.empty(),"before/after mutation failure is explicit and handles close");
#if WQN_SCOPE_HAS_READY
            Check(!Ready(),"failed accepted boundary keeps business admission closed until verification");
#endif
            fail_step=0;g_deck_scope_generation.store(0);events.clear();
            const auto recovered=wqn::RecoverDeckScopeTransaction(nullptr);
            const bool untouched=durable==bytes && sessions==pins && g_deck_scope_generation.load()==5;
            Check(recovered==ESP_OK && (untouched || Installed(6,target)) &&
                durable.at("unrelated")==Text("opaque preserved bytes") && handles.empty(),
                "restart yields original state or fully completed switch without unrelated loss");
        }
    }
#if WQN_SCOPE_HAS_READY
    Reset();g_deck_scope_ready.store(false);g_deck_scope_tail_active=true;storage_owner=false;
    Check(!wqn::IsDeckScopeCleanupOnStorageOwner() && !Ready(),"off-owner cannot borrow cleanup exception or business readiness");
    storage_owner=true;
    Check(wqn::IsDeckScopeCleanupOnStorageOwner() && !Ready(),"validated owner cleanup exception does not reopen business work");
    g_deck_scope_tail_active=false;
    Check(!wqn::IsDeckScopeCleanupOnStorageOwner(),"cleanup exception ends with its validated tail");
#endif
    std::printf("Deck scope recovery: %u checks, %u failures (host NVS ports; NOT HIL)\n",checks,failures);
    return failures?1:0;
}
