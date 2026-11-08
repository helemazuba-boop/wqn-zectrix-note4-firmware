// Production clock, input admission prefix and observation pumps. The domain
// reducers, scheduler/worker, leases and file I/O are counting host ports.
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <utility>
#include "runtime/wall_clock.h"
#include "ui_screen.h"
#include "word_batch_policy.h"
using esp_err_t=int;
std::time_t fake_now=1893456000;
int64_t esp_timer_get_time(){return 40000000;}
bool jump_in_time=false,jump_in_format=false;
extern "C" std::tm* __real_gmtime_r(const std::time_t*,std::tm*);
extern "C" std::time_t __wrap_time(std::time_t* out){
    const auto value=fake_now;if(out)*out=value;
    if(jump_in_time){jump_in_time=false;wqn::runtime::NoteBuildTimeApplied();}
    return value;
}
extern "C" std::tm* __wrap_gmtime_r(const std::time_t* t,std::tm* out){
    if(jump_in_format){jump_in_format=false;wqn::runtime::NoteSntpTimeSynchronized(fake_now,fake_now);}
    return __real_gmtime_r(t,out);
}
int dispatches=0,reservations=0,metadata_calls=0,lease_calls=0,persist_calls=0,session_requests=0;
bool persist_busy=false;
namespace wqn {
@@UI_INPUT_ENUM@@
struct DurableWordObservation {std::string request_id,occurred_at;int64_t accepted_ms=0;};
struct DurableNoteObservation {std::string request_id,occurred_at;};
struct DurableProblemObservation {std::string request_id,occurred_at;};
struct PersistedWordSession {};
struct PersistedNoteSession {};
namespace protocol::note_study_v1 {
struct CreateSessionRequest {struct {std::string request_id;} metadata;struct {std::vector<std::string> notebook_ids;} scope;};
}
struct WordState {
    std::string message;
    struct Session {
        bool observation_effect_ready=false;
        std::vector<DurableWordObservation> buffered_observations;
        size_t batch_in_flight=0;int64_t batch_retry_after_ms=0;
        bool batch_flush_requested=true;
    } session;
};
struct NoteState {std::string message;struct {bool observation_effect_ready=false;} session;};
struct ProblemState {std::string message;bool verdict_effect_ready=false,active=false;};
struct UiState {UiScreen screen=UiScreen::kWord;WordState word_app;NoteState note_app;ProblemState problem_app;};
@@INPUT_PREFIX@@
namespace services {struct Metadata{std::string request_id;};Metadata MakeDeviceRequestMetadata(){++metadata_calls;return {"new-request"};}}
namespace runtime {
enum class SleepBlocker {kStorage};
struct SleepLease {
    bool held=false;explicit operator bool()const{return held;}
    void Reset(){held=false;}
    static SleepLease TryAcquire(SleepBlocker,const char*,const char*,int){++lease_calls;return {true};}
};
}
}
namespace device_ui_internal {
enum class RefreshSchedule {kNone,kSelection};
enum class AppEventKind {kStatusReload};
struct UiUpdate {RefreshSchedule refresh=RefreshSchedule::kNone;};
enum class PersistKind {kWordObservation,kNoteObservation,kProblemVerdict};
struct PersistTicket {unsigned operation_id=1;bool valid()const{return true;}};
enum class NoteCloudOp {kStartSession};
struct NoteCloudRequest {NoteCloudOp op=NoteCloudOp::kStartSession;char request_id[65]={},notebook_id[37]={};};
bool QueueNoteCloudRequest(const NoteCloudRequest&){++session_requests;return true;}
@@NOTE_START@@
bool IsPersistKindBusy(PersistKind){return persist_busy;}
PersistTicket TryReservePersist(PersistKind){++reservations;return {};}
void CancelPersistReservation(PersistTicket){}
std::vector<wqn::DurableWordObservation> enqueued_word;
std::string enqueued_note,enqueued_problem;
void EnqueueReservedWordObservations(PersistTicket,std::vector<wqn::DurableWordObservation> v,wqn::PersistedWordSession){++persist_calls;enqueued_word=std::move(v);}
void EnqueueReservedNoteObservation(PersistTicket,wqn::DurableNoteObservation o,wqn::PersistedNoteSession){++persist_calls;enqueued_note=o.occurred_at;}
void EnqueueReservedProblemVerdict(PersistTicket,wqn::DurableProblemObservation o){++persist_calls;enqueued_problem=o.occurred_at;}
wqn::runtime::SleepLease g_word_buffer_sleep_lease;
class UiRuntime {
public:
    wqn::UiState state_;
    wqn::UiState& state(){return state_;}
    UiUpdate FinishEvent(AppEventKind,RefreshSchedule r,bool){return {r};}
    UiUpdate DispatchLearningTimeRequired();
    UiUpdate DispatchWordObservationBuffered(const std::string& id,const std::string& time,int64_t now){
        state_.word_app.session.observation_effect_ready=false;
        state_.word_app.session.buffered_observations.push_back({id,time,now});return {};
    }
    bool TakeWordObservationBatch(unsigned,int64_t,std::vector<wqn::DurableWordObservation>* out,wqn::PersistedWordSession*){
        *out=std::move(state_.word_app.session.buffered_observations);state_.word_app.session.buffered_observations.clear();return true;
    }
    bool TakeNoteObservationEffect(const std::string& id,const std::string& t,unsigned,wqn::DurableNoteObservation* out,wqn::PersistedNoteSession*){
        state_.note_app.session.observation_effect_ready=false;*out={id,t};return true;
    }
    bool TakeProblemVerdictEffect(const std::string& id,const std::string& t,unsigned,wqn::DurableProblemObservation* out){
        state_.problem_app.verdict_effect_ready=false;*out={id,t};return true;
    }
    UiUpdate DispatchNoteObservationTakeFailed(){return {};}
    UiUpdate DispatchProblemVerdictTakeFailed(){return {};}
};
constexpr std::time_t kMinReasonableUnixTime=1704067200;
@@CLOCK@@
@@NOTICE@@
@@PUMPS@@
}
int passes=0,failures=0;
void Check(bool b,const char* n){std::printf("%s learning-time: %s\n",b?"PASS":"FAIL",n);b?++passes:++failures;}
void Calibrated(){wqn::runtime::NoteSntpTimeSynchronized(fake_now,fake_now);}
void ResetPorts(){dispatches=reservations=metadata_calls=lease_calls=persist_calls=0;persist_busy=false;device_ui_internal::g_word_buffer_sleep_lease.Reset();device_ui_internal::enqueued_word.clear();device_ui_internal::enqueued_note.clear();device_ui_internal::enqueued_problem.clear();}
int main(){
    using namespace wqn;using namespace device_ui_internal;
    Check(!runtime::IsLearningTimeReady(),"unknown clock cannot authorize learning");
    Check(!runtime::IsCalibratedLearningTime({runtime::WallClockSource::kRtcSntp,1,false},fake_now),"RTC calibration provenance also requires validated continuity");
    for(int source=0;source<4;++source){
        if(source==0)runtime::NoteBuildTimeApplied();
        if(source==1)runtime::NoteRtcTimeRestored(false,10,fake_now);
        if(source==2)Calibrated();
        if(source==3)runtime::NoteRtcTimeRestored(true,12,fake_now);
        const bool expected=source>=2;
        Check(runtime::IsLearningTimeReady()==expected,"only SNTP and calibrated continuity-checked RTC admit offline learning");
        Check(CurrentIsoTimestamp().empty()!=expected,"timestamp creation consumes provenance rather than a plausible year");
        Check((CurrentClockLabel()=="--:--")==!expected,"status clock labels approximate time as unavailable");
        Check((CurrentDateLabel()=="时间未校正")==!expected,"date label exposes missing calibration");
        protocol::note_study_v1::CreateSessionRequest start;start.metadata.request_id="request";start.scope.notebook_ids={"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"};session_requests=0;
        Check(QueueNoteSessionStart(start)==expected&&session_requests==int(expected),"actual note session queue requires calibration before creating a new session");
        for(auto screen:{UiScreen::kWord,UiScreen::kNote,UiScreen::kHome,UiScreen::kTime,UiScreen::kSettings,UiScreen::kAi,UiScreen::kTodo})
        for(auto input:{UiInput::kUp,UiInput::kDown,UiInput::kConfirm,UiInput::kLongConfirm,UiInput::kTopPrevious,UiInput::kTopNext})
        for(bool problem:{false,true}){
            UiState state;state.screen=screen;state.problem_app.active=problem;dispatches=0;HandleUiInput(&state,input);
            const bool blocked=!expected&&(screen==UiScreen::kWord||screen==UiScreen::kNote)&&
                (input==UiInput::kUp||input==UiInput::kDown||input==UiInput::kConfirm);
            Check(dispatches==int(!blocked),"actual input admission prefix holds learning and preserves navigation/back/settings");
            if(blocked){
                const std::string& notice=screen==UiScreen::kWord?state.word_app.message:(problem?state.problem_app.message:state.note_app.message);
                Check(notice=="请先联网校正时间","active learning domain receives the calibration notice");
                Check(screen==UiScreen::kWord?(state.note_app.message.empty()&&state.problem_app.message.empty()):state.word_app.message.empty(),"clock admission does not overwrite unrelated domain notices");
            }
        }
        ResetPorts();UiRuntime r;r.state_.word_app.session.observation_effect_ready=true;
        r.state_.note_app.session.observation_effect_ready=true;r.state_.problem_app.verdict_effect_ready=true;
        PumpWordObservationCommit(&r);PumpNoteObservationCommit(&r);PumpProblemVerdictCommit(&r);
        Check((metadata_calls>0)==expected&&(persist_calls>0)==expected,"actual three pumps create no new metadata or durable facts before calibration");
        if(!expected){
            Check(reservations==0&&lease_calls==0&&r.state_.word_app.session.observation_effect_ready&&r.state_.note_app.session.observation_effect_ready&&r.state_.problem_app.verdict_effect_ready,
                "uncalibrated pump retains all armed effects without reserving worker/lease slots");
            r.state_.word_app.session.buffered_observations.push_back({"already-accepted","2026-11-01T00:00:00Z",0});
            g_word_buffer_sleep_lease.held=true;PumpWordObservationCommit(&r);
            Check(enqueued_word.size()==1&&enqueued_word[0].request_id=="already-accepted"&&enqueued_word[0].occurred_at=="2026-11-01T00:00:00Z"&&r.state_.word_app.session.observation_effect_ready,
                "loss of calibration still flushes older accepted RAM facts unchanged while holding new ones");
        }else Check(!enqueued_word.empty()&&enqueued_word[0].occurred_at==CurrentIsoTimestamp()&&enqueued_note==CurrentIsoTimestamp()&&enqueued_problem==CurrentIsoTimestamp(),
                "calibrated pumps record current UTC time for each domain");
    }
    Calibrated();jump_in_time=true;Check(!runtime::IsLearningTimeReady(),"epoch change during input admission is rejected");
    Calibrated();jump_in_format=true;Check(CurrentIsoTimestamp().empty(),"epoch change while formatting cannot publish a mixed-basis timestamp");
    for(auto t:{std::time_t(0),std::time_t(1704067199),std::time_t(4102444800LL)}){fake_now=t;runtime::NoteRtcTimeRestored(true,20,t);Check(!runtime::IsLearningTimeReady()&&CurrentIsoTimestamp().empty(),"calibration cannot excuse an invalid wall-clock range");}
    fake_now=1893456000;Calibrated();ResetPorts();UiState ready_ui;ready_ui.word_app.message="请先联网校正时间";
    HandleUiInput(&ready_ui,UiInput::kUp);Check(ready_ui.word_app.message.empty(),"corrected time clears only the stale calibration notice on the next explicit input");
    UiRuntime resumed;resumed.state_.note_app.session.observation_effect_ready=true;
    persist_busy=true;PumpNoteObservationCommit(&resumed);Check(reservations==0&&resumed.state_.note_app.session.observation_effect_ready,"existing worker ownership is respected");
    persist_busy=false;PumpNoteObservationCommit(&resumed);Check(persist_calls==1&&!resumed.state_.note_app.session.observation_effect_ready,"corrected time permits an explicitly retried pending effect");
    std::printf("Learning time: %d checks, %d failures (production clock/input admission/pumps; host ports; NOT HIL)\n",passes+failures,failures);return failures?1:0;
}
