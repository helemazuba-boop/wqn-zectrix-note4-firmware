#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101;
constexpr const char* kTag="fixture";
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
@@POLICY@@
namespace wqn {
namespace protocol::word_study_v1 {
enum class Mode { kReview,kIntake,kSequential };
enum class ObservationAction { kKnown,kUnknown,kSkipped,kRevealed };
}
using Mode=protocol::word_study_v1::Mode;
using Action=protocol::word_study_v1::ObservationAction;
enum class WordPresentationPhase { kFront,kBack };
enum class WordCardPhase { kFront,kRevealed,kPersisting };
enum class WordAppMode { kHome,kSessionStarting,kWordCard,kReviewComplete };
enum class WordObservationCommitState { kIdle,kPersisting,kCloudPending,kFailed,kBuffered };
constexpr size_t kWordObservationOutboxCapacity=1000;
struct PersistedWordSession {
    struct Item { uint64_t ordinal=0; };
    struct Remote { std::string session_id; uint64_t next_sequence=0; Mode mode=Mode::kReview;
        std::vector<Item> items; bool has_more=false; } remote;
    uint32_t position=0;
    bool active=true,paused=false;
    WordPresentationPhase phase=WordPresentationPhase::kFront;
};
struct DurableWordObservation {
    std::string request_id,occurred_at,session_id; uint64_t sequence=0;
    uint32_t next_position=0; WordPresentationPhase next_phase=WordPresentationPhase::kFront;
    Mode mode=Mode::kReview; Action action=Action::kKnown;
};
@@SESSION@@
struct WordAppState {
    WordSessionState session;
    WordOutboxState outbox;
    struct Review { std::vector<int> pool; } review;
    WordCardPhase card_phase=WordCardPhase::kFront;
    WordAppMode mode=WordAppMode::kWordCard;
    uint32_t reviewed_today=0,correct_today=0;
    std::string message;
};
bool SetSessionCursorOrdinal(PersistedWordSession* session,uint32_t ordinal) {
    for(size_t i=0;i<session->remote.items.size();++i) if(session->remote.items[i].ordinal==ordinal) {
        session->position=uint32_t(i); return true;
    }
    if(!session->remote.items.empty() && ordinal==session->remote.items.back().ordinal+1) {
        session->position=uint32_t(session->remote.items.size()); return true;
    }
    return false;
}
WordCardPhase CardPhaseFromSession(const PersistedWordSession& session) {
    return session.phase==WordPresentationPhase::kBack?WordCardPhase::kRevealed:WordCardPhase::kFront;
}
int bookkeeping=0,finishes=0;
void ApplyWordReviewBookkeeping(WordAppState*,Action,uint64_t) { ++bookkeeping; }
void RequestCandidatePageIfNeeded(WordAppState*) {}
void ShowStudyCard(WordAppState* state) { state->card_phase=CardPhaseFromSession(state->session.persisted); }
void FinishOrLoadAdvancedReview(WordAppState* state) {
    ++finishes;
    if(!state->session.persisted.active) state->mode=WordAppMode::kReviewComplete;
    else ShowStudyCard(state);
}
@@REDUCERS@@
}
using namespace wqn;
int passes=0,failures=0;
void Check(bool okay,const char* label) { std::printf("%s: %s\n",okay?"PASS":"FAIL",label); okay?++passes:++failures; }
WordAppState Initial(int items=20) {
    WordAppState state; state.session.persisted.remote.session_id="session";
    for(int i=0;i<items;++i) state.session.persisted.remote.items.push_back({uint64_t(100+i)});
    bookkeeping=finishes=0; return state;
}
bool Accept(WordAppState* state,int64_t at,Action action=Action::kKnown) {
    auto& session=state->session;
    const uint64_t sequence=session.persisted.remote.next_sequence;
    session.pending_observation={};
    auto& pending=session.pending_observation;
    pending.session_id="session"; pending.sequence=sequence; pending.action=action;
    pending.next_position=100+session.persisted.position+(action==Action::kRevealed?0:1);
    pending.next_phase=action==Action::kRevealed?WordPresentationPhase::kBack:WordPresentationPhase::kFront;
    session.observation_effect_ready=true; session.commit_state=WordObservationCommitState::kPersisting;
    state->card_phase=WordCardPhase::kPersisting;
    return BufferWordObservationEffect(state,"request-"+std::to_string(sequence),"time",at);
}
int main() {
    auto state=Initial();
    std::vector<DurableWordObservation> batch; PersistedWordSession advanced;
    Check(Accept(&state,0) && state.session.persisted.remote.next_sequence==1 &&
        state.outbox.pending_count==0 && state.message=="已暂存，待保存" && bookkeeping==1,
        "RAM acceptance advances one card without claiming local durability or publishing an uploadable record");
    Check(!TakeWordObservationBatch(&state,1,29999,&batch,&advanced),"one event is retained until the absolute 30-second limit");
    Check(Accept(&state,29000) && !TakeWordObservationBatch(&state,1,29999,&batch,&advanced),
        "later input does not restart the oldest-event timer");
    Check(TakeWordObservationBatch(&state,1,30000,&batch,&advanced) && batch.size()==2 && advanced.remote.next_sequence==2,
        "oldest event triggers a flush even if the last event is recent");
    Check(Accept(&state,30001) && state.session.buffered_observations.size()==3 &&
        state.session.batch_in_flight==2,"a later event can be accepted while an immutable prefix is in flight");
    Check(!TakeWordObservationBatch(&state,2,60000,&batch,&advanced),"only one flush is in flight at a time");
    Check(ApplyWordObservationBatchResult(&state,ESP_OK,1,30002) && state.outbox.pending_count==2 &&
        state.session.persisted.remote.next_sequence==3 && state.session.buffered_observations.size()==1 &&
        state.session.buffered_observations.front().accepted_ms==30001,
        "durable prefix completion neither rolls back RAM-ahead progress nor refreshes the remaining oldest timestamp");
    Check(!ApplyWordObservationBatchResult(&state,ESP_OK,1,30003),"duplicate completed result cannot remove the newer tail");
    Check(!TakeWordObservationBatch(&state,2,60000,&batch,&advanced) &&
        TakeWordObservationBatch(&state,2,60001,&batch,&advanced),"remaining tail has its own absolute age limit");
    Check(!ApplyWordObservationBatchResult(&state,ESP_OK,1,60001) && state.session.batch_in_flight==1,
        "stale operation result cannot acknowledge a newer batch");
    Check(ApplyWordObservationBatchResult(&state,ESP_FAIL,2,60002) && state.outbox.pending_count==2 &&
        state.session.buffered_observations.size()==1 && state.session.persisted.remote.next_sequence==3,
        "failed flush retains the exact identities and RAM cursor without a false saved state");
    Check(!TakeWordObservationBatch(&state,3,61001,&batch,&advanced) &&
        TakeWordObservationBatch(&state,3,61002,&batch,&advanced) && batch[0].request_id=="request-2",
        "failed flush retries the same identity after bounded backoff");
    Check(ApplyWordObservationBatchResult(&state,ESP_OK,3,61003) && !HasBufferedWordObservations(state) &&
        state.outbox.pending_count==3,"successful retry clears only the proven durable tail");
    state=Initial(); for(int i=0;i<5;++i) Check(Accept(&state,i),"bounded five-event admission");
    Check(TakeWordObservationBatch(&state,7,4,&batch,&advanced) && batch.size()==5,
        "five events flush without waiting for 30 seconds");
    for(int i=5;i<10;++i) Check(Accept(&state,i),"in-flight plus waiting count is bounded");
    Check(!WordBatchPolicy::CanAccept(state.session.buffered_observations.size(),false) &&
        !Accept(&state,10) && state.session.buffered_observations.size()==10,
        "capacity ten includes the in-flight prefix, never ten waiting plus five in flight");
    state.session.observation_effect_ready=false;
    Check(ApplyWordObservationBatchResult(&state,ESP_OK,7,20) && state.session.buffered_observations.size()==5 &&
        state.session.persisted.remote.next_sequence==10,"first five are removed after success without reinstalling their old session");
    Check(TakeWordObservationBatch(&state,8,20,&batch,&advanced) && batch[0].sequence==5 && advanced.remote.next_sequence==10,
        "waiting five form the next consecutive batch with its own final cursor");
    state=Initial(); Accept(&state,100); state.session.batch_flush_requested=true;
    Check(TakeWordObservationBatch(&state,9,101,&batch,&advanced),"lifecycle boundary forces a one-event flush");
    state=Initial(); Accept(&state,0,Action::kRevealed); Accept(&state,1);
    Check(state.session.buffered_observations.size()==2 && state.session.buffered_observations[0].observation.action==Action::kRevealed &&
        state.session.buffered_observations[1].observation.action==Action::kKnown &&
        state.session.persisted.remote.next_sequence==2,"reveal and verdict remain separate ordered events, not last-judgment coalescing");
    state=Initial(1); Accept(&state,0);
    Check(state.session.batch_finish_pending && state.session.batch_flush_requested && finishes==0 &&
        state.mode==WordAppMode::kSessionStarting,"session completion side effects wait for durability, not RAM acceptance");
    Check(TakeWordObservationBatch(&state,10,1,&batch,&advanced) && ApplyWordObservationBatchResult(&state,ESP_OK,10,2) &&
        !state.session.batch_finish_pending && finishes==1,"durable final batch releases the completion boundary once");
    state=Initial(); for(int i=0;i<5;++i) Accept(&state,i); TakeWordObservationBatch(&state,11,4,&batch,&advanced);
    state.session.pending_observation={}; state.session.observation_effect_ready=true;
    state.session.commit_state=WordObservationCommitState::kPersisting;
    Check(ApplyWordObservationBatchResult(&state,ESP_OK,11,5) && state.session.observation_effect_ready &&
        state.session.commit_state==WordObservationCommitState::kPersisting,"older completion preserves a newer Prepared effect fence");
    std::printf("%d PASS / %d FAIL (production RAM reducers with card/lifecycle seams; NOT HIL)\n",passes,failures);
    return failures?1:0;
}
