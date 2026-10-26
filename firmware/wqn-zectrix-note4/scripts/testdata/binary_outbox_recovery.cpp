#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
@@DOMAIN@@
unsigned checks=0;
#define CHECK(e) do { ++checks; if (!(e)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#e); std::exit(1); } } while(false)
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(e, ...) do { const int r=(e); if(r!=ESP_OK) return r; } while(false)
const char* esp_err_to_name(int) { return "host"; }
const char* kTag="outbox-fixture";
constexpr unsigned kOutboxMagic=0x12345678;
constexpr unsigned kOutboxSchemaVersion=1, kRuntimeCompactAckThreshold=32;
@@RECORDS@@
OutboxScan g_outbox_cache;
bool g_outbox_cache_loaded=false;
char kOutboxPath[512],kOutboxTempPath[512],kOutboxBackupPath[512],kOutboxRecoveryPath[512];
std::map<FILE*,std::string> streams;
std::set<FILE*> bad_reads;
std::map<std::string,unsigned> calls;
std::string fail_op, fail_op2, crash_op;
unsigned fail_occurrence=0;
struct Stopped {};
extern "C" FILE* __real_fopen(const char*,const char*);
extern "C" int __real_fclose(FILE*);
extern "C" size_t __real_fwrite(const void*,size_t,size_t,FILE*);
extern "C" size_t __real_fread(void*,size_t,size_t,FILE*);
extern "C" int __real_ferror(FILE*);
extern "C" int __real_fflush(FILE*);
extern "C" int __real_fsync(int);
extern "C" int __real_rename(const char*,const char*);
extern "C" int __real_remove(const char*);
std::string Tag(const char* path) {
    if(std::strcmp(path,kOutboxPath)==0) return "P";
    if(std::strcmp(path,kOutboxBackupPath)==0) return "B";
    if(std::strcmp(path,kOutboxRecoveryPath)==0) return "R";
    return "T";
}
bool Fail(const std::string& op) {
    ++calls[op];
    if((op==fail_op || op==fail_op2) &&
       (fail_occurrence==0 || calls[op]==fail_occurrence)) { errno=EIO; return true; }
    return false;
}
extern "C" FILE* __wrap_fopen(const char* path,const char* mode) {
    if(Fail("open:"+Tag(path))) return nullptr;
    FILE* f=__real_fopen(path,mode); if(f) streams[f]=Tag(path); return f;
}
extern "C" int __wrap_fclose(FILE* f) {
    const bool failed=Fail("close:"+streams.at(f));
    streams.erase(f); bad_reads.erase(f);
    const int result=__real_fclose(f); return failed?EOF:result;
}
extern "C" size_t __wrap_fwrite(const void* b,size_t n,size_t c,FILE* f) {
    return __real_fwrite(b,n,Fail("write:"+streams.at(f))?c/2:c,f);
}
extern "C" size_t __wrap_fread(void* b,size_t n,size_t c,FILE* f) {
    if(Fail("read:"+streams.at(f))) { bad_reads.insert(f); return 0; }
    return __real_fread(b,n,c,f);
}
extern "C" int __wrap_ferror(FILE* f) { return bad_reads.count(f)?1:__real_ferror(f); }
extern "C" int __wrap_fflush(FILE* f) { return Fail("flush:"+streams.at(f))?EOF:__real_fflush(f); }
extern "C" int __wrap_fsync(int fd) { return Fail("sync")?-1:__real_fsync(fd); }
extern "C" int __wrap_remove(const char* path) {
    return Fail("remove:"+Tag(path))?-1:__real_remove(path);
}
extern "C" int __wrap_rename(const char* from,const char* to) {
    const std::string op="rename:"+Tag(from)+"->"+Tag(to);
    if(Fail(op)) return -1;
    const int result=__real_rename(from,to);
    if(result==0 && op==crash_op) throw Stopped{};
    return result;
}
int64_t esp_timer_get_time() { static int64_t tick=0; return ++tick; }
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* b,uint32_t n) {
    crc=~crc;
    while(n--) { crc^=*b++; for(int bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320U:0U); }
    return ~crc;
}
@@UTILITIES@@
@@VALIDATE@@
@@IO@@
@@MARKERS@@
void Reboot() {
    CHECK(streams.empty()); fail_op.clear(); fail_op2.clear(); crash_op.clear(); calls.clear();
    fail_occurrence=0;
    g_outbox_cache={}; g_outbox_cache_loaded=false;
}
void Reset() {
    Reboot();
    for(const char* path:{kOutboxPath,kOutboxTempPath,kOutboxBackupPath,kOutboxRecoveryPath}) __real_remove(path);
}
OutboxRecord Record(const char* id) {
#if WORD_DOMAIN
    wqn::DurableWordObservation value;
#else
    wqn::DurableNoteObservation value;
#endif
    value.request_id=id; value.session_id="00000000-0000-0000-0000-000000000001";
    value.item_id="00000000-0000-0000-0000-000000000002"; value.occurred_at="2026-10-22T00:00:00Z";
    OutboxRecord result{};
    CHECK(BuildObservationRecord(value,OutboxRecordKind::kObservation,&result)==ESP_OK); return result;
}
void Put(const char* path,const std::vector<OutboxRecord>& records,size_t tail=0) {
    FILE* f=__real_fopen(path,"wb"); CHECK(f);
    for(const auto& record:records) CHECK(__real_fwrite(&record,1,sizeof(record),f)==sizeof(record));
    if(tail) CHECK(__real_fwrite(&records.front(),1,tail,f)==tail);
    CHECK(__real_fclose(f)==0);
}
void Seed(bool parked=true) {
    Reset(); const auto one=Record("first-request-0001"),two=Record("latest-request-002");
    OutboxRecord marker{};
    CHECK(BuildSuspendRecord(one.request_id,static_cast<wqn::OutboxSuspendReason>(1),&marker)==ESP_OK);
    Put(kOutboxBackupPath,{one});
    Put(kOutboxPath,parked?std::vector<OutboxRecord>{one,two,marker}:std::vector<OutboxRecord>{one,two});
}
void Latest(bool parked=true) {
    OutboxScan* scan=nullptr; CHECK(EnsureOutboxCache(&scan)==ESP_OK);
    CHECK(scan->pending.size()==(parked?1:2));
    CHECK(std::strcmp(scan->pending.back().request_id,"latest-request-002")==0);
    CHECK(scan->suspended.size()==(parked?1:0));
    const auto expected=Record("latest-request-002");
    CHECK(std::memcmp(&scan->pending.back(),&expected,sizeof(expected))==0);
    if(parked) {
        const auto first=Record("first-request-0001");
        CHECK(std::memcmp(&scan->suspended.front(),&first,sizeof(first))==0);
    }
}
void CompatibleBackup() {
    OutboxScan backup; CHECK(ScanOutboxFile(kOutboxBackupPath,&backup)==ESP_OK);
    CHECK(backup.pending.size()==1 && backup.suspended.empty());
    const auto expected=Record("first-request-0001");
    CHECK(std::memcmp(&backup.pending.front(),&expected,sizeof(expected))==0);
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    for(auto pair:{std::make_pair(kOutboxPath,"p"),std::make_pair(kOutboxTempPath,"t"),
                   std::make_pair(kOutboxBackupPath,"b"),std::make_pair(kOutboxRecoveryPath,"r")})
        std::snprintf(pair.first,512,"%s/%s",argv[1],pair.second);
    // Preserve the newer primary independently of the older compatible backup.
    Seed(); Latest(); fail_op="rename:T->P";
    CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_FAIL && !g_outbox_cache_loaded);
    Reboot(); Latest(); CompatibleBackup(); CHECK(g_outbox_cache.backup_source);
    CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_OK);
    Reboot(); Latest(); CompatibleBackup();
    for(const char* op:{"open:T","write:T","flush:T","sync","close:T","rename:P->R"}) {
        Seed(); Latest(); fail_op=op;
        CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_FAIL && !g_outbox_cache_loaded);
        Reboot(); Latest(); CompatibleBackup();
    }
    for(const char* stop:{"rename:P->R","rename:T->P"}) {
        Seed(); Latest(); crash_op=stop; bool stopped=false;
        try { CompactCachedOutbox(&g_outbox_cache); } catch(Stopped&) { stopped=true; }
        CHECK(stopped); Reboot(); Latest();
    }
    // Do not replace a good recovery source with the unreadable primary.
    Seed(); CHECK(__real_rename(kOutboxPath,kOutboxRecoveryPath)==0);
    FILE* corrupt=__real_fopen(kOutboxPath,"wb"); CHECK(corrupt);
    CHECK(__real_fwrite("bad",1,3,corrupt)==3); CHECK(__real_fclose(corrupt)==0);
    // A short primary is a readable prefix, so force a CRC-invalid full record.
    auto invalid=Record("invalid-request-00"); invalid.crc^=1; Put(kOutboxPath,{invalid});
    Latest(); fail_op="rename:T->P";
    CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_FAIL); Reboot(); Latest();
    Seed(); Latest(); fail_op="remove:R"; fail_occurrence=2;
    CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_OK && FileExists(kOutboxRecoveryPath));
    Reboot(); Latest(); CompatibleBackup();
    // Stale recovery copies must not beat a newer rotated backup on later failure.
    Seed(); Latest(); fail_op="remove:R";
    // First remove of stale recovery is before touching the authoritative primary.
    CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_FAIL); Reboot(); Latest();
    Seed(false); Put(kOutboxRecoveryPath,{Record("stale-request-0000")}); Latest(false);
    fail_op="rename:T->P"; fail_op2="rename:B->P";
    CHECK(CompactCachedOutbox(&g_outbox_cache)==ESP_FAIL); Reboot(); Latest(false);
    // API write errors invalidate RAM even if full bytes reached the file.
    for(const char* op:{"write:P","flush:P","sync","close:P"}) {
        Reset(); Put(kOutboxPath,{Record("first-request-0001")});
        OutboxScan* scan=nullptr; CHECK(EnsureOutboxCache(&scan)==ESP_OK);
        fail_op=op;
        CHECK(AppendOutboxRecord(Record("latest-request-002"))==ESP_FAIL && !g_outbox_cache_loaded);
        fail_op.clear(); CHECK(EnsureOutboxCache(&scan)==ESP_OK);
        if(scan->partial_tail) {
            CHECK(scan->pending.size()==1);
            CHECK(CompactCachedOutbox(scan)==ESP_OK);
            CHECK(AppendOutboxRecord(Record("latest-request-002"))==ESP_OK);
        }
        Reboot(); Latest(false);
    }
    for(const char* op:{"read:P","close:P"}) {
        Seed(false); fail_op=op; OutboxScan scan;
        CHECK(ScanOutbox(&scan)==ESP_FAIL); CHECK(streams.empty());
    }
#if !WORD_DOMAIN
    // Marker paths must repair a freshly rescanned partial tail before append.
    for(bool park:{false,true}) {
        Seed();
        std::vector<OutboxRecord> records{Record("first-request-0001"),Record("latest-request-002")};
        if(park) { OutboxRecord marker{};
            CHECK(BuildSuspendRecord(records.front().request_id,static_cast<wqn::OutboxSuspendReason>(1),&marker)==ESP_OK);
            records.push_back(marker); }
        Put(kOutboxPath,records,7); Reboot();
        const std::string id="latest-request-002";
        if(park) { SuspendContext c{&id,static_cast<wqn::OutboxSuspendReason>(1)};
                   CHECK(SuspendObservationTransaction(&c)==ESP_OK); }
        else { RequestIdContext c{&id}; CHECK(AckObservationTransaction(&c)==ESP_OK); }
        Reboot(); OutboxScan scan; CHECK(ScanOutbox(&scan)==ESP_OK && !scan.partial_tail);
        CHECK(scan.pending.size()==(park?0:1) && scan.suspended.size()==(park?2:0));
    }
#endif
    Reset();
    std::printf("PASS: %s outbox %u checks; production journal/header + libc faults, NOT HIL\n",
                WORD_DOMAIN?"Word":"Note",checks);
}
