// SDK NVS -> actual pause reader -> actual session loader. The surrounding
// snapshot/journal ports model a healthy store; this is not repair or HIL proof.
#include <cerrno>
#include "esp_rom_crc.h"
namespace wqn::protocol::word_study_v1 {
enum class Mode : uint8_t {kSequential,kRandom,kDictionary,kReview,kIntake,kShuffle,kMistakes};
}
namespace business {
@@HANDLE@@
}
namespace wqn {
using business::NvsHandle;
constexpr const char* WQN_NVS_NAMESPACE="wqn";
[[maybe_unused]] constexpr const char* kTag="pause-sdk-test";
bool scope_ready=true;
bool IsWordDeckScopeReady(){return scope_ready;}
struct PersistedWordSession {
    struct Remote {std::string session_id;uint64_t next_sequence=0;} remote;
    uint32_t deck_scope_generation=17;
    bool paused=false;
};
@@NVS_LOAD@@
@@NVS_SAVE@@
}
namespace pause {
using Mode=wqn::protocol::word_study_v1::Mode;
constexpr const char* kTag="pause-pipeline-test";
const std::string sid="11111111-2222-4333-8444-555555555555";
std::string legacy_path;
bool snapshot_paused=false;
unsigned snapshot_reads=0,snapshot_writes=0,repairs=0;
struct SessionPaths {const char* primary;const char* temporary;const char* backup;};
bool GetSessionCursorPaths(Mode,SessionPaths* paths){*paths={legacy_path.c_str(),nullptr,nullptr};return true;}
@@KEY@@
@@CRC@@
@@RECORD@@
@@READER@@
struct OutboxScan {
    std::vector<int> acknowledged,pending,suspended;
    bool partial_tail=false,backup_source=false;
};
OutboxScan outbox;
esp_err_t LoadSessionRaw(Mode,wqn::PersistedWordSession* session) {
    ++snapshot_reads;*session={};session->remote.session_id=sid;session->paused=snapshot_paused;return ESP_OK;
}
esp_err_t EnsureOutboxCache(OutboxScan** output){*output=&outbox;return ESP_OK;}
void ReconcileSession(const std::vector<int>&,wqn::PersistedWordSession*,bool*){}
bool SessionCoversRecords(const std::vector<int>&,const wqn::PersistedWordSession&){return true;}
esp_err_t SaveSessionRaw(const wqn::PersistedWordSession&){++snapshot_writes;return ESP_OK;}
esp_err_t CheckpointSessionsFromOutbox(const OutboxScan&){++repairs;return ESP_OK;}
esp_err_t CompactCachedOutbox(OutboxScan*){++repairs;return ESP_OK;}
@@CONTEXT@@
@@LOADER@@
}

int main(int argc,char** argv) {
    if(argc!=4)return 2;
    std::ifstream file(argv[2],std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(original.data()),original.size()) || file.peek()!=EOF)return 2;
    const std::string kind=argv[1];pause::legacy_path=argv[3];
    wqn::scope_ready=kind!="unready";
    pause::snapshot_paused=kind=="missing" || kind=="unpaused" || kind=="stale";
    for(unsigned mode=0;mode<7;++mode){
        reset();pause::snapshot_reads=pause::snapshot_writes=pause::repairs=0;
        const auto value=static_cast<pause::Mode>(mode);
        const char* key=pause::GetSessionCursorNvsKey(value);
        if(kind=="disappeared")disappeared=key;
        if(kind=="lookup-fault")lookup_fault=key;
        wqn::PersistedWordSession session;
        pause::LoadSessionContext context{value,&session};
        const auto result=pause::LoadSessionTransaction(&context);
        esp_err_t expected=ESP_OK;
        if(kind=="wrong")expected=ESP_ERR_NVS_TYPE_MISMATCH;
        if(kind=="short")expected=ESP_ERR_INVALID_SIZE;
        if(kind=="crc")expected=ESP_ERR_INVALID_CRC;
        if(kind=="disappeared")expected=ESP_ERR_NVS_NOT_FOUND;
        if(kind=="lookup-fault")expected=ESP_FAIL;
        if(kind=="unready")expected=ESP_ERR_INVALID_STATE;
        const bool expected_pause=kind=="paused" || kind=="legacy" || kind=="missing" || kind=="stale";
        check(result==expected && (expected!=ESP_OK || session.paused==expected_pause) &&
              pause::snapshot_writes==0 && pause::repairs==0 &&
              (kind=="unready" ? opens==0 && pause::snapshot_reads==0 : pause::snapshot_reads==1) &&
              (kind=="legacy" ? sets==1 && partition->writes>0 : sets==0 && partition->writes==0),
              "SDK cursor result reaches actual reader and session loader without guessing unpaused");
        check(partition->bytes==original,"original RAM NVS media bytes unchanged");
    }
    store.reset();partition.reset();handles.clear();
    std::printf("NVS pause pipeline: %u checks, %u failures (actual SDK/readers/loader; healthy journal ports; NOT HIL)\n",checks,failures);
    return failures?1:0;
}
