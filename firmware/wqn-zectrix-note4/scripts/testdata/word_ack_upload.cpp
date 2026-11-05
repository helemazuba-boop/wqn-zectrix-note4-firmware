#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "word_study_store.h"
#include "device_protocol/word_study.h"
const char* esp_err_to_name(int) { return "fixture"; }
constexpr const char* kTag="fixture";
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
enum class WordOutboxUploadState { kPending,kDrained,kFailed,kYielded,kAuthenticationRequired,kProtocolBlocked };
enum class OutboxFailureDisposition { kTransientServer,kAuthenticationRequired,kProtocolBlocked,kProtocolIntegrity,
    kSequenceResolved,kSessionTerminal,kTombstoneRecoverable };
enum class OutboxRetryCause { kLocalStorage,kServer };
namespace wqn {
using Metadata = protocol::v3::RequestMetadata;
struct LocalWordSessionDescriptor { PersistedWordSession session; uint32_t total_count=0; };
}
std::string g_local_registered_session_id;
constexpr size_t kMaxWordObservationBatchSize=64;
std::atomic<uint32_t> g_word_interaction_generation{0};
std::string g_word_outbox_gap_terminal_session_id;
uint8_t g_word_outbox_retry_attempts=0;
constexpr uint8_t kWordOutboxSequenceGapEscalation=5;
int64_t g_word_outbox_retry_not_before_ms=0, now_ms=0;
int64_t esp_timer_get_time() { return now_ms*1000; }
std::vector<wqn::DurableWordObservation> queue;
std::vector<size_t> ack_sizes;
std::vector<std::string> order;
int attempts=0, fail_http_at=0, fail_ack_at=0, yield_at=0, retry_calls=0, ack_calls=0;
int64_t http_delay_ms=0;
OutboxFailureDisposition failure=OutboxFailureDisposition::kTransientServer;
bool defer=false;
bool maintenance_due=false, maintenance_proof_deferred=false, fail_maintenance=false;
int maintenance_checks=0, maintenance_runs=0, yield_in_maintenance_at=0;
wqn::Metadata MakeControlMetadata() { return {}; }
void ResetWordOutboxRetryBackoff() { g_word_outbox_retry_attempts=0; }
bool WordOutboxRetryDeferred(const std::string&,int64_t) { return defer; }
void ScheduleWordOutboxRetry(const std::string&,int,OutboxRetryCause) { ++retry_calls; }
OutboxRetryCause RetryCauseFor(OutboxFailureDisposition) { return OutboxRetryCause::kServer; }
OutboxFailureDisposition ClassifyOutboxFailure(const wqn::protocol::v3::Error&,bool) { return failure; }
wqn::OutboxSuspendReason SuspendReasonFor(const std::string&) { return wqn::OutboxSuspendReason::kUnknownTerminal; }
bool enable_batch=false, local_descriptor=false;
int batch_calls=0, registration_calls=0, registration_status=200, batch_status=200;
int batch_prefix=-1; bool bad_receipt=false;
namespace wqn {
esp_err_t LoadLocalWordSessionDescriptor(const std::string& id,LocalWordSessionDescriptor* out) {
    if (!local_descriptor) return ESP_ERR_NOT_FOUND;
    out->session.remote.session_id=id; out->session.remote.seed="local-seed";
    out->session.remote.include_mastered=true;
    return ESP_OK;
}
esp_err_t RegisterLocalWordStudySessionV1(const std::string&,
 const protocol::word_study_v1::LocalSessionRegistrationRequest& req,
 protocol::word_study_v1::LocalSessionRegistrationData* out,protocol::v3::Error* error,int* status,bool* transport) {
    ++registration_calls;*transport=false;*status=registration_status;
    if(*status!=200){if(*status==401)error->code="UNAUTHORIZED";return ESP_FAIL;}
    out->registered=true;out->session_id=req.session_id;return ESP_OK;
}
esp_err_t SubmitWordStudyObservationBatchV1(const std::string&,
 const protocol::word_study_v1::ObservationBatchRequest& req,
 protocol::word_study_v1::ObservationBatchData* out,protocol::v3::Error* error,int* status,bool* transport) {
    ++batch_calls;*transport=false;*status=enable_batch?batch_status:404;
    if(*status!=200){if(*status==401)error->code="UNAUTHORIZED";return ESP_FAIL;}
    out->session_id=bad_receipt?"bad":req.session_id;
    const size_t n=batch_prefix<0?req.observations.size():std::min(req.observations.size(),size_t(batch_prefix));
    for(size_t i=0;i<n;++i){protocol::word_study_v1::BatchAcceptedItem x;
        x.request_id=req.observations[i].request_id;x.sequence=req.observations[i].sequence;
        x.observation_id="44444444-4444-4444-8444-444444444444";out->accepted.push_back(x);}
    if(n<req.observations.size()) {out->has_failure=true;out->failure.request_id=req.observations[n].request_id;
        out->failure.sequence=req.observations[n].sequence;out->failure.code="TRANSIENT";out->failure.retryable=true;}
    return ESP_OK;
}
}
namespace wqn {
esp_err_t PeekPendingWordObservationExcluding(const std::vector<std::string>& excluded,DurableWordObservation* out) {
    for(const auto& entry:queue) if(std::find(excluded.begin(),excluded.end(),entry.request_id)==excluded.end()) {
        *out=entry; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t AcknowledgeWordObservations(const std::vector<std::string>& ids) {
    ++ack_calls; ack_sizes.push_back(ids.size()); order.push_back("ack");
    if(ack_calls==fail_ack_at) return ESP_FAIL;
    queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const auto& entry) {
        return std::find(ids.begin(),ids.end(),entry.request_id)!=ids.end();
    }),queue.end());
    return ESP_OK;
}
esp_err_t AcknowledgeWordObservations(const std::vector<std::string>& ids,
    const WordOutboxMaintenanceGate& gate) {
    const esp_err_t result=AcknowledgeWordObservations(ids);
    if(result!=ESP_OK) return result;
    if(maintenance_due && !gate.should_defer(gate.context)) {
        ++maintenance_runs; maintenance_due=false;
    }
    return ESP_OK;
}
esp_err_t MaintainWordObservationOutbox(const WordOutboxMaintenanceGate& gate,bool* deferred) {
    ++maintenance_checks;
    if(maintenance_checks==yield_in_maintenance_at) g_word_interaction_generation.fetch_add(1);
    *deferred=false;
    if(fail_maintenance) return ESP_FAIL;
    if(maintenance_due && (maintenance_proof_deferred || gate.should_defer(gate.context))) {
        *deferred=true; return ESP_OK;
    }
    if(maintenance_due) { ++maintenance_runs; maintenance_due=false; }
    return ESP_OK;
}
esp_err_t SubmitWordStudyObservationV1(const std::string&,const protocol::word_study_v1::ObservationRequest&,
    protocol::word_study_v1::ObservationData*,protocol::v3::Error* error,bool* transport) {
    ++attempts; now_ms+=http_delay_ms;
    if(attempts==yield_at) g_word_interaction_generation.fetch_add(1);
    if(attempts==fail_http_at) { error->code="fixture"; *transport=false; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t SkipWordStudyObservationV1(const std::string&,const protocol::word_study_v1::ObservationRequest&,
    protocol::word_study_v1::ObservationData*,protocol::v3::Error*,bool*) { return ESP_OK; }
esp_err_t QuarantinePendingWordObservation(const std::string& id) {
    order.push_back("quarantine");
    queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const auto& entry){return entry.request_id==id;}),queue.end());
    return ESP_OK;
}
esp_err_t SuspendPendingWordObservation(const std::string& id,OutboxSuspendReason) {
    order.push_back("park");
    queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const auto& entry){return entry.request_id==id;}),queue.end());
    return ESP_OK;
}
}
@@UPLOAD@@
int passes=0,failures=0;
void Check(bool okay,const char* label) { std::printf("%s: %s\n",okay?"PASS":"FAIL",label); okay?++passes:++failures; }
void Reset(int count) {
    queue.clear(); ack_sizes.clear(); order.clear(); now_ms=0;
    enable_batch=local_descriptor=bad_receipt=false;batch_calls=registration_calls=0;
    registration_status=batch_status=200;batch_prefix=-1;g_local_registered_session_id.clear();
    attempts=fail_http_at=fail_ack_at=yield_at=retry_calls=ack_calls=0; defer=false; http_delay_ms=0;
    failure=OutboxFailureDisposition::kTransientServer; g_word_interaction_generation=0;
    maintenance_due=maintenance_proof_deferred=fail_maintenance=false;
    maintenance_checks=maintenance_runs=yield_in_maintenance_at=0;
    g_word_outbox_gap_terminal_session_id.clear(); g_word_outbox_retry_attempts=0;
    for(int i=0;i<count;++i) { wqn::DurableWordObservation entry; entry.request_id=std::to_string(i);
        entry.session_id="session"; entry.sequence=uint64_t(i); queue.push_back(entry); }
}
int main() {
    using S=WordOutboxUploadState; using D=OutboxFailureDisposition;
    Reset(5); Check(UploadPendingWordObservations("fixture")==S::kDrained && attempts==5 && ack_sizes==std::vector<size_t>{5},
        "five HTTP successes produce one ACK flush, not five single-record writes");
    Reset(3); Check(UploadPendingWordObservations("fixture")==S::kDrained && ack_sizes==std::vector<size_t>{3} && queue.empty(),
        "empty-queue return flushes the short final ACK batch");
    Reset(0); Check(UploadPendingWordObservations("fixture")==S::kDrained && ack_calls==0,"empty queue never writes an empty ACK batch");
    Reset(64); Check(UploadPendingWordObservations("fixture")==S::kDrained && attempts==64 && ack_calls==13 && ack_sizes.back()==4,
        "64-record round flushes each full group and its four-record tail");
    Reset(65); Check(UploadPendingWordObservations("fixture")==S::kPending && attempts==64 && queue.size()==1,
        "round limit flushes successes and preserves the unattempted record");
    Reset(5); yield_at=2; Check(UploadPendingWordObservations("fixture")==S::kYielded && attempts==2 &&
        ack_sizes==std::vector<size_t>{2} && queue.size()==3,"interaction yield flushes accepted predecessors");
    Reset(5); fail_http_at=3; Check(UploadPendingWordObservations("fixture")==S::kPending &&
        ack_sizes==std::vector<size_t>{2} && queue.size()==3,"transport/server failure flushes earlier successes without dropping the failed head");
    for(D kind:{D::kAuthenticationRequired,D::kProtocolBlocked}) {
        Reset(5); fail_http_at=3; failure=kind;
        Check(UploadPendingWordObservations("fixture")== (kind==D::kAuthenticationRequired?S::kAuthenticationRequired:S::kProtocolBlocked) &&
            ack_sizes==std::vector<size_t>{2} && queue.size()==3,"401/protocol terminal return flushes prior successes and preserves its disposition");
    }
    Reset(5); fail_http_at=3; failure=D::kAuthenticationRequired; fail_ack_at=1;
    Check(UploadPendingWordObservations("fixture")==S::kAuthenticationRequired && ack_calls==1 && queue.size()==5,
        "real 401 still reaches credential recovery if local ACK persistence fails");
    Reset(5); fail_ack_at=1; Check(UploadPendingWordObservations("fixture")==S::kPending && attempts==5 && queue.size()==5 && ack_calls==1,
        "failed ACK batch retains all original payloads without immediate busy-loop retry");
    for(D kind:{D::kProtocolIntegrity,D::kSequenceResolved,D::kSessionTerminal,D::kTombstoneRecoverable}) {
        Reset(4); fail_http_at=3; failure=kind;
        Check(UploadPendingWordObservations("fixture")==S::kDrained && order.size()==3 && order[0]=="ack" && order[2]=="ack" &&
            order[1]==(kind==D::kProtocolIntegrity?"park":"quarantine"),
            "all terminal mutation branches flush predecessor ACKs before park/quarantine");
    }
    Reset(4); defer=true; Check(UploadPendingWordObservations("fixture")==S::kPending && attempts==0 && ack_calls==0,
        "head backoff performs neither HTTP nor an empty write");
    Reset(5); http_delay_ms=16000; Check(UploadPendingWordObservations("fixture")==S::kDrained &&
        ack_sizes==std::vector<size_t>({3,2}),"oldest ACK timer is absolute, not refreshed by later responses");
    Reset(5); maintenance_due=true; yield_at=5;
    Check(UploadPendingWordObservations("fixture")==S::kYielded && ack_calls==1 && queue.empty() &&
        maintenance_due && maintenance_runs==0,
        "interaction during the fifth HTTP item cannot attach long maintenance to its durable ACK");
    Check(UploadPendingWordObservations("fixture")==S::kDrained && !maintenance_due && maintenance_runs==1 &&
        attempts==5 && ack_calls==1,
        "a quiet next round retries deferred maintenance even with no remaining upload or new ACK");
    Reset(0); maintenance_due=true; yield_in_maintenance_at=1;
    Check(UploadPendingWordObservations("fixture")==S::kYielded && maintenance_due && maintenance_runs==0,
        "interaction after an empty queue check still yields maintenance to the quiet timer");
    Reset(0); maintenance_due=true; maintenance_proof_deferred=true;
    Check(UploadPendingWordObservations("fixture")==S::kYielded && maintenance_due && attempts==0,
        "proof invalidation without input is retried rather than stranded behind an empty queue");
    Reset(3); fail_http_at=3; failure=D::kAuthenticationRequired; fail_maintenance=true;
    Check(UploadPendingWordObservations("fixture")==S::kAuthenticationRequired && maintenance_checks==0 && ack_calls==1,
        "optional quiet maintenance cannot mask a real 401 result");
    Reset(3); fail_http_at=3; failure=D::kProtocolBlocked; fail_maintenance=true;
    Check(UploadPendingWordObservations("fixture")==S::kProtocolBlocked && maintenance_checks==0 && ack_calls==1,
        "optional maintenance cannot replace a protocol-blocked terminal disposition");
    Reset(0); fail_maintenance=true;
    Check(UploadPendingWordObservations("fixture")==S::kFailed && maintenance_checks==1 && attempts==0,
        "empty-queue maintenance I/O failure reaches the existing bounded sync retry path");
    const auto ValidQueue=[](int count){Reset(count);for(auto& x:queue){
        x.session_id="33333333-3333-4333-8333-333333333333";
        x.request_id="observation_request_"+x.request_id;
        x.item_id="22222222-2222-4222-8222-222222222222";
        x.occurred_at="2026-11-05T10:00:00Z";}enable_batch=true;};
    ValidQueue(64); Check(UploadPendingWordObservations("fixture")==S::kDrained &&
        batch_calls==1 && attempts==0 && ack_sizes==std::vector<size_t>{64},
        "64 facts use one validated batch and one durable ACK");
    ValidQueue(4);batch_prefix=2;
    Check(UploadPendingWordObservations("fixture")==S::kPending && ack_sizes==std::vector<size_t>{2} &&
        queue.size()==2 && queue.front().sequence==2,"partial receipt ACKs only prefix and preserves failed item/tail");
    ValidQueue(4);bad_receipt=true;
    Check(UploadPendingWordObservations("fixture")==S::kPending && ack_calls==0 && queue.size()==4,
        "malformed receipt ACKs nothing");
    ValidQueue(4);fail_ack_at=1;
    Check(UploadPendingWordObservations("fixture")==S::kPending && queue.size()==4 && ack_calls==1,
        "batch ACK write failure retains identities for replay");
    ValidQueue(4);local_descriptor=true;registration_status=404;
    Check(UploadPendingWordObservations("fixture")==S::kPending && registration_calls==1 &&
        attempts==0 && batch_calls==0 && queue.size()==4,"registration404 never sends unregistered facts or deletes them");
    ValidQueue(4);local_descriptor=true;
    Check(UploadPendingWordObservations("fixture")==S::kDrained && registration_calls==1 && batch_calls==1,
        "local descriptor registered before batch");
    ValidQueue(4);batch_status=400;
    Check(UploadPendingWordObservations("fixture")==S::kDrained && batch_calls==1 && attempts==4,
        "whole-request error falls back to single without blaming valid head");
    ValidQueue(4);batch_status=429;
    Check(UploadPendingWordObservations("fixture")==S::kPending && ack_calls==0 && queue.size()==4,
        "batch rate limit retains every fact");
    ValidQueue(4);batch_status=401;
    Check(UploadPendingWordObservations("fixture")==S::kAuthenticationRequired && ack_calls==0,
        "batch401 reaches credential recovery");
    std::printf("%d PASS / %d FAIL (production upload loop with host seams; NOT network/flash HIL)\n",passes,failures);
    return failures?1:0;
}
