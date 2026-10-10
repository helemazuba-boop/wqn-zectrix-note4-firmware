#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "storage.h"
#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"
using wqn::SyncJournal;
using wqn::SyncJournalContentState;
constexpr const char* kTag="host-journal";
constexpr size_t kSyncJournalMaxBytes=8192;
std::string primary,temp,backup;
const char* kSyncJournalPath=nullptr;
const char* kSyncJournalTempPath=nullptr;
const char* kSyncJournalBackupPath=nullptr;
int64_t esp_timer_get_time() { return 100000; }
const char* esp_err_to_name(int) { return "host-error"; }
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
struct Interruption {};
int metadata_ops=0,fail_op=0,cut_op=0;
bool cut_after=false,fail_write=false,fail_flush=false,fail_sync=false,fail_close=false;
void BeforeMetadata() {
    ++metadata_ops;
    if(metadata_ops==cut_op && !cut_after) throw Interruption{};
}
void AfterMetadata() { if(metadata_ops==cut_op && cut_after) throw Interruption{}; }
int JournalRename(const char* from,const char* to) {
    BeforeMetadata();
    if(metadata_ops==fail_op) { errno=EIO; return -1; }
    const int result=std::rename(from,to); AfterMetadata(); return result;
}
int JournalRemove(const char* path) {
    BeforeMetadata();
    if(metadata_ops==fail_op) { errno=EIO; return -1; }
    const int result=std::remove(path); AfterMetadata(); return result;
}
size_t JournalWrite(const void* data,size_t size,size_t count,FILE* file) {
    return std::fwrite(data,size,fail_write && count?count-1:count,file);
}
int JournalFlush(FILE* file) { const int result=std::fflush(file); return fail_flush?-1:result; }
int JournalSync(int fd) { return fail_sync?-1:fsync(fd); }
int JournalClose(FILE* file) { const int result=std::fclose(file); return fail_close?-1:result; }
int queue_calls=0,lease_depth=0;
struct StorageWriteGuard {
    StorageWriteGuard(const char*,const char*,int) { ++lease_depth; }
    ~StorageWriteGuard() { --lease_depth; }
    explicit operator bool() const { return true; }
};
namespace wqn::services {
int ExecuteStorageTransactionNamed(int(*fn)(void*),void* context,const char* owner) {
    if(lease_depth!=1 || std::strcmp(owner,"save-sync-journal")) return ESP_FAIL;
    ++queue_calls; return fn(context);
}
}
@@PRODUCTION@@

int checks=0,failures=0;
int allocation_calls=0,allocation_fail_at=0,allocation_live=0;
void* JournalAllocate(size_t size) {
    if(++allocation_calls==allocation_fail_at) return nullptr;
    void* value=std::malloc(size); if(value) ++allocation_live; return value;
}
void JournalFree(void* value) { if(value) --allocation_live; std::free(value); }
void Check(bool okay,const char* label) {
    ++checks; failures+=!okay; std::printf("%s: %s\n",okay?"PASS":"FAIL",label);
}
void WriteHost(const std::string& path,const std::string& bytes) {
    FILE* file=std::fopen(path.c_str(),"wb");
    if(!file || std::fwrite(bytes.data(),1,bytes.size(),file)!=bytes.size() || std::fclose(file)) std::abort();
}
std::string ReadHost(const std::string& path) {
    std::string bytes; (void)ReadStorageTextFile(path.c_str(),&bytes); return bytes;
}
void Reset() {
    for(const auto* path:{kSyncJournalPath,kSyncJournalTempPath,kSyncJournalBackupPath}) std::remove(path);
    metadata_ops=fail_op=cut_op=0; cut_after=false;
    fail_write=fail_flush=fail_sync=fail_close=false;
    g_sync_journal_durable_payload_valid=false; g_sync_journal_durable_payload.clear();
}
void ReplaceNumber(cJSON* root,const char* key,double value) {
    cJSON_ReplaceItemInObjectCaseSensitive(root,key,cJSON_CreateNumber(value));
}
std::string Render(cJSON* root) {
    std::string payload; if(JsonToString(root,&payload)!=ESP_OK) std::abort(); return payload;
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    primary=std::string(argv[1])+"/journal.json"; temp=std::string(argv[1])+"/journal.tmp";
    backup=std::string(argv[1])+"/journal.bak";
    kSyncJournalPath=primary.c_str(); kSyncJournalTempPath=temp.c_str(); kSyncJournalBackupPath=backup.c_str();
    SyncJournal old_state,new_state,loaded;
    old_state.config_revision=1; old_state.sync_cursor=10;
    old_state.word_packs.desired_revision=7; old_state.word_packs.applied_revision=6;
    old_state.word_packs.phase=wqn::SyncJournalPhase::kPending;
    new_state=old_state; new_state.config_revision=2; new_state.sync_cursor=11;
    new_state.word_packs.desired_revision=8;
    Reset(); Check(wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==0,"missing journal returns empty schema2");
    Check(wqn::SaveSyncJournalThroughStorageService(old_state,"test-old")==ESP_OK &&
        wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==10 && loaded.word_packs.applied_revision==6,
        "real codec and filesystem round-trip through storage owner");
    const std::string old_payload=ReadHost(primary);
    const int before=metadata_ops;
    Check(wqn::SaveSyncJournalThroughStorageService(old_state,"unchanged")==ESP_OK && metadata_ops==before,
        "exact durable bytes bypass fopen/rename/remove");
    Check(wqn::SaveSyncJournalThroughStorageService(new_state,"test-new")==ESP_OK && ReadHost(backup)==old_payload,
        "successful rotation retains the previous complete checkpoint");
    const std::string new_payload=ReadHost(primary);
    WriteHost(primary,"{broken");
    Check(wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==10,"corrupt primary recovers valid backup");

    for(const bool after:{false,true}) for(int point=1;point<=2;++point) {
        Reset(); WriteHost(primary,"{broken"); WriteHost(backup,old_payload);
        cut_op=point; cut_after=after; bool cut=false;
        try { (void)wqn::SaveSyncJournalThroughStorageService(new_state,"repair"); } catch(const Interruption&) { cut=true; }
        cut_op=0; g_sync_journal_durable_payload_valid=false;
        Check(cut && wqn::LoadSyncJournal(&loaded)==ESP_OK &&
            (loaded.sync_cursor==10 || loaded.sync_cursor==11),
            "backup-repair metadata interruption retains complete old/new checkpoint");
    }
    for(const bool after:{false,true}) for(int point=1;point<=3;++point) {
        Reset(); WriteHost(primary,old_payload);
        cut_op=point; cut_after=after; bool cut=false;
        try { (void)wqn::SaveSyncJournalThroughStorageService(new_state,"rotation"); } catch(const Interruption&) { cut=true; }
        cut_op=0; g_sync_journal_durable_payload_valid=false;
        Check(cut && wqn::LoadSyncJournal(&loaded)==ESP_OK &&
            (loaded.sync_cursor==10 || loaded.sync_cursor==11),
            "valid-primary metadata interruption retains complete old/new checkpoint");
    }
    Reset(); WriteHost(primary,old_payload); fail_op=3;
    Check(wqn::SaveSyncJournalThroughStorageService(new_state,"rename-fail")!=ESP_OK &&
        wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==10,
        "failed new-primary rename restores old checkpoint");
    fail_op=0; const int failed_ops=metadata_ops;
    Check(wqn::SaveSyncJournalThroughStorageService(new_state,"rename-retry")==ESP_OK &&
        metadata_ops>failed_ops && wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==11,
        "failed commit never enters successful dedup cache");
    for(int fault=0;fault<4;++fault) {
        Reset(); WriteHost(primary,old_payload);
        fail_write=fault==0; fail_flush=fault==1; fail_sync=fault==2; fail_close=fault==3;
        const int result=wqn::SaveSyncJournalThroughStorageService(new_state,"flush-fail");
        fail_write=fail_flush=fail_sync=fail_close=false;
        Check(result!=ESP_OK && wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==10,
            "short write/flush/fsync/close failure cannot report or publish new checkpoint");
    }

    cJSON* legacy=cJSON_Parse(old_payload.c_str()); ReplaceNumber(legacy,"schema_version",1);
    for(const char* key:{"full_sync_retry","word_outbox","note_outbox","problem_outbox","protocol_blocked_image_id"})
        cJSON_DeleteItemFromObjectCaseSensitive(legacy,key);
    for(const char* key:{"word_packs","note_packs","problem_packs"})
        cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(legacy,key),"retry_not_before_unix_seconds");
    Check(ParseSyncJournalPayload(Render(legacy),&loaded)==ESP_OK && loaded.schema_version==2 && loaded.sync_cursor==10,
        "legacy schema1 remains readable and normalizes to schema2");
    cJSON_Delete(legacy);
    for(const char* bad:{"-1","0.5","9007199254740992"}) {
        std::string payload=old_payload;
        const std::string needle="\"sync_cursor\":10";
        const size_t pos=payload.find(needle); if(pos==std::string::npos) std::abort();
        payload.replace(pos,needle.size(),std::string("\"sync_cursor\":")+bad);
        Check(ParseSyncJournalPayload(payload,&loaded)!=ESP_OK,"negative/fractional/unsafe JSON cursor is rejected");
    }
    cJSON* missing=cJSON_Parse(old_payload.c_str());
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(missing,"word_packs"),"desired_snapshot_id");
    Check(ParseSyncJournalPayload(Render(missing),&loaded)!=ESP_OK,"schema2 missing string is corruption, not measured empty");
    cJSON_Delete(missing);
    Check(ParseSyncJournalPayload(old_payload+"junk",&loaded)!=ESP_OK &&
        ParseSyncJournalPayload(old_payload+std::string(1,'\0'),&loaded)!=ESP_OK,
        "trailing garbage and embedded NUL are rejected");
    Reset(); WriteHost(primary,std::string(9000,'x')); WriteHost(backup,old_payload);
    Check(wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==10,
        "oversize primary is bounded before allocation and can recover backup");
    WriteHost(backup,"{broken");
    Check(wqn::LoadSyncJournal(&loaded)!=ESP_OK,"both invalid sources fail, never silently reset progress");
    Reset(); auto unsafe=old_state; unsafe.sync_cursor=9007199254740992ULL;
    Check(wqn::SaveSyncJournalThroughStorageService(unsafe,"unsafe")!=ESP_OK && metadata_ops==0,
        "unreadable candidate is rejected before metadata writes");
    auto precise=old_state; precise.sync_cursor=9007199254740991ULL;
    const int precision_result=wqn::SaveSyncJournalThroughStorageService(precise,"precision");
    Check(precision_result!=ESP_OK || (wqn::LoadSyncJournal(&loaded)==ESP_OK && loaded.sync_cursor==precise.sync_cursor),
        "large integer must round-trip exactly or fail, never commit a rounded cursor");
    auto unterminated=old_state; std::memset(unterminated.word_outbox.request_id,'x',65);
    Check(wqn::SaveSyncJournalThroughStorageService(unterminated,"bad-string")!=ESP_OK,
        "unterminated fixed string is rejected before cJSON access");
    bool allocations_safe=true; int reached_failures=0;
    for(int point=1;point<=200;++point) {
        Reset(); WriteHost(primary,old_payload); WriteHost(backup,old_payload);
        allocation_calls=allocation_live=0; allocation_fail_at=point;
        cJSON_Hooks hooks{JournalAllocate,JournalFree}; cJSON_InitHooks(&hooks);
        const int result=wqn::SaveSyncJournalThroughStorageService(new_state,"allocation-fault");
        reached_failures+=allocation_calls>=point;
        allocations_safe=allocations_safe && allocation_live==0;
        cJSON_InitHooks(nullptr);
        const int load=wqn::LoadSyncJournal(&loaded);
        allocations_safe=allocations_safe && load==ESP_OK &&
            (loaded.sync_cursor==10 || loaded.sync_cursor==11) &&
            (result!=ESP_OK || (loaded.sync_cursor==11 && loaded.config_revision==2 && loaded.word_packs.desired_revision==8));
    }
    Check(allocations_safe && reached_failures>50,
        "cJSON allocation failures neither leak detached children nor acknowledge partial checkpoints");
    Check(queue_calls>0 && lease_depth==0,"storage context is synchronous and lease is released on all exits");
    std::printf("%d PASS / %d FAIL (host filesystem call seams; NOT HIL)\n",checks-failures,failures);
    return failures?1:0;
}
