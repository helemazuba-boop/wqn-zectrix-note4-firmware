// Actual public storage wrappers and sync upload function. Scheduling, HTTP,
// metadata and disposition are host seams; no RTOS, auth or wire-contract proof.
#include <atomic>
esp_err_t ExecuteWithStorageLease(const char*,esp_err_t (*transaction)(void*),void* context,bool=false) {
    return transaction(context);
}
@@PUBLIC@@
enum class WordOutboxUploadState {kPending,kDrained,kFailed,kYielded,kAuthenticationRequired,kProtocolBlocked};
enum class OutboxFailureDisposition {kTransientServer,kAuthenticationRequired,kProtocolBlocked,kProtocolIntegrity,kSequenceResolved,kSessionTerminal,kTombstoneRecoverable};
enum class OutboxRetryCause {kLocalStorage,kServer};
std::atomic<uint32_t> g_word_interaction_generation{0};
int64_t g_problem_outbox_retry_not_before_ms=0;
int park_retry_scheduled=0;
bool park_http_reject=false;
OutboxFailureDisposition park_disposition=OutboxFailureDisposition::kTransientServer;
std::vector<std::string> park_submitted;
std::string park_http_marker;
int64_t esp_timer_get_time() {return 1000000;}
const char* esp_err_to_name(esp_err_t) {return "fixture error";}
wqn::protocol::v3::RequestMetadata MakeControlMetadata() {return {};}
void ResetProblemOutboxRetryBackoff() {g_problem_outbox_retry_not_before_ms=0;}
bool ProblemOutboxRetryDeferred(const std::string&,int64_t) {return false;}
void ScheduleProblemOutboxRetry(const std::string&,uint32_t,OutboxRetryCause) {++park_retry_scheduled;}
OutboxFailureDisposition ClassifyOutboxFailure(const wqn::protocol::v3::Error&,bool) {return park_disposition;}
OutboxRetryCause RetryCauseFor(OutboxFailureDisposition) {return OutboxRetryCause::kServer;}
wqn::OutboxSuspendReason SuspendReasonFor(const std::string&) {return wqn::OutboxSuspendReason::kActorOwnership;}
namespace wqn {
esp_err_t SubmitProblemReviewObservationV1(const std::string&,
    const protocol::problem_study_v1::ObservationRequest& request,
    protocol::problem_study_v1::ObservationData*,protocol::v3::Error* error,bool* transport_failure) {
    park_submitted.push_back(request.metadata.request_id);
    if(!park_http_marker.empty())ProofPut(kParkedPath,park_http_marker);
    *transport_failure=false;
    if(park_http_reject) {error->code="fixture-terminal";return ESP_FAIL;}
    return ESP_OK;
}
}
@@UPLOAD@@
void ResetParkUpload() {
    Reset();park_submitted.clear();park_http_marker.clear();park_http_reject=false;
    park_retry_scheduled=0;g_word_interaction_generation=0;ResetProblemOutboxRetryBackoff();
}
void ParkUploadCases() {
    const int old_pass=passes,old_fail=failures;
    const auto a=Value("park-upload-A"),b=Value("park-upload-B");
    ResetParkUpload();Commit(a);Commit(b);ProofPut(kParkedPath,"broken\n");
    auto payload=ProofBytes(kOutboxPath);
    Check(UploadPendingProblemObservations("synthetic")==WordOutboxUploadState::kFailed &&
        park_submitted.empty() && ProofBytes(kOutboxPath)==payload,"park upload: invalid markers block the actual uploader before HTTP");
    ResetParkUpload();Commit(a);Commit(b);Suspend(a);
    Check(UploadPendingProblemObservations("synthetic")==WordOutboxUploadState::kDrained &&
        park_submitted==std::vector<std::string>{b.request_id},"park upload: healthy suspended head permits exactly the next request");
    Check(ProofBytes(kOutboxPath)==EncodeObservationLine(a)+'\n' && ProofBytes(kParkedPath)==ParkLine(a),
        "park upload: successful successor ACK retains paused payload and marker bytes");
    for(unsigned path=0;path<3;++path) {
        ResetParkUpload();Commit(a);Commit(b);payload=ProofBytes(kOutboxPath);
        park_http_marker="broken\n";park_http_reject=path!=0;
        park_disposition=path==1?OutboxFailureDisposition::kProtocolIntegrity:OutboxFailureDisposition::kTombstoneRecoverable;
        const auto result=UploadPendingProblemObservations("synthetic");
        Check(result==(path==2?WordOutboxUploadState::kFailed:WordOutboxUploadState::kPending) &&
            park_submitted==std::vector<std::string>{a.request_id},"park upload: post-response ACK suspend and quarantine revalidate marker admission");
        Check(ProofBytes(kOutboxPath)==payload && ProofBytes(kParkedPath)=="broken\n" && !ProofExists(kRejectedPath),
            "park upload: metadata drift during HTTP preserves payload and prevents quarantine writes");
    }
    ResetParkUpload();Commit(a);Commit(b);park_fail_sync=true;
    Check(Suspend(a)!=ESP_OK,"park upload: failed park enters uncertain state");
    payload=ProofBytes(kOutboxPath);
    Check(UploadPendingProblemObservations("synthetic")==WordOutboxUploadState::kFailed &&
        park_submitted.empty() && ProofBytes(kOutboxPath)==payload,"park upload: uncertain park cannot report drained or submit a successor");
    park_fail_sync=false;const int before=park_sync_calls;
    Check(UploadPendingProblemObservations("synthetic")==WordOutboxUploadState::kDrained &&
        park_submitted==std::vector<std::string>{b.request_id} && park_sync_calls>before,
        "park upload: actual uploader resumes after a successful existing-file synchronization");
    Check(ProofBytes(kOutboxPath)==EncodeObservationLine(a)+'\n',"park upload: recovered flow retains the paused fact");
    std::printf("Problem park upload: %d checks, %d failures (production upload; host HTTP/scheduling seams; NOT HIL)\n",passes-old_pass+failures-old_fail,failures-old_fail);
    ResetParkUpload();
}
