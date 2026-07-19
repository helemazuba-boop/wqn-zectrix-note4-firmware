#include "device_protocol/v3.h"

#include <algorithm>
#include <climits>

#include "cJSON.h"

namespace {

class JsonDocument {
public:
    explicit JsonDocument(cJSON* root) : root_(root) {}
    explicit JsonDocument(const std::string& body) : root_(cJSON_Parse(body.c_str())) {}
    ~JsonDocument() { cJSON_Delete(root_); }
    cJSON* root() const { return root_; }

private:
    cJSON* root_ = nullptr;
};

std::string StringField(cJSON* object, const char* key)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr
        ? value->valuestring
        : "";
}

uint64_t U64Field(cJSON* object, const char* key)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && value->valuedouble >= 0
        ? static_cast<uint64_t>(value->valuedouble)
        : 0;
}

esp_err_t Render(cJSON* root, std::string* body)
{
    if (root == nullptr || body == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    char* rendered = cJSON_PrintUnformatted(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *body = rendered;
    cJSON_free(rendered);
    return ESP_OK;
}

esp_err_t AddRequestMetadata(
    cJSON* root,
    const wqn::protocol::v3::RequestMetadata& metadata)
{
    if (root == nullptr || metadata.request_id.size() < 16 ||
        metadata.boot_id.size() < 16 || metadata.firmware_version.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON* capabilities = cJSON_CreateArray();
    if (capabilities == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "request_id", metadata.request_id.c_str());
    cJSON_AddStringToObject(root, "boot_id", metadata.boot_id.c_str());
    cJSON_AddStringToObject(root, "firmware_version", metadata.firmware_version.c_str());
    cJSON_AddItemToObject(root, "capabilities", capabilities);
    const char* values[] = {"display.epd", "sync.v3", "ai.sse.v2", "flash.v2"};
    for (const char* value : values) {
        cJSON_AddItemToArray(capabilities, cJSON_CreateString(value));
    }
    return ESP_OK;
}

esp_err_t BuildRequest(
    const wqn::protocol::v3::RequestMetadata& metadata,
    bool include_limit,
    std::string* body)
{
    if (body == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t metadata_result = AddRequestMetadata(document.root(), metadata);
    if (metadata_result != ESP_OK) {
        return metadata_result;
    }
    cJSON_AddNumberToObject(
        document.root(),
        "config_revision",
        static_cast<double>(metadata.config_revision));
    cJSON_AddNumberToObject(
        document.root(),
        "sync_cursor",
        static_cast<double>(metadata.sync_cursor));
    if (include_limit) {
        cJSON_AddNumberToObject(document.root(), "limit", metadata.limit);
    }
    return Render(document.root(), body);
}

esp_err_t ParseEnvelope(
    const std::string& body,
    const std::string& expected_request_id,
    JsonDocument* document,
    cJSON** data,
    wqn::protocol::v3::Error* error)
{
    (void)body;
    if (document == nullptr || data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = nullptr;
    *error = {};
    cJSON* root = document->root();
    if (!cJSON_IsObject(root) || StringField(root, "request_id") != expected_request_id) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsTrue(ok)) {
        cJSON* error_object = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON* retryable = cJSON_GetObjectItemCaseSensitive(error_object, "retryable");
        if (!cJSON_IsObject(error_object) || !cJSON_IsBool(retryable)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        error->code = StringField(error_object, "code");
        error->retryable = cJSON_IsTrue(retryable);
        error->retry_after_ms = static_cast<uint32_t>(
            std::min<uint64_t>(U64Field(error_object, "retry_after_ms"), UINT32_MAX));
        return ESP_FAIL;
    }
    *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(*data) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

}  // namespace

namespace wqn::protocol::v3 {

esp_err_t BuildBootstrapRequest(const RequestMetadata& metadata, std::string* body)
{
    return BuildRequest(metadata, false, body);
}

esp_err_t BuildSyncRequest(const RequestMetadata& metadata, std::string* body)
{
    return BuildRequest(metadata, true, body);
}

esp_err_t BuildClaimStartRequest(
    const RequestMetadata& metadata,
    const std::string& hardware_id,
    const std::string& device_public_key,
    std::string* body)
{
    if (body == nullptr || hardware_id.size() < 12 ||
        device_public_key.size() < 86 || device_public_key.size() > 88) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t metadata_result = AddRequestMetadata(document.root(), metadata);
    if (metadata_result != ESP_OK) {
        return metadata_result;
    }
    cJSON_AddStringToObject(document.root(), "hardware_id", hardware_id.c_str());
    cJSON_AddStringToObject(
        document.root(), "device_public_key", device_public_key.c_str());
    return Render(document.root(), body);
}

esp_err_t BuildClaimPollRequest(
    const RequestMetadata& metadata,
    const std::string& claim_id,
    std::string* body)
{
    if (body == nullptr || claim_id.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t metadata_result = AddRequestMetadata(document.root(), metadata);
    if (metadata_result != ESP_OK) {
        return metadata_result;
    }
    cJSON_AddStringToObject(document.root(), "claim_id", claim_id.c_str());
    return Render(document.root(), body);
}

esp_err_t ParseClaimStartResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ClaimStartData* data,
    Error* error)
{
    if (data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    const esp_err_t result =
        ParseEnvelope(body, expected_request_id, &document, &payload, error);
    if (result != ESP_OK) {
        return result;
    }
    data->claim_id = StringField(payload, "claim_id");
    data->display_code = StringField(payload, "display_code");
    data->expires_at_ms = U64Field(payload, "expires_at_ms");
    data->poll_interval_ms = static_cast<uint32_t>(
        std::min<uint64_t>(U64Field(payload, "poll_interval_ms"), UINT32_MAX));
    const bool valid_code = data->display_code.size() == 8 &&
        std::all_of(
            data->display_code.begin(),
            data->display_code.end(),
            [](char value) { return value >= '0' && value <= '9'; });
    if (data->claim_id.empty() || !valid_code || data->expires_at_ms == 0 ||
        data->poll_interval_ms < 1000 || data->poll_interval_ms > 30000) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ParseClaimPollResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ClaimPollData* data,
    Error* error)
{
    if (data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    const esp_err_t result =
        ParseEnvelope(body, expected_request_id, &document, &payload, error);
    if (result != ESP_OK) {
        return result;
    }
    const std::string status = StringField(payload, "status");
    if (status == "pending") {
        data->status = ClaimStatus::kPending;
        data->poll_interval_ms = static_cast<uint32_t>(
            std::min<uint64_t>(U64Field(payload, "poll_interval_ms"), UINT32_MAX));
        if (data->poll_interval_ms < 1000 || data->poll_interval_ms > 30000) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_OK;
    }
    if (status == "expired") {
        data->status = ClaimStatus::kExpired;
        return ESP_OK;
    }
    if (status != "approved") {
        return ESP_ERR_INVALID_RESPONSE;
    }

    data->status = ClaimStatus::kApproved;
    cJSON* sealed = cJSON_GetObjectItemCaseSensitive(payload, "sealed_credential");
    if (!cJSON_IsObject(sealed)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->sealed_credential.server_public_key =
        StringField(sealed, "server_public_key");
    data->sealed_credential.salt = StringField(sealed, "salt");
    data->sealed_credential.iv = StringField(sealed, "iv");
    data->sealed_credential.ciphertext = StringField(sealed, "ciphertext");
    if (data->sealed_credential.server_public_key.size() < 86 ||
        data->sealed_credential.server_public_key.size() > 88 ||
        data->sealed_credential.salt.empty() || data->sealed_credential.iv.empty() ||
        data->sealed_credential.ciphertext.empty()) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ParseBootstrapResponse(
    const std::string& body,
    const std::string& expected_request_id,
    BootstrapData* data,
    Error* error)
{
    if (data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    const esp_err_t result = ParseEnvelope(body, expected_request_id, &document, &payload, error);
    if (result != ESP_OK) {
        return result;
    }
    cJSON* media = cJSON_GetObjectItemCaseSensitive(payload, "media_protocols");
    data->device_id = StringField(payload, "device_id");
    data->config_revision = U64Field(payload, "config_revision");
    data->sync_cursor = U64Field(payload, "sync_cursor");
    if (data->device_id.empty() || !cJSON_IsObject(media) ||
        StringField(media, "ai_sse") != "v2-streaming" ||
        StringField(media, "flash") != "wqn-flash-v2") {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ParseSyncResponse(
    const std::string& body,
    const std::string& expected_request_id,
    SyncData* data,
    Error* error)
{
    if (data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    const esp_err_t result = ParseEnvelope(body, expected_request_id, &document, &payload, error);
    if (result != ESP_OK) {
        return result;
    }
    data->config_revision = U64Field(payload, "config_revision");
    data->sync_cursor = U64Field(payload, "sync_cursor");
    cJSON* configuration = cJSON_GetObjectItemCaseSensitive(payload, "configuration");
    cJSON* summaries = cJSON_GetObjectItemCaseSensitive(payload, "summaries");
    cJSON* due = cJSON_GetObjectItemCaseSensitive(summaries, "due_problem_ids");
    if (!cJSON_IsObject(configuration) || !cJSON_IsObject(summaries) || !cJSON_IsArray(due)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->auto_sync_interval_minutes = static_cast<uint32_t>(
        std::min<uint64_t>(U64Field(configuration, "auto_sync_interval_minutes"), UINT32_MAX));
    data->todo_count = static_cast<int>(
        std::min<uint64_t>(U64Field(summaries, "todo_count"), INT_MAX));
    data->word_due_count = static_cast<int>(
        std::min<uint64_t>(U64Field(summaries, "word_due_count"), INT_MAX));
    const int count = cJSON_GetArraySize(due);
    data->due_problem_ids.reserve(count);
    for (int index = 0; index < count; ++index) {
        cJSON* item = cJSON_GetArrayItem(due, index);
        if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->due_problem_ids.emplace_back(item->valuestring);
    }
    return ESP_OK;
}

}  // namespace wqn::protocol::v3
