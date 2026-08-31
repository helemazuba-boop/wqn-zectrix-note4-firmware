#include "opencode_client.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "config.h"
#include "device_protocol/json_depth_guard.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sse_chunk.h"

namespace {

constexpr char kTag[] = "wqn_opencode_api";
constexpr size_t kMaxJsonResponseBytes = 16 * 1024;
constexpr size_t kMaxPromptBytes = 4096;

std::string AgentUrl(const char* path)
{
    std::string url = WQN_API_BASE;
    url += path;
    return url;
}

void SetResultError(wqn::OpenCodeResult* result, int status, const char* code, const char* detail)
{
    if (result == nullptr) {
        return;
    }
    result->http_status = status;
    result->error_code = code != nullptr ? code : "upstream_error";
    result->detail = detail != nullptr ? detail : "OpenCode request failed";
}

esp_err_t SetCommonHeaders(
    esp_http_client_handle_t client,
    const std::string& token,
    const char* accept,
    const char* content_type)
{
    esp_err_t result = esp_http_client_set_header(client, "Accept", accept);
    if (result == ESP_OK) {
        const std::string authorization = "Bearer " + token;
        result = esp_http_client_set_header(client, "Authorization", authorization.c_str());
    }
    if (result == ESP_OK && content_type != nullptr) {
        result = esp_http_client_set_header(client, "Content-Type", content_type);
    }
    return result;
}

esp_err_t ReadBoundedResponse(
    esp_http_client_handle_t client,
    std::string* body,
    size_t limit)
{
    if (body == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    body->clear();
    std::array<char, 512> buffer = {};
    for (;;) {
        const int count = esp_http_client_read(client, buffer.data(), buffer.size());
        if (count < 0) {
            return ESP_FAIL;
        }
        if (count == 0) {
            return ESP_OK;
        }
        if (body->size() + static_cast<size_t>(count) > limit) {
            return ESP_ERR_INVALID_SIZE;
        }
        body->append(buffer.data(), static_cast<size_t>(count));
    }
}

void ParseErrorBody(const std::string& body, wqn::OpenCodeResult* result)
{
    cJSON* root = wqn::protocol::JsonNestingWithinLimit(body.data(), body.size())
        ? cJSON_ParseWithLength(body.data(), body.size())
        : nullptr;
    cJSON* error = root != nullptr
        ? cJSON_GetObjectItemCaseSensitive(root, "error")
        : nullptr;
    const char* code = cJSON_GetStringValue(
        error != nullptr ? cJSON_GetObjectItemCaseSensitive(error, "code") : nullptr);
    const char* message = cJSON_GetStringValue(
        error != nullptr ? cJSON_GetObjectItemCaseSensitive(error, "message") : nullptr);
    if (result != nullptr) {
        result->error_code = code != nullptr ? code : "upstream_error";
        result->detail = message != nullptr ? message : "OpenCode request failed";
    }
    cJSON_Delete(root);
}

esp_err_t FinishJsonRequest(
    esp_http_client_handle_t client,
    std::string* body,
    wqn::OpenCodeResult* result)
{
    const int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        SetResultError(result, 0, "network_error", "Response headers are incomplete");
        return ESP_FAIL;
    }
    const int status = esp_http_client_get_status_code(client);
    if (result != nullptr) {
        result->http_status = status;
    }
    esp_err_t read_result = ReadBoundedResponse(client, body, kMaxJsonResponseBytes);
    if (read_result == ESP_OK && (status < 200 || status >= 300)) {
        ParseErrorBody(*body, result);
        read_result = ESP_FAIL;
    }
    return read_result;
}

esp_err_t OpenJsonRequest(
    const std::string& token,
    const std::string& url,
    esp_http_client_method_t method,
    const std::string* request_body,
    std::string* response_body,
    wqn::OpenCodeResult* result)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t request_result = SetCommonHeaders(
        client, token, "application/json",
        method == HTTP_METHOD_POST ? "application/json" : nullptr);
    const size_t body_size = request_body != nullptr ? request_body->size() : 0;
    if (request_result == ESP_OK) {
        request_result = esp_http_client_open(client, body_size);
    }
    if (request_result == ESP_OK && request_body != nullptr) {
        const int written = esp_http_client_write(
            client, request_body->data(), request_body->size());
        if (written < 0 || static_cast<size_t>(written) != request_body->size()) {
            request_result = ESP_FAIL;
        }
    }
    if (request_result == ESP_OK) {
        request_result = FinishJsonRequest(client, response_body, result);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return request_result;
}

std::string JsonString(cJSON* object, const char* key)
{
    const char* value = cJSON_GetStringValue(
        object != nullptr ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr);
    return value != nullptr ? value : "";
}

void DispatchAgentEvent(
    const std::string& event_name,
    const std::string& data,
    wqn::OpenCodeEventCallback callback,
    void* callback_ctx)
{
    if (callback == nullptr) {
        return;
    }
    cJSON* root = wqn::protocol::JsonNestingWithinLimit(data.data(), data.size())
        ? cJSON_ParseWithLength(data.data(), data.size())
        : nullptr;
    if (root == nullptr) {
        ESP_LOGW(kTag, "invalid Agent SSE JSON");
        return;
    }
    wqn::OpenCodeEvent event;
    if (event_name == "agent.accepted") {
        event.kind = wqn::OpenCodeEventKind::kAccepted;
    } else if (event_name == "agent.status") {
        event.kind = wqn::OpenCodeEventKind::kStatus;
        event.status = JsonString(root, "status");
        event.text = JsonString(root, "message");
    } else if (event_name == "agent.text.delta") {
        event.kind = wqn::OpenCodeEventKind::kTextDelta;
        event.text = JsonString(root, "delta");
    } else if (event_name == "agent.text") {
        event.kind = wqn::OpenCodeEventKind::kText;
        event.text = JsonString(root, "text");
    } else if (event_name == "agent.tool") {
        event.kind = wqn::OpenCodeEventKind::kTool;
        event.tool = JsonString(root, "tool");
        event.status = JsonString(root, "status");
        event.preview = JsonString(root, "preview");
    } else if (event_name == "agent.permission") {
        event.kind = wqn::OpenCodeEventKind::kPermission;
        event.permission_id = JsonString(root, "permission_id");
        event.tool = JsonString(root, "type");
        event.text = JsonString(root, "title");
        event.preview = JsonString(root, "preview");
    } else if (event_name == "agent.error") {
        event.kind = wqn::OpenCodeEventKind::kError;
        event.text = JsonString(root, "message");
    } else {
        cJSON_Delete(root);
        return;
    }
    cJSON_Delete(root);
    callback(event, callback_ctx);
}

}  // namespace

namespace wqn {

esp_err_t ListOpenCodeSessions(
    const std::string& token,
    std::vector<OpenCodeSessionInfo>* sessions,
    OpenCodeResult* result)
{
    if (token.empty() || sessions == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    sessions->clear();
    std::string body;
    const esp_err_t request_result = OpenJsonRequest(
        token, AgentUrl("/agent/sessions"), HTTP_METHOD_GET, nullptr, &body, result);
    if (request_result != ESP_OK) {
        return request_result;
    }
    cJSON* root = protocol::JsonNestingWithinLimit(body.data(), body.size())
        ? cJSON_ParseWithLength(body.data(), body.size())
        : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    cJSON* rows = data != nullptr ? cJSON_GetObjectItemCaseSensitive(data, "sessions") : nullptr;
    if (!cJSON_IsArray(rows)) {
        cJSON_Delete(root);
        SetResultError(result, result->http_status, "invalid_response", "Session list is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int count = std::min(cJSON_GetArraySize(rows), 12);
    sessions->reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* row = cJSON_GetArrayItem(rows, index);
        OpenCodeSessionInfo session;
        session.id = JsonString(row, "id");
        session.title = JsonString(row, "title");
        cJSON* updated = cJSON_GetObjectItemCaseSensitive(row, "updatedAt");
        if (cJSON_IsNumber(updated)) {
            session.updated_at = static_cast<int64_t>(updated->valuedouble);
        }
        if (session.id.rfind("ses_", 0) == 0) {
            sessions->push_back(std::move(session));
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t TranscribeOpenCodeAudio(
    const std::string& token,
    const AudioCaptureChunk& audio,
    std::string* transcript,
    OpenCodeResult* result)
{
    if (token.empty() || audio.empty() || transcript == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    transcript->clear();
    const std::string url = AgentUrl("/agent/transcribe");
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 120000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t request_result = SetCommonHeaders(
        client, token, "application/json", "application/octet-stream");
    if (request_result == ESP_OK) {
        request_result = esp_http_client_set_header(client, "X-WQN-Audio-Sample-Rate", "16000");
    }
    if (request_result == ESP_OK) {
        request_result = esp_http_client_set_header(client, "X-WQN-Audio-Sample-Format", "s16le");
    }
    if (request_result == ESP_OK) {
        request_result = esp_http_client_set_header(client, "X-WQN-Audio-Channels", "1");
    }
    char duration[16] = {};
    std::snprintf(duration, sizeof(duration), "%d", audio.duration_ms);
    if (request_result == ESP_OK) {
        request_result = esp_http_client_set_header(client, "X-WQN-Audio-Duration-Ms", duration);
    }
    const size_t byte_count = audio.sample_count * sizeof(int16_t);
    if (request_result == ESP_OK) {
        request_result = esp_http_client_open(client, byte_count);
    }
    size_t written = 0;
    while (request_result == ESP_OK && written < byte_count) {
        const size_t chunk = std::min<size_t>(2048, byte_count - written);
        const int count = esp_http_client_write(
            client,
            reinterpret_cast<const char*>(audio.samples) + written,
            chunk);
        if (count <= 0) {
            request_result = ESP_FAIL;
        } else {
            written += static_cast<size_t>(count);
        }
    }
    std::string body;
    if (request_result == ESP_OK) {
        request_result = FinishJsonRequest(client, &body, result);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (request_result != ESP_OK) {
        return request_result;
    }
    cJSON* root = protocol::JsonNestingWithinLimit(body.data(), body.size())
        ? cJSON_ParseWithLength(body.data(), body.size())
        : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    *transcript = JsonString(data, "transcript");
    cJSON_Delete(root);
    if (transcript->empty()) {
        SetResultError(result, result->http_status, "invalid_response", "Transcript is empty");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t RunOpenCodePrompt(
    const std::string& token,
    const std::string& session_id,
    const std::string& prompt,
    OpenCodeEventCallback callback,
    void* callback_ctx,
    OpenCodeResult* result)
{
    if (token.empty() || session_id.rfind("ses_", 0) != 0 || prompt.empty() ||
        prompt.size() > kMaxPromptBytes || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", prompt.c_str());
    cJSON_AddBoolToObject(root, "confirmed", true);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string body = printed;
    cJSON_free(printed);
    const std::string path = "/agent/sessions/" + session_id + "/run";
    const std::string url = AgentUrl(path.c_str());
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5 * 60 * 1000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t request_result = SetCommonHeaders(
        client, token, "text/event-stream", "application/json");
    if (request_result == ESP_OK) {
        request_result = esp_http_client_open(client, body.size());
    }
    if (request_result == ESP_OK) {
        const int written = esp_http_client_write(client, body.data(), body.size());
        if (written < 0 || static_cast<size_t>(written) != body.size()) {
            request_result = ESP_FAIL;
        }
    }
    if (request_result == ESP_OK) {
        const int64_t header_result = esp_http_client_fetch_headers(client);
        request_result = header_result < 0 ? ESP_FAIL : ESP_OK;
        result->http_status = esp_http_client_get_status_code(client);
        if (request_result == ESP_OK &&
            (result->http_status < 200 || result->http_status >= 300)) {
            std::string error_body;
            request_result = ReadBoundedResponse(client, &error_body, kMaxJsonResponseBytes);
            ParseErrorBody(error_body, result);
            if (request_result == ESP_OK) {
                request_result = ESP_FAIL;
            }
        }
    }
    SseFrameBuffer parser;
    std::array<char, 768> buffer = {};
    bool idle_seen = false;
    while (request_result == ESP_OK && !idle_seen) {
        const int count = esp_http_client_read(client, buffer.data(), buffer.size());
        if (count < 0) {
            request_result = ESP_FAIL;
            break;
        }
        if (count == 0) {
            break;
        }
        parser.feed(buffer.data(), static_cast<size_t>(count));
        std::string event_name;
        uint64_t event_id = 0;
        std::string event_data;
        while (parser.extract(&event_name, &event_id, &event_data) ==
               SseFrameBuffer::FrameState::kComplete) {
            (void)event_id;
            DispatchAgentEvent(event_name, event_data, callback, callback_ctx);
            if (event_name == "agent.status") {
                cJSON* status_root = protocol::JsonNestingWithinLimit(
                    event_data.data(), event_data.size())
                    ? cJSON_ParseWithLength(event_data.data(), event_data.size())
                    : nullptr;
                idle_seen = JsonString(status_root, "status") == "idle";
                cJSON_Delete(status_root);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (request_result == ESP_OK && !idle_seen) {
        SetResultError(result, result->http_status, "stream_incomplete", "Agent stream ended before idle");
        return ESP_FAIL;
    }
    return request_result;
}

}  // namespace wqn
