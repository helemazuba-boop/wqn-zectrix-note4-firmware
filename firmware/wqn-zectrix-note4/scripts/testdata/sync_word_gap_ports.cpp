// Transport/outbox ports only. Classification, retry, checkpoint codec and
// startup restoration below come from production source. This is NOT HIL.
std::atomic<uint32_t> g_word_interaction_generation{0};
std::string g_word_outbox_gap_terminal_session_id;
std::string g_local_registered_session_id;
constexpr size_t kMaxWordObservationBatchSize = 64;
std::vector<wqn::DurableWordObservation> word_pending, word_quarantined, word_parked;
std::map<std::string, std::string> word_errors;
bool word_transport_failure = false, word_retryable = false;
bool word_yield_after_quarantine = false;
int word_calls = 0;
namespace wqn {
// These legacy retry/gap cases have no offline descriptor. Keep registration
// unavailable and force the documented batch-404 single-RPC fallback. The
// dedicated upload suite exercises real descriptor/receipt behavior.
esp_err_t LoadLocalWordSessionDescriptor(const std::string&, LocalWordSessionDescriptor*) {
    return ESP_ERR_NOT_FOUND;
}
esp_err_t RegisterLocalWordStudySessionV1(const std::string&,
    const protocol::word_study_v1::LocalSessionRegistrationRequest&,
    protocol::word_study_v1::LocalSessionRegistrationData*,
    protocol::v3::Error*, int*, bool*) {
    return ESP_ERR_NOT_FOUND;
}
esp_err_t SubmitWordStudyObservationBatchV1(const std::string&,
    const protocol::word_study_v1::ObservationBatchRequest&,
    protocol::word_study_v1::ObservationBatchData*,
    protocol::v3::Error*, int* status, bool* transport) {
    *status = 404;
    *transport = false;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t PeekPendingWordObservationExcluding(const std::vector<std::string>& excluded,
                                             DurableWordObservation* out) {
    for (const auto& entry : word_pending) {
        if (std::find(excluded.begin(), excluded.end(), entry.request_id) == excluded.end()) {
            *out = entry;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t AcknowledgeWordObservations(const std::vector<std::string>& ids,
                                     const WordOutboxMaintenanceGate&) {
    word_pending.erase(std::remove_if(word_pending.begin(), word_pending.end(),
        [&](const auto& entry) {
            return std::find(ids.begin(), ids.end(), entry.request_id) != ids.end();
        }), word_pending.end());
    return ESP_OK;
}
esp_err_t MaintainWordObservationOutbox(const WordOutboxMaintenanceGate&, bool* deferred) {
    *deferred = false;
    return ESP_OK;
}
esp_err_t SubmitWordStudyObservationV1(const std::string&,
    const protocol::word_study_v1::ObservationRequest& request,
    protocol::word_study_v1::ObservationData*, protocol::v3::Error* error, bool* transport) {
    ++word_calls;
    const auto it = word_errors.find(request.session_id);
    if (it == word_errors.end()) return ESP_OK;
    error->code = it->second;
    error->retryable = word_retryable;
    *transport = word_transport_failure;
    return ESP_FAIL;
}
esp_err_t SkipWordStudyObservationV1(const std::string&,
    const protocol::word_study_v1::ObservationRequest&,
    protocol::word_study_v1::ObservationData*, protocol::v3::Error*, bool*) {
    return ESP_OK;
}
esp_err_t MoveWordRecord(const std::string& id, std::vector<DurableWordObservation>& destination) {
    const auto it = std::find_if(word_pending.begin(), word_pending.end(),
        [&](const auto& entry) { return entry.request_id == id; });
    if (it == word_pending.end()) return ESP_ERR_NOT_FOUND;
    destination.push_back(*it);
    word_pending.erase(it);
    return ESP_OK;
}
esp_err_t QuarantinePendingWordObservation(const std::string& id) {
    const auto result = MoveWordRecord(id, word_quarantined);
    if (result == ESP_OK && word_yield_after_quarantine) {
        word_yield_after_quarantine = false;
        g_word_interaction_generation.fetch_add(1);
    }
    return result;
}
esp_err_t SuspendPendingWordObservation(const std::string& id, OutboxSuspendReason) {
    return MoveWordRecord(id, word_parked);
}
}
