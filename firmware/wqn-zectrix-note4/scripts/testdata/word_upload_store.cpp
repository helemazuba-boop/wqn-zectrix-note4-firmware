#include <limits>
#include <atomic>
using TickType_t=uint32_t;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE 1
#define pdMS_TO_TICKS(x) (x)
@@RETRY_CONSTANTS@@
std::string g_word_outbox_retry_request_id;
OutboxRetryCause g_word_outbox_retry_cause=OutboxRetryCause::kNone;
uint32_t esp_random(){return 0;}
const char* OutboxRetryCauseName(OutboxRetryCause){return "host";}
void PersistOutboxRetryCheckpoint() {++retry_calls;}
@@RETRY@@
std::atomic<bool> g_word_outbox_sync_requested{true},g_outbox_immediate_requested{false};
WordOutboxUploadState g_last_word_outbox_upload_state=WordOutboxUploadState::kDrained;
WordOutboxUploadState g_last_note_outbox_upload_state=WordOutboxUploadState::kDrained;
WordOutboxUploadState g_last_problem_outbox_upload_state=WordOutboxUploadState::kDrained;
TickType_t NoteOutboxRetryWaitDelay(){return portMAX_DELAY;}
TickType_t ProblemOutboxRetryWaitDelay(){return portMAX_DELAY;}
@@WAIT@@
int peek_calls=0,fail_peek_at=0,fail_native_ack_at=0,native_ack_calls=0;
std::vector<uint64_t> sent_sequences;
esp_err_t RunFixtureTransaction(wqn::services::StorageTransaction tx,void* ctx,const char* owner) {
    if(std::strcmp(owner,"word-outbox-peek-batch")==0 && ++peek_calls==fail_peek_at) return ESP_ERR_INVALID_ARG;
    if(std::strcmp(owner,"word-outbox-ack-batch")==0 && ++native_ack_calls==fail_native_ack_at) return ESP_FAIL;
    return tx?tx(ctx):ESP_ERR_INVALID_ARG;
}
@@UPLOAD@@
extern "C" {
esp_err_t WqnHostBuildObservationRecord(const wqn::DurableWordObservation*,void*);
void WqnHostResetWordOutboxCache();
size_t WqnHostOutboxRecordSize();
}
int checks=0,failures=0;
void Check(bool value,const char* label){++checks;failures+=!value;std::printf("%s upload-store: %s\n",value?"PASS":"FAIL",label);}
void Seed(size_t count) {
    WqnHostResetWordOutboxCache();g_cursor_nvs.clear();
    DIR* dir=opendir(g_storage_root.c_str());if(!dir)std::abort();
    while(auto* entry=readdir(dir))if(entry->d_name[0]!='.')std::remove((g_storage_root+"/"+entry->d_name).c_str());
    closedir(dir);
    now_ms=0;ResetWordOutboxRetryBackoff();retry_calls=0;
    attempts=batch_calls=registration_calls=peek_calls=native_ack_calls=0;
    fail_peek_at=fail_native_ack_at=0;
    enable_batch=true;batch_status=200;batch_prefix=-1;bad_receipt=false;
    g_word_interaction_generation=0;g_local_registered_session_id.clear();
    g_last_word_outbox_upload_state=WordOutboxUploadState::kDrained;
    FILE* file=std::fopen("/storage/wout.v1","wb");if(!file)std::abort();
    for(size_t i=0;i<count;++i){
        wqn::DurableWordObservation o;
        o.request_id="upload_regression_"+std::to_string(i);
        o.session_id="33333333-3333-4333-8333-333333333333";
        o.sequence=i;o.item_id="22222222-2222-4222-8222-222222222222";
        o.action=wqn::protocol::word_study_v1::ObservationAction::kShown;
        o.mode=wqn::protocol::word_study_v1::Mode::kSequential;
        o.occurred_at="2026-11-09T04:37:22Z";o.next_position=i;o.next_phase=wqn::WordPresentationPhase::kBack;
        std::vector<uint8_t> bytes(WqnHostOutboxRecordSize());
        if(WqnHostBuildObservationRecord(&o,bytes.data())!=ESP_OK || std::fwrite(bytes.data(),1,bytes.size(),file)!=bytes.size())std::abort();
    }
    if(std::fclose(file)!=0)std::abort();
}
int main(int argc,char**argv){
    if(argc!=2)return 2;
    g_storage_root=argv[1];
    using S=WordOutboxUploadState;
    for(size_t count:{10u,11u,20u,64u}) {
        Seed(count);Check(UploadPendingWordObservations("host")==S::kDrained,"valid real-store batch drains");
        wqn::DurableWordObservation head;
        Check(batch_calls==1 && attempts==0 && wqn::PeekPendingWordObservation(&head)==ESP_ERR_NOT_FOUND,
              "batch receipt and bounded ACKs drain actual durable records");
        WqnHostResetWordOutboxCache();
        Check(wqn::PeekPendingWordObservation(&head)==ESP_ERR_NOT_FOUND,"reloaded journal retains all ACKs");
    }
    Seed(65);Check(UploadPendingWordObservations("host")==S::kPending && batch_calls==1,"64-item round cap leaves one ready record");
    Check(UploadPendingWordObservations("host")==S::kDrained && batch_calls==2,"next round drains record 65");
    Seed(65);std::vector<std::string> excluded;
    for(int i=0;i<64;++i)excluded.push_back("upload_regression_"+std::to_string(i));
    wqn::DurableWordObservation bound;
    Check(wqn::PeekPendingWordObservationExcluding(excluded,&bound)==ESP_OK && bound.sequence==64,
          "read exclusion accepts 64 identities independently of write limit");
    excluded.push_back("upload_regression_64");
    Check(wqn::PeekPendingWordObservationExcluding(excluded,&bound)==ESP_ERR_INVALID_ARG,
          "read exclusion rejects 65 identities without enlarging journal writes");
    Seed(20);fail_native_ack_at=2;
    Check(UploadPendingWordObservations("host")==S::kPending,"second ACK chunk failure remains pending");
    wqn::DurableWordObservation head;
    Check(wqn::PeekPendingWordObservation(&head)==ESP_OK && head.sequence==10 &&
          g_word_outbox_retry_request_id==head.request_id,"retry cursor binds first unacknowledged chunk");
    WqnHostResetWordOutboxCache();
    Check(wqn::PeekPendingWordObservation(&head)==ESP_OK && head.sequence==10,"successful prefix survives native journal reload");
    now_ms=g_word_outbox_retry_not_before_ms;fail_native_ack_at=0;
    Check(UploadPendingWordObservations("host")==S::kDrained,"partial ACK retry drains remaining identities");
    Seed(20);batch_prefix=12;
    Check(UploadPendingWordObservations("host")==S::kPending &&
          wqn::PeekPendingWordObservation(&head)==ESP_OK && head.sequence==12,"partial server receipt only ACKs accepted prefix");
    Seed(20);fail_peek_at=12;
    g_last_word_outbox_upload_state=UploadPendingWordObservations("host");
    Check(g_last_word_outbox_upload_state==S::kPending && batch_calls==0 && native_ack_calls==0,"selection fault sends and ACKs nothing");
    Check(WordOutboxRetryWaitDelay()>=10000 && OutboxWaitDelay()>=10000,"actual scheduler honors local storage backoff after self-request");
    const int reads=peek_calls;
    for(int tick=1;tick<10;++tick){
        now_ms=tick*1000;
        if(OutboxWaitDelay()==0)g_last_word_outbox_upload_state=UploadPendingWordObservations("host");
        Check(OutboxWaitDelay()>0,"self-notify cannot bypass retry deadline");
    }
    Check(peek_calls==reads && batch_calls==0,"scheduler wait incurs no repeated storage/HTTP work");
    now_ms=g_word_outbox_retry_not_before_ms;fail_peek_at=0;
    Check(OutboxWaitDelay()==0 && UploadPendingWordObservations("host")==S::kDrained,"deadline expiry resumes and drains preserved facts");
    std::printf("Word upload/store: %d checks, %d failures (full production store/upload/protocol/retry; libc + worker/transport ports; NOT HIL)\n",checks,failures);
    return failures?1:0;
}
