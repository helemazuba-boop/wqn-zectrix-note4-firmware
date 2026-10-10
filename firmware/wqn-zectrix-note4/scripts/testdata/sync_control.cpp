#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "cJSON.h"
#include "storage.h"
#include "services/sync_service.h"
using wqn::SyncJournal;

#define CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE 1
#define CONFIG_WQN_WIFI_STA_ENABLE 1
#define WQN_FIRMWARE_VERSION "host-audit-only"
#define WQN_SYNC_LIMIT 20
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_RETURN_ON_ERROR(call, ...) do { const int audit_result=(call); if(audit_result!=ESP_OK) return audit_result; } while(0)
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
const char* esp_err_to_name(int) { return "mock-error"; }
constexpr const char* kTag="host-audit";
@@RETRY_CONFIG@@
constexpr uint32_t kFullRetryMagic=0x57514E52;
constexpr std::time_t kMinScheduleUnixTime=1704067200;
@@OUTBOX_RETRY_CONFIG@@
constexpr uint32_t kWordPacksRefreshBit=1,kNotePacksRefreshBit=2,kProblemPacksRefreshBit=4;
constexpr uint32_t kClaimRetryBaseMs=15000;
enum class OutboxRetryCause:uint8_t { kNone,kTransport,kServer,kLocalStorage };
const char* OutboxRetryCauseName(OutboxRetryCause) { return "mock-cause"; }
int g_sync_snapshot_lock=0,g_periodic_schedule_lock=0,mock_mutex_storage=0;
void* g_sync_journal_mutex=&mock_mutex_storage;
constexpr int pdTRUE=1;
int mock_lock_depth=0,lock_errors=0;
int xSemaphoreTake(void*,TickType_t) { if(mock_lock_depth) { ++lock_errors; return 0; } mock_lock_depth=1; return pdTRUE; }
void xSemaphoreGive(void*) { if(mock_lock_depth!=1) ++lock_errors; mock_lock_depth=0; }
int64_t mock_ms=100000;
std::time_t mock_wall=1705000000;
std::time_t CurrentUnixSeconds() { return mock_wall; }
int64_t esp_timer_get_time() { return mock_ms*1000; }
uint32_t esp_random() { return 0; }
const char* CurrentFirmwareImageId() { return "host-audit-image"; }
uint32_t ClaimPollDelayMs() { return 10000; }
uint32_t AddClaimJitter(uint32_t value) { return value; }
void SetSyncStatus(const char*) {}
void PublishSyncEvent(wqn::services::SyncEventStatus,int64_t,wqn::services::SyncEventScope) {}
std::string RandomControlId(const char* prefix) {
    static unsigned count=0;
    return std::string(prefix)+"host_only_"+std::to_string(++count);
}
const std::string& ControlBootId() {
    static const std::string id="boot_host_only_0001";
    return id;
}
uint8_t g_full_sync_retry_attempts=0;
uint32_t g_full_sync_retry_magic=0;
int64_t g_full_sync_retry_unix_seconds=0;
std::atomic<int64_t> g_full_sync_retry_not_before_ms{0};
std::atomic<uint32_t> g_auto_sync_interval_minutes{15};
uint64_t g_config_revision=1,g_sync_cursor=10;
bool g_bootstrap_complete=false,g_control_state_loaded=false;
bool g_control_protocol_blocked_this_round=false,g_claim_active=false;
uint32_t g_control_retry_after_ms=0;
wqn::SyncJournal g_sync_journal;
bool g_sync_journal_loaded=true,g_outbox_protocol_suspended=false;
wqn::services::SyncSnapshot g_sync_snapshot;
uint32_t g_content_active_generation[3]={0,0,0};
std::string g_word_outbox_retry_request_id,g_note_outbox_retry_request_id,g_problem_outbox_retry_request_id;
int64_t g_word_outbox_retry_not_before_ms=0,g_note_outbox_retry_not_before_ms=0,g_problem_outbox_retry_not_before_ms=0;
uint8_t g_word_outbox_retry_attempts=0,g_note_outbox_retry_attempts=0,g_problem_outbox_retry_attempts=0;
OutboxRetryCause g_word_outbox_retry_cause=OutboxRetryCause::kNone;
OutboxRetryCause g_note_outbox_retry_cause=OutboxRetryCause::kNone;
OutboxRetryCause g_problem_outbox_retry_cause=OutboxRetryCause::kNone;

// Real NVS 5.5 writes each key before commit (verified in local IDF source).
// This seam models API-call boundaries only, not the NVS flash-page algorithm.
constexpr const char* WQN_NVS_NAMESPACE="host-only";
constexpr const char* kControlConfigRevisionKey="v3_cfg_rev";
constexpr const char* kControlSyncCursorKey="v3_cursor";
constexpr int NVS_READWRITE=1;
struct NvsHandle { int handle=1; };
std::map<std::string,uint64_t> mock_nvs;
int nvs_calls=0;
std::set<int> fail_nvs_calls;
int nvs_open(const char*,int,int*) { return ESP_OK; }
int nvs_set_u64(int,const char* key,uint64_t value) {
    if(fail_nvs_calls.count(++nvs_calls)) return ESP_FAIL;
    mock_nvs[key]=value; return ESP_OK;
}
int nvs_commit(int) { return ESP_OK; }
int LoadU64FromNvs(const char* key,uint64_t* out,bool* found) {
    const auto it=mock_nvs.find(key);
    *found=it!=mock_nvs.end(); *out=*found?it->second:0; return ESP_OK;
}
struct PowerCut {};
int journal_calls=0,cut_journal_at=0,cut_after_save_at=0,load_error=ESP_OK;
std::set<int> fail_journal_calls;
wqn::SyncJournal durable_journal;
std::string durable_payload;
std::vector<std::string> attempted_payloads,committed_payloads;
int WriteSyncJournalFileAtomic(const std::string& payload) {
    attempted_payloads.push_back(payload);
    ++journal_calls;
    if(journal_calls==cut_journal_at) throw PowerCut{};
    if(fail_journal_calls.count(journal_calls)) return ESP_FAIL;
    durable_payload=payload; committed_payloads.push_back(payload); return ESP_OK;
}
int SaveSyncJournalRaw(const wqn::SyncJournal&);
namespace wqn {
void SetWordReviewDueCount(int) {}
void SetWordMistakeCount(int) {}
int SaveSyncJournalThroughStorageService(const SyncJournal& value) {
    if(mock_lock_depth!=1) ++lock_errors;
    const int result=SaveSyncJournalRaw(value);
    if(result==ESP_OK) { durable_journal=value; if(journal_calls==cut_after_save_at) throw PowerCut{}; }
    return result;
}
int LoadSyncJournal(SyncJournal* out) { if(load_error!=ESP_OK) return load_error; *out=durable_journal; return ESP_OK; }
}
bool reject_changed_body=true;
int transport_failures=0; std::string rpc_error;
wqn::protocol::v3::SyncData remote_sync;
wqn::protocol::v3::BootstrapData remote_bootstrap;
std::vector<std::string> rpc_bodies,rpc_ids;
std::map<std::string,std::string> server_replays;
int MockRemote(const std::string& id,const std::string& body,wqn::protocol::v3::Error* error) {
    rpc_ids.push_back(id); rpc_bodies.push_back(body);
    if(!rpc_error.empty()) { error->code=rpc_error; return ESP_FAIL; }
    if(transport_failures>0) { --transport_failures; return ESP_ERR_TIMEOUT; }
    const auto it=server_replays.find(id);
    if(reject_changed_body && it!=server_replays.end() && it->second!=body) {
        error->code="REQUEST_ID_REUSED"; error->retryable=false; return ESP_FAIL;
    }
    server_replays[id]=body; return ESP_OK;
}
namespace wqn {
int SyncDeviceControlV3(const std::string&,const protocol::v3::RequestMetadata& metadata,
    uint32_t interval,protocol::v3::SyncData* data,protocol::v3::Error* error) {
    std::string body;
    const int build=protocol::v3::BuildSyncRequest(metadata,interval,&body);
    if(build!=ESP_OK) return build;
    const int result=MockRemote(metadata.request_id,body,error);
    if(result==ESP_OK) *data=remote_sync;
    return result;
}
int BootstrapDeviceControlV3(const std::string&,const protocol::v3::RequestMetadata& metadata,
    protocol::v3::BootstrapData* data,protocol::v3::Error* error) {
    std::string body;
    const int build=protocol::v3::BuildBootstrapRequest(metadata,&body);
    if(build!=ESP_OK) return build;
    const int result=MockRemote(metadata.request_id,body,error);
    if(result==ESP_OK) *data=remote_bootstrap;
    return result;
}
}

wqn::protocol::v3::RequestMetadata MutableSyncMetadata();
@@PRODUCTION@@

wqn::protocol::v3::RequestMetadata MutableSyncMetadata() {
    auto metadata=MakeControlMetadata(); metadata.request_id=g_sync_request_id;
    return metadata;
}
int checks=0,failures=0;
void Check(bool okay,const char* label) {
    ++checks; failures+=!okay;
    std::printf("%s: %s\n",okay?"PASS":"FAIL",label);
}
void Reset() {
    mock_lock_depth=0; mock_ms=100000; mock_wall=1705000000;
    g_full_sync_retry_attempts=0; g_full_sync_retry_magic=0;
    g_full_sync_retry_unix_seconds=0; g_full_sync_retry_not_before_ms=0;
    g_config_revision=1; g_sync_cursor=10;
    g_bootstrap_complete=g_control_state_loaded=g_control_protocol_blocked_this_round=g_claim_active=false;
    g_control_retry_after_ms=0; g_auto_sync_interval_minutes=15; ResetControlExchanges();
    g_sync_journal={}; g_sync_journal.config_revision=1; g_sync_journal.sync_cursor=10;
    g_sync_snapshot={}; g_content_active_generation[0]=g_content_active_generation[1]=g_content_active_generation[2]=0;
    g_sync_journal_loaded=true; g_outbox_protocol_suspended=false;
    g_word_outbox_retry_request_id.clear(); g_note_outbox_retry_request_id.clear(); g_problem_outbox_retry_request_id.clear();
    g_word_outbox_retry_not_before_ms=g_note_outbox_retry_not_before_ms=g_problem_outbox_retry_not_before_ms=0;
    g_word_outbox_retry_attempts=g_note_outbox_retry_attempts=g_problem_outbox_retry_attempts=0;
    mock_nvs={{kControlConfigRevisionKey,98},{kControlSyncCursorKey,100}};
    nvs_calls=0; fail_nvs_calls.clear(); fail_journal_calls.clear(); cut_journal_at=cut_after_save_at=0; load_error=ESP_OK;
    remote_sync={}; remote_sync.config_revision=2; remote_sync.sync_cursor=11; remote_sync.auto_sync_interval_minutes=15;
    remote_bootstrap={}; remote_bootstrap.config_revision=2; remote_bootstrap.sync_cursor=11;
    transport_failures=0; rpc_error.clear(); reject_changed_body=true; rpc_ids.clear(); rpc_bodies.clear(); server_replays.clear();
    SaveSyncJournalRaw(g_sync_journal); durable_journal=g_sync_journal;
    journal_calls=0; attempted_payloads.clear(); committed_payloads.clear();
}
int main() {
    Reset(); transport_failures=1;
    Check(SyncControlPlaneV3("host-placeholder")!=ESP_OK,"transport failure is propagated");
    g_auto_sync_interval_minutes=60; g_config_revision=4; g_sync_cursor=50;
    Check(SyncControlPlaneV3("host-placeholder")==ESP_OK,"transport retry succeeds after local metadata changes");
    Check(rpc_ids.size()==2 && rpc_ids[0]==rpc_ids[1] && rpc_bodies[0]==rpc_bodies[1],
        "sync retry freezes every actual JSON body field");
    Reset(); transport_failures=1; BootstrapControlV3("host-placeholder");
    g_config_revision=4; g_sync_cursor=50;
    Check(BootstrapControlV3("host-placeholder")==ESP_OK && rpc_bodies[0]==rpc_bodies[1],
        "bootstrap retry freezes every actual JSON body field");

    Reset(); remote_sync.content_targets={{wqn::protocol::v3::SyncContentKind::kProblemPacks,7,""}};
    fail_journal_calls={1};
    Check(SyncControlPlaneV3("host-placeholder")!=ESP_OK,"local journal failure is propagated");
    Check(g_config_revision==1 && g_sync_cursor==10 &&
        g_sync_journal.sync_cursor==10 && g_sync_snapshot.problem_packs.desired_revision==0,
        "failed bundled checkpoint publishes neither new cursor nor content target");
    Check(durable_journal.sync_cursor==10 && durable_journal.problem_packs.desired_revision==0,
        "failed bundled checkpoint retains old durable state");
    fail_journal_calls.clear();
    Check(SyncControlPlaneV3("host-placeholder")==ESP_OK && rpc_ids.size()==1,
        "local storage retry reuses successful response without another RPC");
    Check(durable_journal.sync_cursor==11 && durable_journal.problem_packs.desired_revision==7 &&
        g_sync_snapshot.problem_packs.desired_revision==7 && g_sync_cursor==11,
        "target and cursor become durable before runtime publication");
    Check(nvs_calls==0 && mock_nvs.at(kControlSyncCursorKey)==100,
        "control commit never advances legacy NVS scalar keys");
    Check(g_sync_request_id.empty() && !g_sync_response_ready,"successful local commit retires response and request together");
    Reset(); fail_journal_calls={1}; BootstrapControlV3("host-placeholder");
    fail_journal_calls.clear();
    Check(BootstrapControlV3("host-placeholder")==ESP_OK && rpc_ids.size()==1 &&
        g_bootstrap_complete && durable_journal.sync_cursor==11,
        "bootstrap local failure also retries only the cached response commit");

    Reset(); rpc_error="REQUEST_ID_REUSED";
    SyncControlPlaneV3("host-placeholder"); const auto conflicted=rpc_ids.back(); rpc_error.clear();
    Check(SyncControlPlaneV3("host-placeholder")==ESP_OK && rpc_ids.back()!=conflicted,
        "an explicit server ID conflict retires the broken control identity");
    Reset(); fail_journal_calls={1}; SyncControlPlaneV3("host-placeholder");
    const auto abandoned=rpc_ids.back(); ResetControlExchanges(); fail_journal_calls.clear();
    Check(SyncControlPlaneV3("host-placeholder")==ESP_OK && rpc_ids.back()!=abandoned,
        "credential exchange reset cannot reuse a previous response or ID");

    for(const bool after:{false,true}) {
        Reset(); remote_sync.content_targets={{wqn::protocol::v3::SyncContentKind::kProblemPacks,7,""}};
        if(after) cut_after_save_at=1; else cut_journal_at=1;
        bool cut=false; try { SyncControlPlaneV3("host-placeholder"); } catch(const PowerCut&) { cut=true; }
        Check(cut,"control interruption seam reached");
        cut_journal_at=cut_after_save_at=0; ResetControlExchanges();
        g_sync_journal_loaded=false; g_control_state_loaded=false; g_sync_snapshot={};
        Check(EnsureControlStateLoaded()==ESP_OK &&
            g_sync_cursor==(after?11u:10u) && g_config_revision==(after?2u:1u) &&
            g_sync_journal.problem_packs.desired_revision==(after?7u:0u),
            "restart reads only a coherent old/new journal checkpoint and ignores advanced NVS");
    }
    Reset(); durable_journal={}; g_sync_journal_loaded=false; g_control_state_loaded=false;
    Check(EnsureControlStateLoaded()==ESP_OK && g_config_revision==0 && g_sync_cursor==0,
        "missing journal starts at zero instead of importing a torn legacy pair");
    Reset(); load_error=ESP_FAIL; g_sync_journal_loaded=false; g_control_state_loaded=false;
    Check(EnsureControlStateLoaded()!=ESP_OK && !g_control_state_loaded,
        "invalid journal does not silently fall back to legacy NVS");
    Reset(); durable_journal.problem_packs.phase=wqn::SyncJournalPhase::kInstalling;
    durable_journal.problem_packs.desired_revision=7;
    g_sync_journal_loaded=false; fail_journal_calls={1};
    Check(EnsureSyncJournalLoaded()!=ESP_OK && !g_sync_journal_loaded,
        "failed startup recovery remains unready");
    fail_journal_calls.clear();
    Check(EnsureSyncJournalLoaded()==ESP_OK && durable_journal.problem_packs.phase==wqn::SyncJournalPhase::kPending,
        "startup recovery is retried and committed before readiness");
    Reset(); g_content_active_generation[2]=1;
    const wqn::services::SyncContentTicket ticket{wqn::services::SyncContentDomain::kProblemPacks,1,7};
    fail_journal_calls={1};
    Check(wqn::services::BeginContentInstall(ticket)!=ESP_OK && durable_journal.problem_packs.phase==wqn::SyncJournalPhase::kClean,
        "failed install marker cannot be reported as durable");
    fail_journal_calls.clear();
    Check(wqn::services::BeginContentInstall(ticket)==ESP_OK && durable_journal.problem_packs.phase==wqn::SyncJournalPhase::kInstalling,
        "installation marker is durable before installation proceeds");
    const std::string hash(64,'a'); wqn::services::CompleteContentRefresh(ticket,ESP_OK,hash.c_str());
    Check(durable_journal.problem_packs.applied_revision==7,"successful content completion becomes durable");
    Check(lock_errors==0 && mock_lock_depth==0,"every save holds one non-recursive mutation/commit lock, released on all exits");
    std::printf("%d PASS / %d FAIL (production control paths; NOT HIL)\n",checks-failures,failures);
    return failures?1:0;
}
