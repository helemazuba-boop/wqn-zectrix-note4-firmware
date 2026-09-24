#include "opencode_client.h"

#if CONFIG_WQN_AGENT_ENABLE

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include "cJSON.h"
#include "config.h"
#include "device_protocol/json_depth_guard.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sse_chunk.h"

namespace wqn {
// Both are defined in the `wqn` namespace at the bottom of this file. The
// anonymous-namespace code below reaches them before their definitions are
// visible, so they are declared here at global scope: a nested
// `namespace wqn` inside the anonymous namespace would declare a different,
// never-defined function.
esp_err_t PostQuestionReply(const std::string& token,
                            const std::string& session_id,
                            const std::string& question_id,
                            const std::string& answer,
                            OpenCodeResult* result);
esp_err_t InterruptOpenCodeSession(const std::string& token,
                                   const std::string& session_id,
                                   OpenCodeResult* result);

// One string field of a cJSON object. It lives here rather than in the anonymous
// namespace below because the exported frame and history parsers need it too;
// the `using` inside that namespace keeps its unqualified call sites intact.
std::string JsonString(cJSON* object, const char* key)
{
    const char* value = cJSON_GetStringValue(
        object != nullptr ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr);
    return value != nullptr ? value : "";
}

// Fail a request with a machine-readable code and a short user-facing detail.
// Also here rather than in the anonymous namespace, for the same reason: the
// exported parsers report `invalid_response` with it.
void SetResultError(OpenCodeResult* result, int status, const char* code, const char* detail)
{
    if (result == nullptr) {
        return;
    }
    result->http_status = status;
    result->error_code = code != nullptr ? code : "upstream_error";
    result->detail = detail != nullptr ? detail : "OpenCode request failed";
}
// Device-side budgets. They are the same numbers the agent-gateway-v0 manifest
// records under `bounds`, and the exported history parser enforces the JSON one
// itself, so they live with the parsers rather than with the HTTP plumbing.
constexpr size_t kMaxJsonResponseBytes = 16 * 1024;
constexpr size_t kMaxPromptBytes = 4096;
// Backfill caps. The gateway already truncates to the device's JSON budget;
// these stop a future gateway change from turning one backfill into an
// unbounded heap allocation on a 4 KB-stack worker.
constexpr int kMaxHistoryMessages = 24;
constexpr int kMaxHistoryTools = 8;
}  // namespace wqn

namespace {

using wqn::JsonString;
using wqn::SetResultError;
using wqn::kMaxJsonResponseBytes;
using wqn::kMaxPromptBytes;
using wqn::kMaxHistoryMessages;
using wqn::kMaxHistoryTools;

constexpr char kTag[] = "wqn_opencode_api";

std::string AgentUrl(const char* path)
{
    std::string url = WQN_API_BASE;
    url += path;
    return url;
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

void DispatchAgentEvent(
    const std::string& event_name,
    const std::string& data,
    wqn::OpenCodeEventCallback callback,
    void* callback_ctx)
{
    if (callback == nullptr) {
        return;
    }
    wqn::OpenCodeEvent event;
    const esp_err_t parsed = wqn::ParseOpenCodeAgentFrame(event_name, data, &event);
    switch (parsed) {
        case ESP_OK:
            callback(event, callback_ctx);
            return;
        case ESP_ERR_NOT_SUPPORTED:
            // Whitelist stays whitelist: an event the gateway invented and this
            // firmware does not know is dropped, never surfaced as text. Log it
            // at debug level so on-device verification can spot "cloud maps it,
            // firmware does not" without a reflash-to-printf cycle.
            ESP_LOGD(kTag, "unhandled Agent SSE event: %s", event_name.c_str());
            return;
        default:
            ESP_LOGW(kTag, "dropped Agent SSE frame %s (%s)", event_name.c_str(),
                     esp_err_to_name(parsed));
            return;
    }
}

esp_err_t PostOutboundReply(
    const std::string& token,
    const std::string& session_id,
    const wqn::OpenCodeOutboundReply& reply)
{
    std::string body;
    std::string path;
    if (reply.is_question) {
        // The device sends the option's value; the gateway turns it into the
        // upstream form's `{[fieldKey]: value}` answer record, because only the
        // cloud knows which field the projected options came from.
        wqn::OpenCodeResult question_result;
        const esp_err_t question_error = wqn::PostQuestionReply(
            token, session_id, reply.question_id, reply.answer, &question_result);
        if (question_error != ESP_OK) {
            // Surface the allocator/transport state alongside the gateway
            // error: a failed reply here is exactly the internal-RAM
            // contention pattern the 2026-09 RCA documented, so the log has to
            // be diagnosable.
            ESP_LOGW(kTag,
                     "question reply rejected: %s (%s) err=%s internal_free=%u dma_largest=%u",
                     question_result.error_code.c_str(),
                     question_result.detail.c_str(),
                     esp_err_to_name(question_error),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
        }
        return question_error;
    }
    {
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(root, "permission_id", reply.permission_id.c_str());
        cJSON_AddStringToObject(root, "decision", reply.approve ? "once" : "reject");
        cJSON_AddBoolToObject(root, "confirmed", true);
        char* printed = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (printed == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        body = printed;
        cJSON_free(printed);
        path = "/agent/sessions/" + session_id + "/permission";
    }
    std::string response_body;
    wqn::OpenCodeResult reply_result;
    const esp_err_t request_result = OpenJsonRequest(
        token,
        AgentUrl(path.c_str()),
        HTTP_METHOD_POST,
        &body,
        &response_body,
        &reply_result);
    if (request_result != ESP_OK) {
        // Surface the allocator/transport state alongside the gateway error:
        // a failed reply here is exactly the internal-RAM contention pattern
        // the 2026-09 RCA documented, so the log must be diagnosable.
        ESP_LOGW(kTag,
                 "permission reply rejected: %s (%s) err=%s internal_free=%u dma_largest=%u",
                 reply_result.error_code.c_str(),
                 reply_result.detail.c_str(),
                 esp_err_to_name(request_result),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    }
    return request_result;
}

// Ask upstream to stop a run. Returns ESP_OK whenever the gateway accepted the
// request, including when it reports that nothing was running: `interrupted:
// false` is a successful "already finished", not a failure. Defined in the
// `wqn` namespace below because the read loop below calls it.

struct AgentStreamRequest {
    const std::string& token;
    const std::string& path;
    const std::string* request_body;
    const std::string& session_id;
    wqn::OpenCodeOutboundQueue* outbound_replies;
    wqn::OpenCodeReplyFailedCallback reply_failed;
    void* reply_failed_ctx;
    wqn::OpenCodeEventCallback callback;
    void* callback_ctx;
    wqn::OpenCodeResult* result;
    // Set by the session layer (from the UI thread) to ask the worker to stop
    // this stream. The worker is the only task allowed to make the interrupt
    // POST, so the UI thread never opens a connection of its own.
    std::atomic<bool>* interrupt_requested = nullptr;
    // Set by the worker once it has actually delivered the interrupt, so the
    // session layer can report "已中止" instead of a transport failure.
    std::atomic<bool>* interrupt_delivered = nullptr;
};

void DrainOutboundReplies(const AgentStreamRequest& request)
{
    if (request.outbound_replies == nullptr) {
        return;
    }
    wqn::OpenCodeOutboundReply reply;
    while (request.outbound_replies->Pop(&reply)) {
        // Reply POSTs run on this worker between stream reads. They open a
        // short-lived second connection, but never a second task or a second
        // long-lived TLS session, and the stream itself keeps its socket.
        const esp_err_t reply_error =
            PostOutboundReply(request.token, request.session_id, reply);
        if (reply_error != ESP_OK && request.reply_failed != nullptr) {
            request.reply_failed(reply, reply_error, request.reply_failed_ctx);
        }
    }
}

esp_err_t ReadAgentEventStream(const AgentStreamRequest& request)
{
    const std::string url = AgentUrl(request.path.c_str());
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method =
        request.request_body != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    config.timeout_ms = 5 * 60 * 1000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const size_t body_size =
        request.request_body != nullptr ? request.request_body->size() : 0;
    esp_err_t request_result = SetCommonHeaders(
        client,
        request.token,
        "text/event-stream",
        request.request_body != nullptr ? "application/json" : nullptr);
    if (request_result == ESP_OK) {
        request_result = esp_http_client_open(client, body_size);
    }
    if (request_result == ESP_OK && request.request_body != nullptr) {
        const int written = esp_http_client_write(
            client, request.request_body->data(), request.request_body->size());
        if (written < 0 ||
            static_cast<size_t>(written) != request.request_body->size()) {
            request_result = ESP_FAIL;
        }
    }
    if (request_result == ESP_OK) {
        const int64_t header_result = esp_http_client_fetch_headers(client);
        request_result = header_result < 0 ? ESP_FAIL : ESP_OK;
        request.result->http_status = esp_http_client_get_status_code(client);
        if (request_result == ESP_OK &&
            (request.result->http_status < 200 ||
             request.result->http_status >= 300)) {
            std::string error_body;
            request_result = ReadBoundedResponse(client, &error_body, kMaxJsonResponseBytes);
            ParseErrorBody(error_body, request.result);
            if (request_result == ESP_OK) {
                request_result = ESP_FAIL;
            }
        }
    }
    wqn::SseFrameBuffer parser;
    std::array<char, 768> buffer = {};
    bool idle_seen = false;
    while (request_result == ESP_OK && !idle_seen) {
        if (request.interrupt_requested != nullptr &&
            request.interrupt_requested->load(std::memory_order_acquire)) {
            // This loop is the only place that can end the stream, so it is
            // also the only place that can perform the interrupt. Without it a
            // cancel press would be invisible until the gateway itself closed
            // the run, i.e. up to the 30-minute outer timeout.
            const esp_err_t interrupt_error =
                wqn::InterruptOpenCodeSession(request.token, request.session_id, nullptr);
            if (interrupt_error == ESP_OK && request.interrupt_delivered != nullptr) {
                request.interrupt_delivered->store(true, std::memory_order_release);
            }
            break;
        }
        const int count = esp_http_client_read(client, buffer.data(), buffer.size());
        if (count < 0) {
            request_result = ESP_FAIL;
            break;
        }
        if (count == 0) {
            break;
        }
        if (!parser.feed(buffer.data(), static_cast<size_t>(count))) {
            // A frame past the device's JSON budget cannot be parsed even when
            // complete, so this is a broken stream rather than a dropped frame.
            SetResultError(request.result, request.result->http_status, "frame_overflow",
                           "Agent stream frame exceeded the device limit");
            request_result = ESP_FAIL;
            break;
        }
        std::string event_name;
        uint64_t event_id = 0;
        std::string event_data;
        while (parser.extract(&event_name, &event_id, &event_data) ==
               wqn::SseFrameBuffer::FrameState::kComplete) {
            (void)event_id;
            DispatchAgentEvent(event_name, event_data, request.callback, request.callback_ctx);
            if (event_name == "agent.status") {
                cJSON* status_root = wqn::protocol::JsonNestingWithinLimit(
                    event_data.data(), event_data.size())
                    ? cJSON_ParseWithLength(event_data.data(), event_data.size())
                    : nullptr;
                idle_seen = JsonString(status_root, "status") == "idle";
                cJSON_Delete(status_root);
            }
        }
        DrainOutboundReplies(request);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    const bool interrupted = request.interrupt_delivered != nullptr &&
        request.interrupt_delivered->load(std::memory_order_acquire);
    if (interrupted) {
        // Ending by request is a success, not an incomplete stream: the run is
        // over because the user stopped it.
        return ESP_OK;
    }
    if (request_result == ESP_OK && !idle_seen) {
        SetResultError(request.result, request.result->http_status, "stream_incomplete", "Agent stream ended before idle");
        return ESP_FAIL;
    }
    return request_result;
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
    OpenCodeOutboundQueue* outbound_replies,
    OpenCodeReplyFailedCallback reply_failed,
    void* reply_failed_ctx,
    std::atomic<bool>* interrupt_requested,
    std::atomic<bool>* interrupt_delivered,
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
    AgentStreamRequest request{
        token,
        path,
        &body,
        session_id,
        outbound_replies,
        reply_failed,
        reply_failed_ctx,
        callback,
        callback_ctx,
        result,
        interrupt_requested,
        interrupt_delivered};
    return ReadAgentEventStream(request);
}

esp_err_t WatchOpenCodeSession(
    const std::string& token,
    const std::string& session_id,
    OpenCodeOutboundQueue* outbound_replies,
    OpenCodeReplyFailedCallback reply_failed,
    void* reply_failed_ctx,
    std::atomic<bool>* interrupt_requested,
    std::atomic<bool>* interrupt_delivered,
    OpenCodeEventCallback callback,
    void* callback_ctx,
    OpenCodeResult* result)
{
    if (token.empty() || session_id.rfind("ses_", 0) != 0 || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    const std::string path = "/agent/sessions/" + session_id + "/events";
    AgentStreamRequest request{
        token,
        path,
        nullptr,
        session_id,
        outbound_replies,
        reply_failed,
        reply_failed_ctx,
        callback,
        callback_ctx,
        result,
        interrupt_requested,
        interrupt_delivered};
    return ReadAgentEventStream(request);
}

void OpenCodeOutboundQueue::Push(OpenCodeOutboundReply reply)
{
    std::lock_guard<std::mutex> lock(mutex_);
    items_.push_back(std::move(reply));
}

bool OpenCodeOutboundQueue::Pop(OpenCodeOutboundReply* out)
{
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) {
        return false;
    }
    *out = std::move(items_.front());
    items_.erase(items_.begin());
    return true;
}

esp_err_t CreateOpenCodeSession(
    const std::string& token,
    OpenCodeSessionInfo* session,
    OpenCodeResult* result)
{
    if (token.empty() || session == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    *session = OpenCodeSessionInfo{};
    const std::string body = "{}";
    std::string response;
    const esp_err_t request_result = OpenJsonRequest(
        token,
        AgentUrl("/agent/sessions"),
        HTTP_METHOD_POST,
        &body,
        &response,
        result);
    if (request_result != ESP_OK) {
        return request_result;
    }
    cJSON* root = protocol::JsonNestingWithinLimit(response.data(), response.size())
        ? cJSON_ParseWithLength(response.data(), response.size())
        : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    cJSON* row = data != nullptr ? cJSON_GetObjectItemCaseSensitive(data, "session") : nullptr;
    if (row != nullptr) {
        session->id = JsonString(row, "id");
        session->title = JsonString(row, "title");
        cJSON* updated = cJSON_GetObjectItemCaseSensitive(row, "updatedAt");
        if (cJSON_IsNumber(updated)) {
            session->updated_at = static_cast<int64_t>(updated->valuedouble);
        }
    }
    cJSON_Delete(root);
    if (session->id.rfind("ses_", 0) != 0) {
        SetResultError(result, result->http_status, "invalid_response", "Created session is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (session->title.empty()) {
        session->title = "新 Session";
    }
    return ESP_OK;
}

// Ask upstream to stop a run. Returns ESP_OK whenever the gateway accepted the
// request, including when it reports that nothing was running: `interrupted:
// false` is a successful "already finished", not a failure.
esp_err_t InterruptOpenCodeSession(
    const std::string& token,
    const std::string& session_id,
    OpenCodeResult* result)
{
    if (token.empty() || session_id.rfind("ses_", 0) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (result != nullptr) {
        *result = OpenCodeResult{};
    }
    static const std::string kBody = "{}";
    std::string response_body;
    const esp_err_t request_result = OpenJsonRequest(
        token,
        AgentUrl(("/agent/sessions/" + session_id + "/interrupt").c_str()),
        HTTP_METHOD_POST,
        &kBody,
        &response_body,
        result);
    if (request_result != ESP_OK) {
        return request_result;
    }
    cJSON* root = protocol::JsonNestingWithinLimit(response_body.data(), response_body.size())
        ? cJSON_ParseWithLength(response_body.data(), response_body.size())
        : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    // `interrupted` is a boolean, so it has to be read as one. Reading it as a
    // string yields "" for both values, which makes every interrupt look
    // delivered -- the log is the only consumer today, but it is the thing an
    // on-device verification reads to tell the two apart.
    cJSON* flag = data != nullptr ? cJSON_GetObjectItemCaseSensitive(data, "interrupted") : nullptr;
    const bool interrupted = cJSON_IsBool(flag) ? cJSON_IsTrue(flag) : false;
    cJSON_Delete(root);
    ESP_LOGI(kTag, "session interrupt %s: %s", session_id.c_str(),
             interrupted ? "delivered" : "no-op");
    return ESP_OK;
}

esp_err_t GetOpenCodeHistory(
    const std::string& token,
    const std::string& session_id,
    std::vector<OpenCodeHistoryMessage>* messages,
    OpenCodeResult* result)
{
    if (token.empty() || session_id.rfind("ses_", 0) != 0 ||
        messages == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    messages->clear();
    std::string body;
    const esp_err_t request_result = OpenJsonRequest(
        token,
        AgentUrl(("/agent/sessions/" + session_id + "/history").c_str()),
        HTTP_METHOD_GET,
        nullptr,
        &body,
        result);
    if (request_result != ESP_OK) {
        return request_result;
    }
    return ParseOpenCodeHistoryBody(body, messages, result);
}

esp_err_t PostQuestionReply(
    const std::string& token,
    const std::string& session_id,
    const std::string& question_id,
    const std::string& answer,
    OpenCodeResult* result)
{
    if (token.empty() || session_id.rfind("ses_", 0) != 0 ||
        question_id.empty() || answer.empty() || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "question_id", question_id.c_str());
    cJSON_AddStringToObject(root, "answer", answer.c_str());
    cJSON_AddBoolToObject(root, "confirmed", true);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string body = printed;
    cJSON_free(printed);
    std::string response_body;
    return OpenJsonRequest(
        token,
        AgentUrl(("/agent/sessions/" + session_id + "/question").c_str()),
        HTTP_METHOD_POST,
        &body,
        &response_body,
        result);
}

// Parse one SSE frame into an event. Return codes:
//   ESP_OK             -- the frame is inside the vocabulary and well formed
//   ESP_ERR_NOT_SUPPORTED -- an event name this firmware does not know. The
//                            vocabulary is a whitelist, so the frame is dropped
//                            rather than passed through; the caller logs it at
//                            debug level so on-device verification can spot a
//                            "the cloud maps it, the firmware does not" gap.
//   ESP_ERR_INVALID_RESPONSE -- a known event with a payload the device cannot
//                            act on (a question with no options, say).
esp_err_t ParseOpenCodeAgentFrame(
    const std::string& event_name,
    const std::string& data,
    wqn::OpenCodeEvent* out_event)
{
    if (out_event == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_event = wqn::OpenCodeEvent{};
    cJSON* root = wqn::protocol::JsonNestingWithinLimit(data.data(), data.size())
        ? cJSON_ParseWithLength(data.data(), data.size())
        : nullptr;
    if (root == nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    wqn::OpenCodeEvent event;
    if (event_name == "agent.accepted") {
        event.kind = wqn::OpenCodeEventKind::kAccepted;
    } else if (event_name == "agent.attached") {
        event.kind = wqn::OpenCodeEventKind::kAttached;
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
    } else if (event_name == "agent.reasoning.delta") {
        // Reasoning travels its own channel. It is never folded into
        // `agent.text`, so a gateway mapping bug cannot leak chain-of-thought
        // into the answer.
        event.kind = wqn::OpenCodeEventKind::kReasoningDelta;
        event.text = JsonString(root, "delta");
    } else if (event_name == "agent.reasoning") {
        event.kind = wqn::OpenCodeEventKind::kReasoning;
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
    } else if (event_name == "agent.question") {
        // The gateway projects the upstream form onto at most two options; a
        // form with more than two is delivered as `agent.status` instead, so an
        // empty `options` here is a malformed frame and is dropped below.
        event.kind = wqn::OpenCodeEventKind::kQuestion;
        event.question_id = JsonString(root, "question_id");
        event.text = JsonString(root, "title");
        cJSON* options = cJSON_GetObjectItemCaseSensitive(root, "options");
        if (cJSON_IsArray(options)) {
            const int option_count = std::min<int>(cJSON_GetArraySize(options), 2);
            event.question_options.reserve(static_cast<size_t>(option_count));
            for (int i = 0; i < option_count; ++i) {
                const cJSON* option = cJSON_GetArrayItem(options, i);
                if (!cJSON_IsObject(option)) {
                    continue;
                }
                wqn::OpenCodeQuestionOption projected;
                projected.value = JsonString(const_cast<cJSON*>(option), "value");
                projected.label = JsonString(const_cast<cJSON*>(option), "label");
                if (!projected.value.empty()) {
                    event.question_options.push_back(std::move(projected));
                }
            }
        }
        if (event.question_id.empty() || event.question_options.empty()) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else if (event_name == "agent.error") {
        event.kind = wqn::OpenCodeEventKind::kError;
        event.text = JsonString(root, "message");
    } else {
        cJSON_Delete(root);
        return ESP_ERR_NOT_SUPPORTED;
    }
    cJSON_Delete(root);
    *out_event = std::move(event);
    return ESP_OK;
}

// The body of `GET /agent/sessions/{id}/history`. The gateway has already
// trimmed it to the device's budget, so a body this large is a contract bug
// rather than a large conversation, and is refused rather than half-rendered:
// a truncated read would look identical to a short transcript. Bounded the same
// way here as in the HTTP reader, so the boot self-test can exercise the bound
// without a socket.
esp_err_t ParseOpenCodeHistoryBody(
    const std::string& body,
    std::vector<OpenCodeHistoryMessage>* messages,
    OpenCodeResult* result)
{
    if (messages == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Carried over from the request, if there was one: the self-test calls this
    // with a bare body, where it stays 0.
    const int http_status = result->http_status;
    *result = OpenCodeResult{};
    messages->clear();
    if (body.size() > kMaxJsonResponseBytes) {
        SetResultError(result, http_status, "invalid_size", "History response is too large");
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON* root = protocol::JsonNestingWithinLimit(body.data(), body.size())
        ? cJSON_ParseWithLength(body.data(), body.size())
        : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    cJSON* rows = data != nullptr ? cJSON_GetObjectItemCaseSensitive(data, "messages") : nullptr;
    if (!cJSON_IsArray(rows)) {
        cJSON_Delete(root);
        SetResultError(result, http_status, "invalid_response", "History is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int count = std::min<int>(cJSON_GetArraySize(rows), kMaxHistoryMessages);
    messages->reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* row = cJSON_GetArrayItem(rows, index);
        OpenCodeHistoryMessage message;
        message.role = JsonString(row, "role");
        message.text = JsonString(row, "text");
        message.thinking = JsonString(row, "thinking");
        cJSON* tools = cJSON_GetObjectItemCaseSensitive(row, "tools");
        if (cJSON_IsArray(tools)) {
            const int tool_count = std::min<int>(cJSON_GetArraySize(tools), kMaxHistoryTools);
            message.tools.reserve(static_cast<size_t>(tool_count));
            for (int t = 0; t < tool_count; ++t) {
                cJSON* tool_row = cJSON_GetArrayItem(tools, t);
                if (!cJSON_IsObject(tool_row)) {
                    continue;
                }
                OpenCodeHistoryTool tool;
                tool.name = JsonString(tool_row, "name");
                tool.status = JsonString(tool_row, "status");
                tool.preview = JsonString(tool_row, "preview");
                if (!tool.name.empty()) {
                    message.tools.push_back(std::move(tool));
                }
            }
        }
        // Only the two roles the gateway projects are backfilled; an empty
        // assistant row would render as a blank bubble.
        if ((message.role == "user" || message.role == "assistant") &&
            (!message.text.empty() || !message.thinking.empty() || !message.tools.empty())) {
            messages->push_back(std::move(message));
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

}  // namespace wqn

#endif  // CONFIG_WQN_AGENT_ENABLE
