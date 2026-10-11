#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=1, ESP_ERR_INVALID_STATE=2;
constexpr char kTag[]="test";
bool owner=false, lease=false, deleted=false, fail_remove=false, fail_open=false;
bool fail_flush=false, fail_sync=false, fail_close=false, reject_quiesce=false;
unsigned outside=0, no_lease=0, opens=0, removes=0, flushes=0, syncs=0, closes=0, writes=0;
unsigned passed=0, failed=0;
std::vector<std::string> dispatches;
template<typename... Args> void Log(const char*, const char*, Args&&...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
const char* esp_err_to_name(int result) { return result==ESP_OK?"ESP_OK":"ESP_FAIL"; }
int64_t esp_timer_get_time() { static int64_t ticks=0; return ++ticks; }
void vTaskDelay(unsigned) {}
void vTaskDelete(void*) { deleted=true; }
constexpr unsigned kBenchStartDelayTicks=1;
constexpr int kBenchRounds=2, kBenchStreamRounds=2;
constexpr int64_t kBenchStreamAbortMs=6000;
constexpr char kBenchPrimary[]="/private/bench.pri", kBenchTemp[]="/private/bench.tmp";
constexpr char kBenchBackup[]="/private/bench.bak", kBenchAppend[]="/private/bench.append";
constexpr char kBenchStream[]="/private/bench.stream";
FILE* g_bench_stream=nullptr;
struct BenchShape { const char* name; size_t bytes; bool stream; };
constexpr BenchShape kBenchShapes[]={{"probe",0,false},{"stream",0,true}};
struct BenchContext { const BenchShape* shape=nullptr; int round=0; };
bool BenchFreeSpaceOk(const BenchShape&, int, int64_t* info) { *info=0; return true; }
int esp_spiffs_info(const char*, size_t* total, size_t* used) { *total=1000000; *used=0; return ESP_OK; }
void Io() { if (!owner) ++outside; }
int TestRemove(const char*) { Io(); ++removes; errno=fail_remove?EACCES:ENOENT; return -1; }
FILE* TestOpen(const char*, const char*) { Io(); ++opens; return fail_open?nullptr:reinterpret_cast<FILE*>(1); }
int TestFlush(FILE*) { Io(); ++flushes; return fail_flush?-1:0; }
int TestSync(int) { Io(); ++syncs; return fail_sync?-1:0; }
int TestClose(FILE*) { Io(); ++closes; return fail_close?-1:0; }
int TestFileNo(FILE*) { return 1; }
int BenchTransaction(void* context) {
    if (static_cast<BenchContext*>(context)->shape->stream) { Io(); ++writes; }
    return ESP_OK;
}
namespace wqn::runtime {
enum class SleepBlocker { kStorage };
class SleepLease {
public:
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int) {
        lease=!reject_quiesce; return SleepLease();
    }
    explicit operator bool() const { return lease; }
    void Reset() { lease=false; }
};
}
namespace wqn::services {
int ExecuteStorageTransactionNamed(int (*call)(void*), void* context, const char* name) {
    if (!lease) ++no_lease;
    dispatches.emplace_back(name);
    const bool saved=owner; owner=true;
    const int result=call(context); owner=saved;
    return result;
}
}
@@PRODUCTION@@
void Check(bool value, const char* name) {
    std::printf("%s: %s\n", value?"PASS":"FAIL", name); ++(value?passed:failed);
}
int main() {
    BenchTask(nullptr);
    Check(outside==0,"actual bench controller never mutates outside storage owner");
    Check(no_lease==0,"controller retains storage lease across every dispatched wait");
    Check(opens==1 && closes==1 && flushes==1 && syncs==1,"one held stream has one open/flush/sync/close");
    Check(writes==kBenchStreamRounds,"controller keeps per-round owner dispatch");
    Check(removes==6,"scratch begin and final five-path cleanup are owner operations");
    Check(dispatches.size()==11,"begin/rounds/commit/cleanup remain separate transactions");
    Check(g_bench_stream==nullptr && !lease && deleted,"normal end releases stream and lease before task deletion");
    owner=true;
    bool aborted=false;
    Check(BenchStreamCommitTransaction(&aborted)==ESP_ERR_INVALID_ARG,"empty stream commit rejects without I/O");
    fail_remove=true;
    const unsigned before_open=opens;
    Check(BenchStreamBeginTransaction(nullptr)==ESP_FAIL && opens==before_open,"remove error cannot truncate scratch anyway");
    fail_remove=false; fail_open=true;
    Check(BenchStreamBeginTransaction(nullptr)==ESP_FAIL && g_bench_stream==nullptr,"open error is reported");
    fail_open=false;
    Check(BenchStreamBeginTransaction(nullptr)==ESP_OK,"missing scratch file is harmless on begin");
    Check(BenchStreamBeginTransaction(nullptr)==ESP_ERR_INVALID_STATE,"already held stream is not overwritten/leaked");
    Check(BenchStreamCommitTransaction(nullptr)==ESP_ERR_INVALID_ARG && g_bench_stream!=nullptr,"invalid commit context does not discard held handle");
    fail_flush=true; const unsigned before_close=closes, before_sync=syncs;
    Check(BenchStreamCommitTransaction(&aborted)==ESP_FAIL && closes==before_close+1 && syncs==before_sync && g_bench_stream==nullptr,"flush failure still closes and clears handle");
    fail_flush=false; fail_sync=true; BenchStreamBeginTransaction(nullptr);
    Check(BenchStreamCommitTransaction(&aborted)==ESP_FAIL && g_bench_stream==nullptr,"sync failure is reported without leaked handle");
    fail_sync=false; fail_close=true; BenchStreamBeginTransaction(nullptr);
    Check(BenchStreamCommitTransaction(&aborted)==ESP_FAIL && g_bench_stream==nullptr,"close failure remains error");
    fail_close=false;
    Check(BenchRemoveTransaction(nullptr)==ESP_ERR_INVALID_ARG,"null cleanup target rejected");
    Check(BenchRemoveTransaction(const_cast<char*>(kBenchStream))==ESP_OK,"ENOENT cleanup is idempotent");
    fail_remove=true;
    Check(BenchRemoveTransaction(const_cast<char*>(kBenchStream))==ESP_FAIL,"other cleanup errors are not success");
    fail_remove=false; owner=false; reject_quiesce=true; dispatches.clear(); deleted=false;
    BenchTask(nullptr);
    Check(dispatches.empty() && deleted && !lease,"quiesce rejection performs no scratch write or dispatch");
    std::printf("%u PASS / %u FAIL (actual controller/lifecycle, host owner/I-O seams; NOT HIL)\n",passed,failed);
    return failed?1:0;
}
