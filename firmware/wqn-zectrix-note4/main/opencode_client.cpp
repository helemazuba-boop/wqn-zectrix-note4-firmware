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
// The manifest's `history_response_bytes`: the agreed ceiling for a backfill
// body, tighter than the generic JSON budget because the gateway trims the
// transcript to fit it. A body over this is a contract violation, not a large
// conversation.
constexpr size_t kMaxHistoryResponseBytes = 12 * 1024;
// Backfill caps. The gateway already truncates to the device's JSON budget;
// these stop a future gateway change from turning one backfill into an
// unbounded heap allocation on a 4 KB-stack worker.
constexpr int kMaxHistoryMessages = 24;
constexpr int kMaxHistoryTools = 8;
// The manifest's `sessions_listed`. Was the bare literal 12 in the row walk
// below, which is exactly the problem: a limit that decides how much of the
// picker the device can hold had no name to check the manifest against.
constexpr int kMaxSessionsListed = 12;
// The manifest's `text_event_bytes` (`agent.text`) and `delta_event_bytes`
// (`agent.text.delta`). MEASURED, NOT ENFORCED, and that asymmetry is
// deliberate -- see the warning at the frame parser.
//
// The stream already refuses a frame past the 16 KiB SSE budget
// (kMaxSseFrameBytes in sse_chunk.h), so these two are a tighter per-event
// contract the device currently only observes. Turning them into refusals would
// be a behaviour change against a gateway nobody has measured the largest frame
// of from here; the answer is a log line that says the peer is over, so the
// number stops being a manifest entry nobody reads.
constexpr size_t kMaxTextEventBytes = 8 * 1024;
constexpr size_t kMaxDeltaEventBytes = 2 * 1024;
// The manifest's `question_frame_bytes`. OBSERVED like the two text bounds, and
// that used to be a refusal -- see the warning at the frame parser for why that
// was wrong in both directions: the bound is reachable by a conformant CJK peer
// (the worst schema-legal questionData is 10,265 B with 3-byte characters and
// 13,561 B with 4-byte, against 10,240), and refusing the frame dropped the ask,
// which is the one outcome the guard was meant to prevent.
constexpr size_t kMaxQuestionFrameBytes = 10 * 1024;
// How many bytes `{"<field>":""}` costs around the field the manifest actually
// caps. The manifest's `text_event_bytes` / `delta_event_bytes` bound a STRING
// (`agent.text.data.text`, `agent.text.delta.data.delta`); the warning below
// measures `payload.size()`, which is the whole JSON envelope. Comparing them
// directly made the effective cap that many bytes tighter than the manifest, so
// a conformant peer sitting at the schema's own `maxLength` ceiling warned on
// every frame: `{"delta":<2048 chars>}` is 2,060 B against a 2,048 bound.
// `maxLength` also counts CODE POINTS while this counts BYTES, so multibyte
// text still overruns -- that part is a real property of the UTF-8 wire and is
// what the warning is for. This only removes the part that is pure arithmetic.
constexpr size_t kJsonFieldEnvelopeBytes = 16;
}  // namespace wqn

namespace {

using wqn::JsonString;
using wqn::SetResultError;
using wqn::kMaxJsonResponseBytes;
using wqn::kMaxPromptBytes;
using wqn::kMaxHistoryResponseBytes;
using wqn::kMaxHistoryMessages;
using wqn::kMaxHistoryTools;
using wqn::kMaxSessionsListed;
using wqn::kMaxTextEventBytes;
using wqn::kMaxDeltaEventBytes;
using wqn::kMaxQuestionFrameBytes;
using wqn::kJsonFieldEnvelopeBytes;

constexpr char kTag[] = "wqn_opencode_api";

// [interrupt-fix] One socket-read window inside the agent event stream.
// esp_http_client_read() only returns once the caller's buffer is full or the
// socket read times out, and the stream buffer is 768 bytes. A silent run
// carries only ~13-byte keepalive comments every ~14 s, so a single read could
// stay blocked for minutes -- and the interrupt flag was only re-checked
// between reads. A one-second window makes an idle stream return
// -ESP_ERR_HTTP_EAGAIN once a second so the loop can poll the flag; real data
// still arrives long before the window closes, so no bytes are lost.
constexpr int kAgentStreamIdleReadTimeoutMs = 1000;

std::string AgentUrl(const char* path)
{
    std::string url = WQN_API_BASE;
    url += path;
    return url;
}

// [detail] Every agent route that projects agent output carries the device's
// requested detail tier (0 简要 / 1 标准 / 2 详细, see kOpenCodeDetailDefault).
// Clamped here so a junk value from a future setting cannot reach the wire.
std::string DetailQuery(uint8_t detail)
{
    const uint8_t clamped = detail > 2 ? 2 : detail;
    return std::string("?detail=") + std::to_string(clamped);
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
    // The ask's own session wins. Both reply routes are session-scoped upstream,
    // and a subagent raises its asks against its own id -- so answering on the
    // attached session is a 404 against the ownership check, which is what made
    // subagent asks discoverable but never answerable. A relay that omits the
    // field, or sends one this device does not recognize, keeps answering on the
    // attached session exactly as before.
    const std::string& reply_session =
        wqn::OpenCodeReplySessionId(reply, session_id);
    std::string body;
    std::string path;
    if (reply.is_question) {
        // The device sends the option's value; the gateway turns it into the
        // upstream form's `{[fieldKey]: value}` answer record, because only the
        // cloud knows which field the projected options came from.
        wqn::OpenCodeResult question_result;
        const esp_err_t question_error = wqn::PostQuestionReply(
            token, reply_session, reply.question_id, reply.answer, &question_result);
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
        path = "/agent/sessions/" + reply_session + "/permission";
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
    // [agent] Set by the session layer to ask the worker to detach from this
    // stream *without* asking upstream to stop the run. Switching session is a
    // change of local view and the cloud keeps executing (§0), so this is the
    // whole difference from `interrupt_requested`: a switch performs no POST at
    // all, so nothing upstream ever learns the device left.
    std::atomic<bool>* switch_requested = nullptr;
    // Set by the worker once it has actually detached, so the session layer can
    // say "已切换" and chain its follow-up instead of reporting an incomplete
    // stream.
    std::atomic<bool>* switch_delivered = nullptr;
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
    if (request_result == ESP_OK) {
        // [interrupt-fix] Connect and header fetch above keep the long
        // timeout; only the streaming read gets the short idle window.
        esp_http_client_set_timeout_ms(client, kAgentStreamIdleReadTimeoutMs);
    }
    wqn::SseFrameBuffer parser;
    std::array<char, 768> buffer = {};
    // Either terminal status ends the read loop: `idle` is the contract's
    // terminator, and `error` is the same signal with a failure verdict (the
    // device records it as a failed run). Waiting for `idle` after an `error`
    // left the device reading a stream the gateway had already finished.
    bool terminal_seen = false;
    while (request_result == ESP_OK && !terminal_seen) {
        if (request.interrupt_requested != nullptr &&
            request.interrupt_requested->load(std::memory_order_acquire)) {
            // This loop is the only place that can end the stream, so it is
            // also the only place that can perform the interrupt. Without it a
            // cancel press would be invisible until the gateway itself closed
            // the run, i.e. up to the 30-minute outer timeout.
            //
            // Checked BEFORE the switch below, and that ordering is deliberate:
            // an explicit cancel must never be swallowed by a pending switch, or
            // the run the user asked to stop keeps executing. Both requests can
            // be armed at once -- the caller honours the switch from its tail
            // even when this branch is the one that ends the stream -- so
            // checking the destructive intent first loses nothing.
            const esp_err_t interrupt_error =
                wqn::InterruptOpenCodeSession(request.token, request.session_id, nullptr);
            if (interrupt_error == ESP_OK && request.interrupt_delivered != nullptr) {
                request.interrupt_delivered->store(true, std::memory_order_release);
            }
            break;
        }
        if (request.switch_requested != nullptr &&
            request.switch_requested->load(std::memory_order_acquire)) {
            // [agent] Detaching is deliberately NOT an interrupt: no POST, so
            // no upstream state changes and the run keeps going in the cloud.
            if (request.switch_delivered != nullptr) {
                request.switch_delivered->store(true, std::memory_order_release);
            }
            break;
        }
        const int count = esp_http_client_read(client, buffer.data(), buffer.size());
        if (count == -ESP_ERR_HTTP_EAGAIN) {
            // The idle window closed with no bytes: a quiet stream, not a
            // transport failure. Loop so the interrupt flag is re-checked;
            // the next window picks up whatever arrives.
            continue;
        }
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
                const std::string status = JsonString(status_root, "status");
                terminal_seen = status == "idle" || status == "error";
                cJSON_Delete(status_root);
                if (terminal_seen) {
                    // Frames after a terminator belong to no run this device is
                    // watching; stop extracting rather than dispatching them.
                    break;
                }
            }
        }
        if (parser.overflowed()) {
            // One frame's data passed kMaxSseFrameBytes and was dropped by the
            // parser. Its remainder was dropped with it, so any frame parsed
            // after this point may be spliced garbage -- fail the stream the
            // same way the line cap does, rather than rendering the splice.
            SetResultError(request.result, request.result->http_status, "frame_overflow",
                           "Agent stream frame exceeded the device limit");
            request_result = ESP_FAIL;
            break;
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
    if (request.switch_delivered != nullptr &&
        request.switch_delivered->load(std::memory_order_acquire)) {
        // [agent] Same for a switch, and the distinction matters more here: the
        // run is still executing upstream, so reporting `stream_incomplete`
        // would put an error on screen for a run the user merely navigated away
        // from. ESP_OK says only that this device's delivery ended on purpose.
        return ESP_OK;
    }
    if (request_result == ESP_OK && !terminal_seen) {
        SetResultError(request.result, request.result->http_status, "stream_incomplete", "Agent stream ended before idle");
        return ESP_FAIL;
    }
    return request_result;
}

}  // namespace

namespace wqn {

// Maps one device-contract `outcome` string onto the enum. Unrecognised and
// absent both land on kUnknown, which the callers must read as "no run in
// flight" -- see OpenCodeSessionOutcome. The gateway is the only place that can
// distinguish the reasons, and it already collapsed them.
//
// Every unrecognised value lands on kUnknown rather than an error, and that
// includes the invalid fixture below: a row the device cannot classify must be
// loadable, because dropping the whole list would take the readable rows with
// it. The direction is the same fail-safe the picker marker and the lease
// criterion use -- an unknown outcome costs a missing glyph, never a device
// that never sleeps.
wqn::OpenCodeSessionOutcome ParseSessionOutcome(std::string_view value)
{
    if (value == "running") {
        return wqn::OpenCodeSessionOutcome::kRunning;
    }
    if (value == "succeeded") {
        return wqn::OpenCodeSessionOutcome::kSucceeded;
    }
    if (value == "interrupted") {
        return wqn::OpenCodeSessionOutcome::kInterrupted;
    }
    if (value == "failed") {
        return wqn::OpenCodeSessionOutcome::kFailed;
    }
    return wqn::OpenCodeSessionOutcome::kUnknown;
}

// The body of `GET /agent/sessions`. Split out of ListOpenCodeSessions exactly
// the way ParseOpenCodeHistoryBody is split out of GetOpenCodeHistory, and for
// the same reason: the row walk decides which sessions the picker offers, how
// the 运行中 marker is drawn, and whether the device holds its sleep lease --
// and until it lived in a function that needs a token and a socket, ZERO of
// that was covered by any test. Every assertion in the boot self-test that
// touches `outcome` today was written against a hand-rolled copy of this loop,
// which tests the copy.
esp_err_t ParseOpenCodeSessionsBody(
    const std::string& body,
    std::vector<OpenCodeSessionInfo>* sessions,
    OpenCodeResult* result)
{
    if (sessions == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Carried over from the request, if there was one: the self-test calls this
    // with a bare body, where it stays 0.
    const int http_status = result->http_status;
    *result = OpenCodeResult{};
    sessions->clear();
    if (body.size() > kMaxJsonResponseBytes) {
        SetResultError(result, http_status, "invalid_size", "Session list is too large");
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON* root = protocol::JsonNestingWithinLimit(body.data(), body.size())
        ? cJSON_ParseWithLength(body.data(), body.size())
        : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    cJSON* rows = data != nullptr ? cJSON_GetObjectItemCaseSensitive(data, "sessions") : nullptr;
    if (!cJSON_IsArray(rows)) {
        cJSON_Delete(root);
        SetResultError(result, http_status, "invalid_response", "Session list is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    // The manifest's `sessions_listed`: the picker windows four rows and the
    // snapshot rides the UI copy verbatim, so an unbounded list is an unbounded
    // PSRAM cost and an unbounded frame signature. Rows past the cap are
    // ignored, not an error -- the gateway is the side that trims.
    const int available = cJSON_GetArraySize(rows);
    const int count = std::min(available, kMaxSessionsListed);
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
        session.outcome = ParseSessionOutcome(JsonString(row, "outcome"));
        // A row without a usable id is not a session this device can attach to,
        // and OfferSessionOptions would offer one anyway. Dropped silently: the
        // rest of the list is still worth showing.
        if (session.id.rfind("ses_", 0) == 0) {
            sessions->push_back(std::move(session));
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

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
    return ParseOpenCodeSessionsBody(body, sessions, result);
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
    uint8_t detail,
    const std::string& prompt,
    const std::string& request_id,
    OpenCodeOutboundQueue* outbound_replies,
    OpenCodeReplyFailedCallback reply_failed,
    void* reply_failed_ctx,
    std::atomic<bool>* interrupt_requested,
    std::atomic<bool>* interrupt_delivered,
    std::atomic<bool>* switch_requested,
    std::atomic<bool>* switch_delivered,
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
    // Omitted when empty so a caller with no key keeps the pre-P2 body byte
    // for byte. The cloud validates the shape (16 lowercase hex chars).
    if (!request_id.empty()) {
        cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string body = printed;
    cJSON_free(printed);
    const std::string path = "/agent/sessions/" + session_id + "/run" + DetailQuery(detail);
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
        interrupt_delivered,
        switch_requested,
        switch_delivered};
    return ReadAgentEventStream(request);
}

esp_err_t WatchOpenCodeSession(
    const std::string& token,
    const std::string& session_id,
    uint8_t detail,
    OpenCodeOutboundQueue* outbound_replies,
    OpenCodeReplyFailedCallback reply_failed,
    void* reply_failed_ctx,
    std::atomic<bool>* interrupt_requested,
    std::atomic<bool>* interrupt_delivered,
    std::atomic<bool>* switch_requested,
    std::atomic<bool>* switch_delivered,
    OpenCodeEventCallback callback,
    void* callback_ctx,
    OpenCodeResult* result)
{
    if (token.empty() || session_id.rfind("ses_", 0) != 0 || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = OpenCodeResult{};
    const std::string path = "/agent/sessions/" + session_id + "/events" + DetailQuery(detail);
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
        interrupt_delivered,
        switch_requested,
        switch_delivered};
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
        // A brand-new session has no run and no settle, so the gateway answers
        // `unknown`; parsing it rather than assuming it keeps one code path for
        // "what the row said" and lets the create path disagree later.
        session->outcome = ParseSessionOutcome(JsonString(row, "outcome"));
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
    uint8_t detail,
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
        AgentUrl(("/agent/sessions/" + session_id + "/history" + DetailQuery(detail)).c_str()),
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
//                            act on (a question with nine options, say).
esp_err_t ParseOpenCodeAgentFrame(
    const std::string& event_name,
    const std::string& data,
    wqn::OpenCodeEvent* out_event)
{
    if (out_event == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_event = wqn::OpenCodeEvent{};
    // [contract-ack] `data` is OPTIONAL in streamFrame -- the schema requires only
    // `event` -- and its own description says "Empty for the two
    // acknowledgements". So `agent.accepted` / `agent.attached` arriving with the
    // field absent entirely (no `data:` line in the SSE frame, which is what the
    // gateway's own shape allows) is a CONTRACT-VALID frame, and parsing an empty
    // string as JSON returned null here, so the device dropped it as
    // invalid_response: the acknowledgement was lost, and with it the state
    // transition that is the only thing the frame was for.
    //
    // Whitelisted to those two events on purpose. They are the two whose payload
    // the schema documents as empty, they are the two that read nothing out of
    // `data`, and every other event's data is REQUIRED (agent.status needs
    // `status`, agent.text needs `text`, ...). Accepting an empty payload for
    // those would silently swallow a frame the device needs but cannot act on --
    // a delta with no text is a dropped answer, not an empty one. The negatives
    // below pin that line.
    const bool acknowledgement =
        event_name == "agent.accepted" || event_name == "agent.attached";
    std::string payload = data;
    if (acknowledgement && payload.find_first_not_of(" \t\r\n") == std::string::npos) {
        payload = "{}";
    }
    // [bounds] The manifest's `text_event_bytes` / `delta_event_bytes`, OBSERVED
    // and not enforced. The two sides of that sentence are the whole point:
    //
    //   observed  -- a frame over the bound is logged once, at warning, naming
    //                the event, the size and the bound. The manifest entry was
    //                unread; now a peer that outgrows it says so.
    //   not enforced -- the frame is still parsed and still acted on.
    //
    // Enforcing would be the stricter-looking and wronger choice HERE, which is
    // the opposite of what this file used to do for `agent.question` and for a
    // reason worth stating:
    // nobody has measured the v2 gateway's largest text frame from this
    // checkout, and refusing one over 8 KiB would drop the tail of a real
    // answer to protect against a bound the peer may already exceed. A dropped
    // answer is unrecoverable; an over-long one is merely unplanned. The 16 KiB
    // SSE budget is what the device actually refuses, so the exposure stays
    // bounded either way.
    //
    // `agent.question` USED to be "the exception and is refused further down",
    // on the claim that its bound was unreachable by a conformant peer. It is
    // reachable -- see kMaxQuestionFrameBytes -- and the refusal dropped the
    // ask, so it is no longer an exception.
    //
    // The envelope slack: the manifest caps the FIELD, this measures the whole
    // JSON envelope, and comparing them directly made the effective cap that
    // many bytes tighter than the manifest intended -- a conformant peer
    // sitting at the schema's own `maxLength` warned on every frame. See
    // kJsonFieldEnvelopeBytes. What is left over after removing it is real:
    // `maxLength` counts code points and this counts bytes, so multibyte text
    // at the ceiling still warns, which is the point of the warning.
    const size_t text_envelope = kMaxTextEventBytes + kJsonFieldEnvelopeBytes;
    const size_t delta_envelope = kMaxDeltaEventBytes + kJsonFieldEnvelopeBytes;
    if (!acknowledgement && payload.size() > text_envelope &&
        (event_name == "agent.text" || event_name == "agent.reasoning")) {
        ESP_LOGW(kTag,
                 "agent frame %s is %u B, over the manifest's text_event_bytes=%u "
                 "plus its JSON envelope -- parsing anyway",
                 event_name.c_str(), static_cast<unsigned>(payload.size()),
                 static_cast<unsigned>(kMaxTextEventBytes));
    }
    if (!acknowledgement && payload.size() > delta_envelope &&
        (event_name == "agent.text.delta" || event_name == "agent.reasoning.delta")) {
        ESP_LOGW(kTag,
                 "agent frame %s is %u B, over the manifest's delta_event_bytes=%u "
                 "plus its JSON envelope -- parsing anyway",
                 event_name.c_str(), static_cast<unsigned>(payload.size()),
                 static_cast<unsigned>(kMaxDeltaEventBytes));
    }
    cJSON* root = wqn::protocol::JsonNestingWithinLimit(payload.data(), payload.size())
        ? cJSON_ParseWithLength(payload.data(), payload.size())
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
        // The upstream call id. Upstream only ever pairs it with the tool name
        // in the frame that announces the call, so it is what makes two calls of
        // the same tool two blocks rather than one.
        event.call_id = JsonString(root, "call_id");
    } else if (event_name == "agent.permission") {
        event.kind = wqn::OpenCodeEventKind::kPermission;
        event.session_id = JsonString(root, "session_id");
        event.permission_id = JsonString(root, "permission_id");
        event.tool = JsonString(root, "type");
        event.text = JsonString(root, "title");
        event.preview = JsonString(root, "preview");
        // An ask with no id can never be answered, and arming it would hold the
        // option bar for the rest of the run -- the reply route needs the id.
        // Drop the frame instead, the same way a malformed question is dropped.
        if (event.permission_id.empty()) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else if (event_name == "agent.question") {
        // [bounds] The manifest's `question_frame_bytes`, OBSERVED like the two
        // text bounds above -- and it used to be ENFORCED here, which was wrong
        // twice over.
        //
        // (1) The bound is reachable by a conformant peer, so the refusal fired
        // on legal traffic. `maxLength` counts CODE POINTS and the wire is UTF-8:
        // computing the worst schema-legal `questionData` (session_id 128 ASCII
        // by pattern, question_id 128, title 160, 8 x {value 256, label 120})
        // gives 3,673 B all-ASCII, 10,265 B with CJK and 13,561 B with astral
        // characters -- against a 10,240 B bound. CJK is the ordinary case for a
        // Chinese-language product, not the exotic one. The first version of
        // this check measured only the ASCII worst case and concluded the bound
        // was unreachable.
        //
        // (2) Refusing it dropped the ask, and a dropped ask is the one outcome
        // the original comment claimed to prevent. DispatchAgentEvent logs a
        // refused frame and returns; it does not end the stream. So the run
        // continued, the user never saw the question, and no reply was ever
        // POSTed -- the gateway sat waiting on an answer the device had thrown
        // away. Parsing it instead renders each label through
        // AgentOneLine(label, 88), which truncates to one line, so the user can
        // still answer or take the 自定义回答 pseudo-option that interrupts.
        // "Cannot hold" and "cannot dismiss" were being conflated: parsing is
        // what makes it dismissable.
        //
        // What is left of the bound is the diagnostic: a frame over it is logged
        // once, naming the event, the size and the bound.
        if (payload.size() > kMaxQuestionFrameBytes) {
            ESP_LOGW(kTag,
                     "agent frame %s is %u B, over the manifest's "
                     "question_frame_bytes=%u -- parsing anyway",
                     event_name.c_str(), static_cast<unsigned>(payload.size()),
                     static_cast<unsigned>(kMaxQuestionFrameBytes));
        }
        // One step of a question sequence: the gateway walks a multi-field
        // form one field at a time and reuses `question_id` as the step id
        // (`{formId}#{step}`, opaque to the device). Up to
        // kMaxOpenCodeQuestionOptions options; an empty list is the
        // abort-only ask for a field with nothing the device can choose
        // between, and it must still reach the UI so the user can escape.
        // Only a missing id makes the frame unanswerable.
        event.kind = wqn::OpenCodeEventKind::kQuestion;
        event.session_id = JsonString(root, "session_id");
        event.question_id = JsonString(root, "question_id");
        event.text = JsonString(root, "title");
        cJSON* options = cJSON_GetObjectItemCaseSensitive(root, "options");
        if (cJSON_IsArray(options)) {
            const int option_count = cJSON_GetArraySize(options);
            if (option_count > wqn::kMaxOpenCodeQuestionOptions) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_RESPONSE;
            }
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
        if (event.question_id.empty()) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else if (event_name == "agent.error") {
        event.kind = wqn::OpenCodeEventKind::kError;
        event.text = JsonString(root, "message");
        // An absent or non-boolean flag means fatal, so a gateway that predates
        // the field keeps its original meaning rather than silently downgrading
        // every error into a recoverable one.
        cJSON* fatal = cJSON_GetObjectItemCaseSensitive(root, "fatal");
        if (cJSON_IsBool(fatal)) {
            event.fatal = cJSON_IsTrue(fatal);
        }
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
    if (body.size() > kMaxHistoryResponseBytes) {
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
    const int row_count = cJSON_GetArraySize(rows);
    const int count = std::min<int>(row_count, kMaxHistoryMessages);
    // Keep the newest rows, not the oldest: the gateway trims the transcript
    // from the front, so the tail is the part the user was looking at. Taking
    // the first N showed the oldest N whenever a gateway sent more than the cap.
    const int start = row_count > count ? row_count - count : 0;
    messages->reserve(static_cast<size_t>(count));
    for (int index = start; index < start + count; ++index) {
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
