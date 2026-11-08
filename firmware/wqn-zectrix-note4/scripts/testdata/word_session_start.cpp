#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "device_protocol/word_study.h"
// Calibrated-time port; provenance and epoch handling use the real clock in
// test_learning_time.py. This fixture tests the actual session-start pump.
namespace wqn::runtime {
bool learning_ready=true;
bool IsLearningTimeReady(){return learning_ready;}
}
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NOT_FOUND=3;
constexpr const char* kTag="fixture";
void Log(const char*,const char*,...) {}
const char* esp_err_to_name(int result) {return result==ESP_OK?"ESP_OK":"ESP_FAIL";}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(value) do {(void)(value);} while(false)
@@POLICY@@
namespace wqn {
bool IsWordDeckScopeReady() { return true; }
template<typename T> using WordStorePsramAllocator=std::allocator<T>;
enum class WordPresentationPhase : uint8_t {kFront,kBack};
@@ENUMS@@
@@STORE@@
@@OBSERVATION@@
constexpr size_t kWordObservationOutboxCapacity=1000;
constexpr int kWordNewWordDailyLimit=20;
@@STATE@@
struct WqnWordEntry {std::string word;};
struct WordPackIndex {};
// [deck-autorepair] Real POD fields used by the injected DefaultDeckRepairBlocking.
struct DefaultDeckRepairState {
    bool pending = false;
    uint32_t op_id = 0;
    int64_t retry_after_ms = 0;
    uint32_t attempts = 0;
    uint32_t scope_generation = 0;
    bool must_complete = false;
};
struct WordAppState {
    bool default_deck_manual_pending=false;
    WordAppMode mode=WordAppMode::kSessionStarting;
    WordSessionState session;
    WordSessionChain chain;
    WordSessionChain shuffle_chain;
    WordReviewRuntime review;
    WordOutboxState outbox;
    WordPackIndex pack_index;
    WqnWordEntry current_word;
    DefaultDeckRepairState default_deck_repair;
    bool scope_reset_pending_valid=false;
    bool pending_pack_index_ready=false,pack_index_pinned=false;
    std::string message,scoped_deck_id,default_deck_id;
};
@@BLOCKING@@
bool HasBufferedWordObservations(const WordAppState&);
int card_reads=0,saves=0;
bool IsReviewSession(const PersistedWordSession& s){return s.remote.mode==protocol::word_study_v1::Mode::kReview;}
bool IsSequentialSession(const PersistedWordSession& s){return s.remote.mode==protocol::word_study_v1::Mode::kSequential;}
bool IsIntakeSession(const PersistedWordSession& s){return s.remote.mode==protocol::word_study_v1::Mode::kIntake;}
esp_err_t LoadCurrentReviewWord(WordAppState*){++card_reads;return ESP_OK;}
void ShowStudyCard(WordAppState* s){s->mode=WordAppMode::kWordCard;}
bool PickAnyReplay(const WordReviewRuntime&,size_t*){return false;}
size_t SessionIndexOfOrdinal(const StoredWordSessionData&,uint64_t){return 0;}
uint64_t SessionEndOrdinal(const StoredWordSessionData&){return 0;}
size_t NextQueuePosition(const WordAppState&,size_t p){return p;}
int64_t esp_timer_get_time(){return 0;}
@@SEED@@
void SetStudySessionResumable(WordAppState*,protocol::word_study_v1::Mode,bool){}
void RequestCandidatePageIfNeeded(WordAppState*){}
void ActivatePendingWordPackIndex(WordAppState*){}
void InstallWordPackIndex(WordAppState*,WordPackIndex,std::string){}
void SetWordReviewDueCount(int){}
esp_err_t LoadWordPackIndex(WordPackIndex*){return ESP_OK;}
esp_err_t SaveWordSequentialCursor(uint32_t){++saves;return ESP_OK;}
esp_err_t SavePersistedWordSession(const PersistedWordSession&){++saves;return ESP_OK;}
@@CHAIN@@
@@REQUEST@@
@@CANCEL@@
@@CURSOR@@
@@MUTATION@@
@@BUFFER@@
@@BATCH_RESULT@@
enum class UiScreen {kWord,kOther};
struct UiState {UiScreen screen=UiScreen::kWord;WordAppState word_app;};
int metadata_count=0;
namespace services {
protocol::v3::RequestMetadata MakeDeviceRequestMetadata(){
    protocol::v3::RequestMetadata result;
    result.request_id="request-"+std::to_string(++metadata_count);
    return result;
}
}
}
namespace wqn { uint32_t GetDeckScopeGeneration() { return 7; }
 }
namespace device_ui_internal {
@@CLOUD_REQUEST@@
enum class PersistDomain {kWord};
bool IsPersistDomainBusy(PersistDomain) { return false; }
bool QueueWordCloudRequest(const WordCloudRequest&) { return true; }
enum class PersistKind {kWordObservation};
bool cloud_busy=false,persist_busy=false,queue_allowed=true;
int attempts=0;
std::vector<wqn::protocol::word_study_v1::CreateSessionRequest> queued;
bool IsWordCloudBusy(){return cloud_busy;}
bool IsPersistKindBusy(PersistKind){return persist_busy;}
bool QueueWordSessionStart(const wqn::protocol::word_study_v1::CreateSessionRequest& request, bool = false){
    ++attempts;
    if(!queue_allowed)return false;
    queued.push_back(request);cloud_busy=true;return true;
}
class UiRuntime {
public:
    wqn::UiState state_;
    const wqn::UiState& state() const{return state_;}
    bool TakeWordSessionStartRequest(wqn::protocol::word_study_v1::CreateSessionRequest* request);
    void RestoreWordSessionStartRequest();
    void RequestWordBatchFlush();
};
@@RUNTIME@@
@@FLUSH@@
@@PUMP@@
}
using namespace wqn;
using namespace device_ui_internal;
using Mode=protocol::word_study_v1::Mode;
int passes=0,failures=0;
void Check(bool okay,const char* name){std::printf("%s: %s\n",okay?"PASS":"FAIL",name);okay?++passes:++failures;}
UiRuntime Initial(){
    wqn::runtime::learning_ready=true;
    cloud_busy=persist_busy=false;queue_allowed=true;attempts=0;queued.clear();metadata_count=0;card_reads=saves=0;
    UiRuntime runtime;
    auto& word=runtime.state_.word_app;
    word.chain.active=true;word.chain.sequential_cursor=37;
    word.session.requested_mode=Mode::kIntake;word.session.start_result_expected=true;
    word.session.create_request_id="intake-request";
    word.default_deck_id="aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    return runtime;
}
void EmptyIntake(UiRuntime* r){ApplyWordSessionStartResult(&r->state_.word_app,ESP_OK,ESP_OK,ESP_OK,{});}
void Pending(UiRuntime* r){auto& s=r->state_.word_app.session;s.start_result_expected=false;s.start_requested=true;}
int main(){
    WordReviewRuntime stale_review;stale_review.queue_unavailable=true;
    SeedReviewRuntime(&stale_review,"online-review-again");
    Check(!stale_review.queue_unavailable,"a refreshed online review clears the old offline-queue marker");
    auto runtime=Initial();auto* word=&runtime.state_.word_app;
    EmptyIntake(&runtime);
    wqn::runtime::learning_ready=false;PumpWordSessionStart(&runtime);
    Check(queued.empty() && metadata_count==0 && word->session.start_requested &&
        !word->session.start_result_expected,"uncalibrated clock holds automatic offline continuation without consuming it");
    wqn::runtime::learning_ready=true;
    Check(word->session.start_requested && word->session.requested_mode==Mode::kSequential &&
        word->mode==WordAppMode::kSessionStarting,"empty intake arms the sequential continuation");
    cloud_busy=true;PumpWordSessionStart(&runtime);
    Check(queued.empty() && word->session.start_requested && !word->session.start_result_expected &&
        metadata_count==0,"old cloud result ownership defers without consuming the continuation");
    cloud_busy=false;PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].mode==Mode::kSequential && queued[0].start_index==37 &&
        !word->session.start_requested && word->session.start_result_expected,
        "empty intake proceeds without another button or signature change");
    Check(queued.size()==1 && queued[0].scope.deck_ids==std::vector<std::string>{word->default_deck_id} &&
        queued[0].new_word_limit==0,"sequential start retains the scope and continuation parameters");
    cloud_busy=false;PumpWordSessionStart(&runtime);PumpWordSessionStart(&runtime);
    Check(queued.size()==1,"an accepted start is not duplicated on later UI iterations");

    runtime=Initial();word=&runtime.state_.word_app;
    word->session.start_result_expected=false;word->session.batch_finish_pending=true;
    word->session.persisted.remote.mode=Mode::kIntake;
    word->session.persisted.remote.session_id="old-intake-session";
    for(int n=0;n<2;++n){StoredWordSessionItem item;std::snprintf(item.item_id,sizeof(item.item_id),"intake-%d",n);word->session.persisted.remote.items.push_back(item);}
    word->session.persisted.position=2;
    word->session.buffered_observations.resize(2);
    word->session.batch_in_flight=2;word->session.batch_operation_id=9;
    PumpWordSessionStart(&runtime);
    Check(queued.empty() && !word->session.start_requested,"last intake RAM event cannot replace the SID before flush");
    Check(ApplyWordObservationBatchResult(word,ESP_FAIL,9,1000) &&
        word->session.buffered_observations.size()==2 && word->session.batch_finish_pending &&
        !word->session.start_requested,"failed final flush keeps the chain and original RAM tail");
    word->session.batch_in_flight=2;word->session.batch_operation_id=10;
    Check(ApplyWordObservationBatchResult(word,ESP_OK,10,2000) &&
        word->session.buffered_observations.empty() && word->session.start_requested &&
        word->chain.exclude_ids==std::vector<std::string>({"intake-0","intake-1"}),
        "successful final batch arms sequential and retains intake exclusion IDs");
    PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].mode==Mode::kSequential && saves==0 &&
        word->outbox.pending_count==2,"last intake flush proceeds without UI flash I/O or another press");

    runtime=Initial();word=&runtime.state_.word_app;
    word->session.start_requested=false;word->session.start_result_expected=false;
    word->session.persisted.remote.mode=Mode::kShuffle;
    word->session.persisted.remote.seed="11111111-1111-4111-8111-111111111111";
    word->shuffle_chain.active=true;word->shuffle_chain.segment_finished=true;
    word->shuffle_chain.sequential_cursor=500;word->shuffle_chain.sequential_total=3490;
    word->mode=WordAppMode::kHome;
    PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].mode==Mode::kShuffle && queued[0].start_index==500 &&
        word->chain.sequential_cursor==37,
        "durable random segment completion continues with its own mode and cursor");
    word->session.requested_mode=Mode::kShuffle;
    word->shuffle_chain.segment_finished=true;word->chain.segment_finished=true;
    CancelWordSessionStartResult(word);
    Check(!word->shuffle_chain.segment_finished && word->chain.segment_finished,
        "cancelled random start clears only its continuation and preserves the sequential round");
    runtime=Initial();word=&runtime.state_.word_app;
    word->session.start_requested=true;word->session.requested_mode=Mode::kShuffle;
    word->session.persisted.remote.mode=Mode::kSequential;word->chain.segment_finished=true;word->chain.active=true;
    PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].mode==Mode::kShuffle,
        "explicit random selection is not replaced by a stale sequential continuation");

    runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
    word->session.create_request_id="same-start-request";
    queue_allowed=false;PumpWordSessionStart(&runtime);
    Check(attempts==1 && queued.empty() && word->session.start_requested &&
        !word->session.start_result_expected && word->mode==WordAppMode::kSessionStarting &&
        word->session.create_request_id=="same-start-request","enqueue reject re-arms rather than returning home or losing identity");
    queue_allowed=true;PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].metadata.request_id=="same-start-request" &&
        queued[0].mode==Mode::kIntake,"retry uses the original request ID and mode");

    runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
    word->scoped_deck_id="bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].scope.deck_ids==std::vector<std::string>{word->scoped_deck_id},
        "explicit scoped deck still wins over the default");

    runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
    word->session.buffered_observations.resize(1);
    word->session.persisted.remote.session_id="old-session";
    PumpWordSessionStart(&runtime);
    Check(attempts==0 && word->session.batch_flush_requested && word->session.start_requested &&
        word->session.persisted.remote.session_id=="old-session","pending start forces RAM flush and preserves the old session");

    for(int gate=0;gate<3;++gate){
        runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
        if(gate==0)word->session.observation_effect_ready=true;
        if(gate==1)word->session.commit_state=WordObservationCommitState::kPersisting;
        if(gate==2)persist_busy=true;
        PumpWordSessionStart(&runtime);
        Check(attempts==0 && word->session.start_requested && !word->session.start_result_expected,
            gate==0?"Prepared effect defers start":gate==1?"kPersisting gates the Prepare-to-reserve gap":"worker ownership defers start");
    }

    // [deck-autorepair] NEW gate: the REAL DefaultDeckRepairBlocking defers a
    // new session while a repair is pending / in flight / owes a failed
    // accepted transaction's marker tail (must_complete). Not an always-false
    // stub: exercise each field and the cleared (proceeds) case.
    {
        runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
        word->default_deck_repair.pending=true;
        PumpWordSessionStart(&runtime);
        Check(attempts==0 && word->session.start_requested &&
            !word->session.start_result_expected,
            "pending auto repair defers a new session (real gate)");
        // Clear pending, but an accepted in-flight op still blocks.
        word->default_deck_repair.pending=false;
        word->default_deck_repair.op_id=55;
        PumpWordSessionStart(&runtime);
        Check(attempts==0 && word->session.start_requested,
            "in-flight auto repair op defers a new session (real gate)");
        // A failed accepted transaction's recovery obligation also blocks.
        word->default_deck_repair.op_id=0;
        word->default_deck_repair.must_complete=true;
        PumpWordSessionStart(&runtime);
        Check(attempts==0 && word->session.start_requested,
            "must_complete recovery tail defers a new session (real gate)");
        // Only once the obligation is fully cleared does the start proceed.
        word->default_deck_repair.must_complete=false;
        PumpWordSessionStart(&runtime);
        Check(attempts==1 && queued.size()==1 && !word->session.start_requested,
            "cleared auto repair lets the pending session start (real gate)");
    }

    runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
    runtime.state_.screen=UiScreen::kOther;PumpWordSessionStart(&runtime);
    Check(attempts==0 && word->session.start_requested,"a hidden word page cannot start a new session");
    runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
    word->mode=WordAppMode::kHome;PumpWordSessionStart(&runtime);
    Check(attempts==0,"home/cancelled state cannot be revived by a stale pending flag");
    PumpWordSessionStart(nullptr);Check(attempts==0,"null runtime is a no-op");

    runtime=Initial();word=&runtime.state_.word_app;Pending(&runtime);
    CancelWordSessionStartResult(word);RestoreWordSessionStartRequest(word);PumpWordSessionStart(&runtime);
    Check(attempts==0 && !word->session.start_requested && word->session.create_request_id.empty(),
        "late restore cannot undo a cancellation");
    for(int error=0;error<3;++error){
        runtime=Initial();word=&runtime.state_.word_app;
        PersistedWordSession active;active.active=true;active.remote.items.resize(1);
        ApplyWordSessionStartResult(word,error==0?ESP_FAIL:ESP_OK,
            error==1?ESP_FAIL:ESP_OK,error==2?ESP_FAIL:ESP_OK,active);
        PumpWordSessionStart(&runtime);
        Check(attempts==0 && word->mode==WordAppMode::kHome,
            error==0?"transport failure keeps the existing explicit retry boundary":
            error==1?"compaction failure does not publish a session":"save failure does not publish a session");
    }
    runtime=Initial();word=&runtime.state_.word_app;
    word->session.requested_mode=Mode::kReview;EmptyIntake(&runtime);PumpWordSessionStart(&runtime);
    Check(attempts==0 && word->mode==WordAppMode::kReviewComplete,
        "empty review stays on completion until the user chooses sequential");
    runtime=Initial();word=&runtime.state_.word_app;
    StartIntakeHead(word);PumpWordSessionStart(&runtime);
    Check(queued.size()==1 && queued[0].mode==Mode::kIntake && queued[0].new_word_limit==20 &&
        queued[0].start_index==-1,"initial intake still carries the daily budget without a sequential cursor");
    PersistedWordSession active;active.active=true;active.remote.mode=Mode::kIntake;
    active.remote.items.resize(1);
    Check(ApplyWordSessionStartResult(word,ESP_OK,ESP_OK,ESP_OK,active) &&
        word->mode==WordAppMode::kWordCard && card_reads==1 && saves==0,
        "nonempty already-durable session still installs its first card without a UI snapshot write");
    // Cursor ACK owns the visible transition; stale or failed ACK cannot
    // change the card, SID, generation or exact pending pause intent.
    runtime=Initial();word=&runtime.state_.word_app;
    word->mode=WordAppMode::kWordCard;
    word->session.persisted.active=true;
    word->session.persisted.remote.session_id="pause-session";
    word->session.persisted.deck_scope_generation=7;
    word->session.persisted.phase=WordPresentationPhase::kBack;
    auto arm_pause=[&](){
        auto& s=word->session;
        s.cursor_intent_armed=true;s.cursor_intent_paused=true;
        s.cursor_intent_operation_id=0;s.cursor_intent_session=s.persisted;
        s.cursor_intent_session.paused=true;
    };
    arm_pause();
    Check(WordMutationBlockedByPendingWorker(*word),"pending cursor intent gates new word mutations");
    PersistedWordSession intent;
    Check(TakeWordSessionCursorEffect(word,10,&intent) && intent.paused &&
        intent.phase==WordPresentationPhase::kBack,"pause worker receives the exact back-phase snapshot");
    Check(!TakeWordSessionCursorEffect(word,11,&intent),"one cursor intent has only one worker owner");
    Check(!ApplyWordSessionCursorResult(word,9,ESP_OK) && !word->session.persisted.paused &&
        word->mode==WordAppMode::kWordCard,"stale operation ACK cannot publish pause");
    Check(ApplyWordSessionCursorResult(word,10,ESP_FAIL) &&
        word->session.cursor_intent_armed && word->session.cursor_awaiting_retry_confirm &&
        !word->session.persisted.paused && word->mode==WordAppMode::kWordCard,
        "failed pause preserves intent and requires retry confirmation");
    word->session.cursor_awaiting_retry_confirm=false;
    Check(TakeWordSessionCursorEffect(word,11,&intent) &&
        ApplyWordSessionCursorResult(word,11,ESP_OK) && word->session.persisted.paused &&
        !word->session.cursor_intent_armed && word->mode==WordAppMode::kHome,
        "successful retry ACK publishes durable pause and returns home");
    Check(!ApplyWordSessionCursorResult(word,11,ESP_OK),"duplicate cursor ACK is ignored");
    for(int mismatch=0;mismatch<2;++mismatch){
        arm_pause();TakeWordSessionCursorEffect(word,20,&intent);
        if(mismatch==0)word->session.persisted.remote.session_id="replacement-session";
        else ++word->session.persisted.deck_scope_generation;
        Check(!ApplyWordSessionCursorResult(word,20,ESP_OK) && !word->session.cursor_intent_armed,
            mismatch==0?"replaced SID rejects cursor ACK":"changed scope generation rejects cursor ACK");
    }
    arm_pause();word->session.buffered_observations.resize(1);
    Check(!TakeWordSessionCursorEffect(word,30,&intent),"RAM observations must flush before cursor worker ownership");
    std::printf("%d PASS / %d FAIL (production start/chain/batch/pump, host cloud/card/I/O seams; NOT HIL)\n",passes,failures);
    return failures?1:0;
}
