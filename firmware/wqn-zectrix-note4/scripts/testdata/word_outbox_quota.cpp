// Production packed records, scanner and writer; real temporary libc files.
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
using esp_err_t = int;
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=0x102,
    ESP_ERR_INVALID_STATE=0x103, ESP_ERR_INVALID_SIZE=0x104,
    ESP_ERR_NOT_FOUND=0x105, ESP_ERR_INVALID_CRC=0x109, ESP_ERR_INVALID_RESPONSE=0x108;
constexpr const char* kTag="quota-fixture";
@@BATCH_ENABLED@@
void FixtureLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const auto fixture_error=(expr); \
    if (fixture_error!=ESP_OK) return fixture_error; } while(false)
const char* esp_err_to_name(esp_err_t) { return "fixture"; }
namespace wqn::protocol::word_study_v1 {
enum class Mode : uint8_t { kSequential, kRandom, kDictionary, kReview, kIntake, kShuffle, kMistakes };
enum class ObservationAction : uint8_t { kUnknown, kKnown, kSkipped, kLookedUp };
}
namespace wqn::protocol::v3 { constexpr uint64_t kMaxSafeJsonInteger=9007199254740991ULL; }
namespace wqn {
template<typename T> using WordStorePsramAllocator=std::allocator<T>;
constexpr size_t kWordObservationOutboxCapacity=1000;
enum class WordPresentationPhase : uint8_t { kFront, kBack };
enum class OutboxSuspendReason : uint8_t { kProtocol=1 };
struct DurableWordObservation {
    std::string request_id, session_id, item_id, occurred_at;
    uint64_t sequence=0; uint32_t next_position=0;
    protocol::word_study_v1::Mode mode=protocol::word_study_v1::Mode::kReview;
    protocol::word_study_v1::ObservationAction action=protocol::word_study_v1::ObservationAction::kKnown;
    WordPresentationPhase next_phase=WordPresentationPhase::kFront;
};
}
constexpr size_t kRuntimeCompactAckThreshold=32;
constexpr uint32_t kOutboxMagic=0x424f5157;
constexpr uint16_t kOutboxSchemaVersion=1;
@@RECORDS@@
OutboxScan g_outbox_cache;
bool g_outbox_cache_loaded=false;
char kOutboxPath[256], kOutboxTempPath[256], kOutboxBackupPath[256], direct_path[256];
bool fail_stat=false, fail_read=false, fail_flush=false, fail_sync=false, fail_close=false, partial_write=false;
size_t partial_write_bytes=7;
int write_calls=0, flush_calls=0, sync_calls=0, close_calls=0;
int rename_call=0, fail_rename_call=0;
int64_t fixture_clock=0;
int64_t esp_timer_get_time() { return ++fixture_clock; }
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* data, uint32_t size) {
    for(uint32_t i=0;i<size;++i) {
        crc^=data[i];
        for(int bit=0;bit<8;++bit) crc=(crc>>1) ^ ((crc&1) ? 0xedb88320U : 0U);
    }
    return crc;
}
int FixtureStat(int fd, struct stat* info) {
    if(fail_stat) { errno=EIO; return -1; }
    return ::fstat(fd,info);
}
size_t FixtureRead(void* data, size_t size, size_t count, FILE* file) {
    if(fail_read) {
        fail_read=false;
        // Force fread's EBADF without making fclose fail as well: otherwise
        // the close guard would mask removal of the read-error guard.
        const int fd=fileno(file), writable=::open(kOutboxPath,O_WRONLY);
        if(writable<0 || ::dup2(writable,fd)<0) std::abort();
        if(writable!=fd) ::close(writable);
    }
    return std::fread(data,size,count,file);
}
size_t FixtureWrite(const void* data, size_t size, size_t count, FILE* file) {
    ++write_calls;
    if(partial_write) { partial_write=false; return std::fwrite(data,size,std::min(count,partial_write_bytes),file); }
    return std::fwrite(data,size,count,file);
}
int FixtureFlush(FILE* file) { ++flush_calls; if(fail_flush) { errno=EIO; return EOF; } return std::fflush(file); }
int FixtureSync(int fd) { ++sync_calls; if(fail_sync) { errno=EIO; return -1; } return ::fsync(fd); }
int FixtureClose(FILE* file) { ++close_calls; const int result=std::fclose(file); return fail_close ? EOF : result; }
int FixtureRename(const char* from, const char* to) {
    if(++rename_call==fail_rename_call) { errno=EIO; return -1; }
    return std::rename(from,to);
}
namespace std {
using ::FixtureRead; using ::FixtureWrite; using ::FixtureFlush; using ::FixtureClose; using ::FixtureRename;
}
#define fstat FixtureStat
#define fread FixtureRead
#define fwrite FixtureWrite
#define fflush FixtureFlush
#define fsync FixtureSync
#define fclose FixtureClose
#define rename FixtureRename
@@UTILITIES@@
@@VALID_MODE@@
@@READ@@
@@QUOTA@@
@@APPEND_TO@@
@@WRITE@@
#undef fstat
#undef fread
#undef fwrite
#undef fflush
#undef fsync
#undef fclose
#undef rename
int passes=0, failures=0;
void Check(bool ok, const char* name) { std::printf("%s: %s\n",ok?"PASS":"FAIL",name); ok?++passes:++failures; }
size_t Size(const char* path) { struct stat info={}; return ::stat(path,&info)==0 ? size_t(info.st_size) : 0; }
void Reset() {
    fail_stat=fail_read=fail_flush=fail_sync=fail_close=partial_write=false;
    partial_write_bytes=7;
    write_calls=flush_calls=sync_calls=close_calls=0;
    rename_call=fail_rename_call=0;
    g_outbox_cache={}; g_outbox_cache_loaded=false;
    for(const char* path : {kOutboxPath,kOutboxTempPath,kOutboxBackupPath,direct_path}) std::remove(path);
}
OutboxRecord Record(const std::string& request="request-1") {
    wqn::DurableWordObservation value;
    value.request_id=request; value.session_id="00000000-0000-0000-0000-000000000001";
    value.item_id="00000000-0000-0000-0000-000000000002"; value.occurred_at="2026-10-09T00:00:00Z";
    value.next_position=11;
    OutboxRecord record={};
    if(BuildObservationRecord(value,OutboxRecordKind::kObservation,&record)!=ESP_OK) std::abort();
    return record;
}
void WriteRecords(const char* path, const OutboxRecord& record, size_t count, size_t tail=0) {
    FILE* file=std::fopen(path,"wb"); if(!file) std::abort();
    for(size_t i=0;i<count;++i) if(std::fwrite(&record,1,sizeof(record),file)!=sizeof(record)) std::abort();
    if(tail && std::fwrite(&record,1,tail,file)!=tail) std::abort();
    if(std::fclose(file)!=0) std::abort();
}
int main() {
    char directory[]="/tmp/wqn-outbox-quota-XXXXXX";
    if(!mkdtemp(directory)) return 2;
    std::snprintf(kOutboxPath,sizeof(kOutboxPath),"%s/wout.v1",directory);
    std::snprintf(kOutboxTempPath,sizeof(kOutboxTempPath),"%s/wout.tmp",directory);
    std::snprintf(kOutboxBackupPath,sizeof(kOutboxBackupPath),"%s/wout.bak",directory);
    std::snprintf(direct_path,sizeof(direct_path),"%s/direct.log",directory);
    const auto one=Record(), two=Record("request-2");
    Check(sizeof(OutboxRecord)==200 && kOutboxMaxRecords==2064 && kOutboxMaxBytes==412800,
          "physical byte budget uses the actual packed production record, not the replay mock layout");
    Reset(); WriteRecords(kOutboxPath,one,kOutboxMaxRecords-1);
    size_t bytes=0; int64_t open_ms=0;
    Check(AppendOutboxRecord(two,&open_ms,&bytes)==ESP_OK && bytes==200 && Size(kOutboxPath)==412800,
          "physical append admits the last complete record at the exact byte boundary");
    g_outbox_cache_loaded=true; bytes=999;
    Check(AppendOutboxRecord(two,&open_ms,&bytes)==ESP_ERR_INVALID_SIZE && bytes==0 &&
          Size(kOutboxPath)==412800 && !g_outbox_cache_loaded,
          "physical full-file refusal writes zero bytes and invalidates stale cache");
    Reset(); WriteRecords(kOutboxPath,one,1,7); g_outbox_cache_loaded=true;
    Check(AppendOutboxRecord(two)==ESP_ERR_INVALID_SIZE && Size(kOutboxPath)==207 && !g_outbox_cache_loaded,
          "a partial physical tail cannot be extended with another misaligned record");
    Reset(); WriteRecords(kOutboxPath,one,1); fail_stat=true; g_outbox_cache_loaded=true;
    Check(AppendOutboxRecord(two)==ESP_FAIL && Size(kOutboxPath)==200 && !g_outbox_cache_loaded,
          "physical-size read failure is not permission to append");
    Reset(); WriteRecords(kOutboxPath,one,kOutboxMaxRecords+1);
    WriteRecords(kOutboxBackupPath,two,1); OutboxScan scan;
    Check(ScanOutbox(&scan)==ESP_OK && scan.total_records==2065 && !scan.backup_source &&
          scan.pending.size()==1 && std::strcmp(scan.pending[0].request_id,"request-1")==0,
          "valid oversized legacy primary remains readable and never falls back solely because of quota");
    Reset(); WriteRecords(kOutboxPath,one,1,7);
    Check(ScanOutboxFile(kOutboxPath,&scan)==ESP_OK && scan.total_records==1 && scan.partial_tail &&
          scan.pending.size()==1,
          "a genuine short tail retains the preceding complete observation");
    Reset(); WriteRecords(kOutboxPath,one,1); fail_read=true;
    Check(ScanOutboxFile(kOutboxPath,&scan)==ESP_FAIL,
          "libc read I/O error is not a successful torn-tail scan");
    Reset(); WriteRecords(kOutboxPath,one,1); fail_close=true;
    Check(ScanOutboxFile(kOutboxPath,&scan)==ESP_FAIL,
          "scan close error is propagated instead of publishing unchecked counters");
    Reset(); WriteRecords(kOutboxPath,one,1); partial_write=true; g_outbox_cache_loaded=true;
    Check(AppendOutboxRecord(two)==ESP_FAIL && Size(kOutboxPath)==207 && !g_outbox_cache_loaded,
          "partial append failure invalidates counters even when close flushes its short payload");
    OutboxScan* cached=nullptr;
    Check(EnsureOutboxCache(&cached)==ESP_OK && cached->total_records==1 && cached->partial_tail &&
          cached->pending.size()==1,
          "next operation rescans the actual partial file instead of trusting old cached counts");
    for(int fault=0;fault<3;++fault) {
        Reset(); WriteRecords(kOutboxPath,one,1); g_outbox_cache_loaded=true;
        fail_flush=fault==0; fail_sync=fault==1; fail_close=fault==2;
        const bool failed=AppendOutboxRecord(two)==ESP_FAIL && !g_outbox_cache_loaded;
        fail_flush=fail_sync=fail_close=false;
        Check(failed && EnsureOutboxCache(&cached)==ESP_OK && cached->total_records==2 &&
              cached->pending.size()==2,
              "flush/sync/close failure rescans a complete physically written record for idempotent retry");
    }
    Reset(); WriteRecords(kOutboxPath,one,1); WriteRecords(kOutboxTempPath,two,1);
    std::vector<OutboxRecord,wqn::WordStorePsramAllocator<OutboxRecord>> too_many(kOutboxMaxRecords+1,two);
    Check(CompactOutbox(too_many)==ESP_ERR_INVALID_SIZE && Size(kOutboxPath)==200 && Size(kOutboxTempPath)==200,
          "oversized rewrite is rejected before truncating any temporary or primary file");
    for(int fault=1;fault<=2;++fault) {
        Reset(); WriteRecords(kOutboxPath,one,1); g_outbox_cache_loaded=true;
        g_outbox_cache.pending.push_back(two); fail_rename_call=fault;
        const bool failed=CompactCachedOutbox(&g_outbox_cache)==ESP_FAIL && !g_outbox_cache_loaded;
        fail_rename_call=0;
        Check(failed && EnsureOutboxCache(&cached)==ESP_OK && cached->pending.size()==1 &&
              std::strcmp(cached->pending[0].request_id,"request-1")==0,
              "replacement/rollback failure invalidates cache and reloads the retained primary generation");
    }
    Reset(); WriteRecords(kOutboxPath,one,1); g_outbox_cache.pending.push_back(two);
    g_outbox_cache.suspended.push_back(one); g_outbox_cache.suspended_reasons.push_back(1);
    Check(CompactCachedOutbox(&g_outbox_cache)==ESP_OK && Size(kOutboxPath)==600 &&
          ScanOutboxFile(kOutboxPath,&scan)==ESP_OK && scan.pending.size()==1 && scan.suspended.size()==1 &&
          scan.total_records==3 && scan.suspended_reasons[0]==1,
          "bounded rewrite preserves pending payload plus parked payload and its marker reason");
#if FIXTURE_BATCH
    std::vector<OutboxRecord> batch;
    for(int i=0;i<10;++i) batch.push_back(Record("batch-"+std::to_string(i)));
    Reset(); bytes=999;
    const auto batch_result=AppendOutboxRecords(batch.data(),5,&open_ms,&bytes);
    const bool one_flush=write_calls==1 && flush_calls==1 && sync_calls==1 && close_calls==1;
    Check(batch_result==ESP_OK && bytes==1000 && one_flush &&
          ScanOutboxFile(kOutboxPath,&scan)==ESP_OK && scan.pending.size()==5 &&
          std::strcmp(scan.pending[4].request_id,"batch-4")==0,
          "five distinct CRC records share exactly one write/flush/sync/close");
    Reset();
    Check(AppendOutboxRecords(batch.data(),10)==ESP_OK && Size(kOutboxPath)==2000,
          "maximum ten-event batch is admitted");
    for(int invalid=0;invalid<4;++invalid) {
        Reset(); bytes=999;
        const auto result=AppendOutboxRecordsTo(invalid==0?nullptr:kOutboxPath,
            invalid==1?nullptr:batch.data(),invalid==2?0:invalid==3?11:5,&open_ms,&bytes,kOutboxMaxBytes);
        Check(result==ESP_ERR_INVALID_ARG && bytes==0 && write_calls==0 && !FileExists(kOutboxPath),
              "invalid batch is refused before creating a journal");
    }
    Reset(); WriteRecords(kOutboxPath,one,kOutboxMaxRecords-5);
    Check(AppendOutboxRecords(batch.data(),5)==ESP_OK && Size(kOutboxPath)==kOutboxMaxBytes,
          "entire batch fits the exact physical byte boundary");
    Reset(); WriteRecords(kOutboxPath,one,kOutboxMaxRecords-4); g_outbox_cache_loaded=true; bytes=999;
    Check(AppendOutboxRecords(batch.data(),5,nullptr,&bytes)==ESP_ERR_INVALID_SIZE &&
          bytes==0 && write_calls==0 && !g_outbox_cache_loaded &&
          Size(kOutboxPath)==kOutboxMaxBytes-800,
          "batch quota checks all records before writing any prefix");
    Reset(); WriteRecords(kOutboxPath,one,1,7); g_outbox_cache_loaded=true;
    Check(AppendOutboxRecords(batch.data(),5)==ESP_ERR_INVALID_SIZE && write_calls==0 &&
          !g_outbox_cache_loaded && Size(kOutboxPath)==207,
          "batch cannot append over an existing torn tail");
    Reset(); WriteRecords(kOutboxPath,one,1); fail_stat=true; g_outbox_cache_loaded=true;
    Check(AppendOutboxRecords(batch.data(),5)==ESP_FAIL && write_calls==0 &&
          !g_outbox_cache_loaded && Size(kOutboxPath)==200,
          "batch physical-size failure is not permission to write");
    for(int prefix=0;prefix<5;++prefix) {
        Reset(); partial_write=true; partial_write_bytes=size_t(prefix)*200+7;
        g_outbox_cache_loaded=true; bytes=999;
        const bool failed=AppendOutboxRecords(batch.data(),5,nullptr,&bytes)==ESP_FAIL &&
            !g_outbox_cache_loaded && bytes==size_t(prefix)*200+7 && sync_calls==0;
        Check(failed && EnsureOutboxCache(&cached)==ESP_OK && cached->partial_tail &&
              cached->pending.size()==size_t(prefix) && cached->total_records==size_t(prefix),
              "short batch rescans the complete prefix and rejects the torn record");
    }
    for(int fault=0;fault<3;++fault) {
        Reset(); g_outbox_cache_loaded=true;
        fail_flush=fault==0; fail_sync=fault==1; fail_close=fault==2;
        const bool failed=AppendOutboxRecords(batch.data(),5)==ESP_FAIL && !g_outbox_cache_loaded;
        fail_flush=fail_sync=fail_close=false;
        Check(failed && EnsureOutboxCache(&cached)==ESP_OK && cached->pending.size()==5,
              "batch flush/sync/close failure preserves identities for a rescan-based retry");
    }
#else
    Check(false,"bounded batch append is implemented, not a single-record loop");
#endif
    Reset();
    if(::rmdir(directory)!=0) return 2;
    std::printf("%d PASS / %d FAIL (production libc files/fault seams; NOT ESP32 power-loss HIL)\n",passes,failures);
    return failures?1:0;
}
