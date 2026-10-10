#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <vector>
#define CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE 1
using TickType_t=uint32_t;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr uint32_t portTICK_PERIOD_MS=1;
#define pdMS_TO_TICKS(value) (value)
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
void Log(const char*,const char*,...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
constexpr const char* kTag="host-only";
constexpr uint32_t kFullRetryMagic=1,kClaimRetryBaseMs=15000;
constexpr std::time_t kMinScheduleUnixTime=1704067200;
int g_periodic_schedule_lock=0;
uint8_t g_full_sync_retry_attempts=0;
uint32_t g_full_sync_retry_magic=0,g_control_retry_after_ms=0;
int64_t g_full_sync_retry_unix_seconds=0,now_ms=0;
std::atomic<int64_t> g_full_sync_retry_not_before_ms{0};
bool g_claim_active=false;
std::time_t CurrentUnixSeconds() { return 1705000000+now_ms/1000; }
int64_t esp_timer_get_time() { return now_ms*1000; }
uint32_t esp_random() { return 0; }
uint32_t ClaimPollDelayMs() { return 10000; }
uint32_t AddClaimJitter(uint32_t x) { return x; }
void PersistFullSyncRetryCheckpoint() {}
int outbox_saves=0;
void PersistOutboxRetryCheckpoint() { ++outbox_saves; }
enum class OutboxRetryCause:uint8_t { kNone,kTransport,kServer,kLocalStorage };
const char* OutboxRetryCauseName(OutboxRetryCause) { return "mock"; }
std::string g_word_outbox_retry_request_id,g_note_outbox_retry_request_id,g_problem_outbox_retry_request_id;
int64_t g_word_outbox_retry_not_before_ms=0,g_note_outbox_retry_not_before_ms=0,g_problem_outbox_retry_not_before_ms=0;
uint8_t g_word_outbox_retry_attempts=0,g_note_outbox_retry_attempts=0,g_problem_outbox_retry_attempts=0;
OutboxRetryCause g_word_outbox_retry_cause=OutboxRetryCause::kNone,g_note_outbox_retry_cause=OutboxRetryCause::kNone,g_problem_outbox_retry_cause=OutboxRetryCause::kNone;
@@PRODUCTION@@
int checks=0,failures=0;
void Check(bool okay,const char* label) {
    ++checks; failures+=!okay;
    std::printf("%s: %s\n",okay?"PASS":"FAIL",label);
}
int main() {
    std::vector<uint32_t> delays;
    for(int i=0;i<6;++i) {
        AdmitFull(i==0?static_cast<uint32_t>(kFullSyncManual):0u,i>0,false);
        const auto delay=FullSyncFailureRetryMs(true);
        delays.push_back(delay); ScheduleFullSyncRetry(delay); now_ms+=delay;
    }
    Check(delays==std::vector<uint32_t>({10000,30000,60000,300000,900000,900000}),
        "automatic dispatch preserves the full retry ladder");
    for(const auto reason:{kFullSyncBoot,kFullSyncContentRefresh}) {
        g_full_sync_retry_attempts=3; AdmitFull(reason,false,false);
        Check(g_full_sync_retry_attempts==3,"boot/content admission preserves restored attempt");
    }
    g_full_sync_retry_attempts=3; AdmitFull(0,false,true);
    Check(g_full_sync_retry_attempts==3,"periodic admission does not reset failure attempt");
    for(const auto reason:{kFullSyncManual,kFullSyncCredentials}) {
        g_full_sync_retry_attempts=3; AdmitFull(reason,false,false);
        Check(g_full_sync_retry_attempts==0,"manual/credential admission explicitly resets attempt");
    }
    g_full_sync_retry_attempts=250; bool capped=true;
    for(int i=0;i<300;++i) capped=capped && FullSyncFailureRetryMs(true)==900000;
    Check(capped && g_full_sync_retry_attempts==UINT8_MAX,"long failure streak saturates without counter wrap");
    g_control_retry_after_ms=4000;
    Check(FullSyncFailureRetryMs(true)==4000 && g_control_retry_after_ms==0,"server retry-after override is consumed");
    Check(FullSyncFailureRetryMs(false)==15000,"no-token claim retains its separate retry policy");
    struct Case {
        void (*schedule)(const std::string&,uint32_t,OutboxRetryCause);
        void (*reset)(); bool (*deferred)(const std::string&,int64_t);
        int64_t* deadline; uint8_t* attempts;
    };
    const Case cases[]={
        {ScheduleWordOutboxRetry,ResetWordOutboxRetryBackoff,WordOutboxRetryDeferred,&g_word_outbox_retry_not_before_ms,&g_word_outbox_retry_attempts},
        {ScheduleNoteOutboxRetry,ResetNoteOutboxRetryBackoff,NoteOutboxRetryDeferred,&g_note_outbox_retry_not_before_ms,&g_note_outbox_retry_attempts},
        {ScheduleProblemOutboxRetry,ResetProblemOutboxRetryBackoff,ProblemOutboxRetryDeferred,&g_problem_outbox_retry_not_before_ms,&g_problem_outbox_retry_attempts}
    };
    for(const auto& c:cases) {
        delays.clear();
        for(int i=0;i<6;++i) {
            c.schedule("same-id",0,OutboxRetryCause::kTransport);
            delays.push_back(static_cast<uint32_t>(*c.deadline-now_ms));
            now_ms=*c.deadline;
            Check(!c.deferred("same-id",now_ms) && *c.attempts==i+1,"outbox due check preserves same-ID attempt");
        }
        Check(delays==std::vector<uint32_t>({30000,60000,120000,240000,300000,300000}),"outbox ladder is unaffected");
        c.reset(); const int saved=outbox_saves; c.reset();
        Check(outbox_saves==saved,"unchanged empty outbox clear does not save");
        c.schedule("new-id",180000,OutboxRetryCause::kServer);
        Check(*c.deadline-now_ms==180000 && *c.attempts==1,"new outbox identity resets attempt and respects server delay");
    }
    std::printf("%d PASS / %d FAIL (production retry paths; NOT HIL)\n",checks-failures,failures);
    return failures?1:0;
}
