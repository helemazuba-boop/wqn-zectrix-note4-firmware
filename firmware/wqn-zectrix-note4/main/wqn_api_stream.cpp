// wqn_api_stream.cpp - Implementation of the v2 SSE streaming AI client.
//
// Design notes:
//   * The request body is raw 16 kHz mono s16le PCM. Metadata travels in the
//     X-WQN-* headers shared with the cloud route's validated contract.
//   * The event handler drives an SSE frame parser that calls back into the
//     caller's WqnAiSseCallback on every complete frame.
//   * One monotonic budget covers upload, headers and streaming. A disconnect
//     before final/error fails the request; it is never automatically replayed.
//
#include "wqn_api.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "sse_chunk.h"
#include "storage.h"
#include "wqn_api_stream_internal.h"
#include "voice_diagnostics.h"

namespace {

constexpr char kTag[] = "wqn_sse";

struct StreamContext {
  wqn::SseFrameBuffer parser;
  wqn::WqnAiStreamRequest request;
  wqn::WqnAiChatResponse* response = nullptr;
  std::string mac;
  std::string user_id_header;
  bool fatal = false;
  int http_status = 0;
  std::string error_code;
  std::string error_message;
  uint64_t last_event_id = 0;
  bool terminal_event_seen = false;
  int64_t deadline_us = 0;
  bool timed_out = false;
  bool close_requested = false;
  // Owner-local observations only; no extra timers, tasks or per-delta logs.
  int64_t started_us = 0;
  int64_t first_data_us = -1;
  int64_t last_data_us = -1;
  int64_t first_event_us = -1;
  int64_t last_event_us = -1;
  int64_t last_progress_us = -1;
  uint64_t received_bytes = 0;
  uint32_t data_callbacks = 0;
  uint32_t decoded_events = 0;
  uint8_t stage_logs = 0;
  int last_read_result = 0;
  int last_read_errno = 0;
  const char* phase = "init";
  char last_server_stage[48] = "none";
};

void LogHttpTrace(const StreamContext* ctx, const char* stage, esp_err_t result)
{
  const int64_t now_us = esp_timer_get_time();
  ESP_LOGI(kTag,
           "event=ai.http stage=%s req=%s phase=%s server_stage=%s err=0x%x http_status=%d terminal=%d error=%s elapsed_ms=%lld first_data_ms=%lld first_event_ms=%lld data_gap_ms=%lld event_gap_ms=%lld progress_gap_ms=%lld body_bytes=%llu callbacks=%lu events=%lu read_result=%d read_errno=%d stage_logs=%u",
           stage, wqn::voice_diagnostics::SafeKey(ctx->request.request_id.c_str()),
           ctx->phase, ctx->last_server_stage, static_cast<unsigned>(result),
           ctx->http_status, ctx->terminal_event_seen ? 1 : 0,
           wqn::voice_diagnostics::SafeKey(ctx->error_code.c_str()),
           static_cast<long long>((now_us - ctx->started_us) / 1000),
           static_cast<long long>(ctx->first_data_us < 0 ? -1 : (ctx->first_data_us - ctx->started_us) / 1000),
           static_cast<long long>(ctx->first_event_us < 0 ? -1 : (ctx->first_event_us - ctx->started_us) / 1000),
           static_cast<long long>(ctx->last_data_us < 0 ? -1 : (now_us - ctx->last_data_us) / 1000),
           static_cast<long long>(ctx->last_event_us < 0 ? -1 : (now_us - ctx->last_event_us) / 1000),
           static_cast<long long>(ctx->last_progress_us < 0 ? -1 : (now_us - ctx->last_progress_us) / 1000),
           static_cast<unsigned long long>(ctx->received_bytes),
           static_cast<unsigned long>(ctx->data_callbacks),
           static_cast<unsigned long>(ctx->decoded_events),
           ctx->last_read_result, ctx->last_read_errno, static_cast<unsigned>(ctx->stage_logs));
}

std::string BuildMacAddress()
{
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    return {};
  }
  char buf[18];
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

void DispatchEvent(StreamContext* ctx, const wqn::SseFrameBuffer& /*frame*/,
                   const std::string& event_name, uint64_t event_id,
                   const std::string& data_json)
{
  ctx->last_event_id = event_id;
  if (ctx->request.callback == nullptr) {
    return;
  }
  wqn::WqnAiSseEvent ev;
  cJSON* root = nullptr;
  if (!wqn::DecodeSseEvent(event_name, event_id, data_json, &ev, &root)) {
    ESP_LOGW(kTag, "SSE JSON parse failed: bytes=%u req=%s",
             static_cast<unsigned>(data_json.size()),
             wqn::voice_diagnostics::SafeKey(ctx->request.request_id.c_str()));
    return;
  }

  const auto k = ev.kind;
  using Kind = wqn::WqnAiSseEvent::Kind;
  ctx->last_event_us = esp_timer_get_time();
  ++ctx->decoded_events;
  // A heartbeat/unknown event or repeated stage is traffic, not business
  // progress. This observation never renews or changes the request budget.
  bool progress = k != Kind::kUnknown && k != Kind::kReady &&
                  k != Kind::kState && k != Kind::kStage;
  if (k == Kind::kAsrDelta || k == Kind::kThinkingDelta || k == Kind::kTextDelta) {
    progress = !ev.delta.empty() || !ev.text.empty() || !ev.full_text.empty();
  }
  if (k == Kind::kStage) {
    const char* key = wqn::voice_diagnostics::SafeKey(ev.stage.c_str(), sizeof(ctx->last_server_stage) - 1);
    progress = !ev.stage.empty() && std::strcmp(key, "redacted") != 0 &&
               std::strcmp(ctx->last_server_stage, key) != 0;
  }
  if (progress) ctx->last_progress_us = ctx->last_event_us;
  if (ctx->first_event_us < 0) {
    ctx->first_event_us = ctx->last_event_us;
    LogHttpTrace(ctx, "first_event", ESP_OK);
  }
  if (k == wqn::WqnAiSseEvent::Kind::kStage) {
    const char* key = wqn::voice_diagnostics::SafeKey(ev.stage.c_str(), sizeof(ctx->last_server_stage) - 1);
    if (std::strcmp(ctx->last_server_stage, key) != 0) {
      std::strncpy(ctx->last_server_stage, key, sizeof(ctx->last_server_stage) - 1);
      if (ctx->stage_logs < 32) {
        ++ctx->stage_logs;
        LogHttpTrace(ctx, "server_stage", ESP_OK);
      }
    }
  }

  // Snapshot conversation_id into the response (used by the final callback).
  if (!ev.conversation_id.empty() && ctx->response != nullptr) {
    ctx->response->conversation_id = ev.conversation_id;
  }

  // Drive ctx-level error state if this is an `error` frame.
  if (k == wqn::WqnAiSseEvent::Kind::kError) {
    ctx->terminal_event_seen = true;
    ctx->error_code = ev.error_code.empty() ? "model_failed" : ev.error_code;
    ctx->error_message = ev.error_message;
  } else if (k == wqn::WqnAiSseEvent::Kind::kFinal && ctx->response != nullptr) {
    ctx->terminal_event_seen = true;
    ctx->response->latency_ms = ev.latency_ms;
    ctx->response->conversation_id = ev.conversation_id;
    // Pull actions / function_calls into the response so legacy callers keep working.
    cJSON* arr;
    if ((arr = cJSON_GetObjectItemCaseSensitive(root, "actions")) != nullptr && cJSON_IsArray(arr)) {
      const int n_arr = cJSON_GetArraySize(arr);
      ctx->response->actions.reserve(ctx->response->actions.size() + n_arr);
      for (int i = 0; i < n_arr; ++i) {
        cJSON* a = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(a)) continue;
        wqn::WqnAiAction action{};
        const char* t = nullptr;
        if ((t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "type"))) != nullptr) {
          action.type = t;
        }
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "todo_id"))) action.todo_id = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "word_id"))) action.word_id = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "deck_id"))) action.deck_id = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "word"))) action.word = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "problem_id"))) action.problem_id = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "title"))) action.title = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "status"))) action.status = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "due_at"))) action.due_at = v;
        ctx->response->actions.push_back(std::move(action));
      }
    }
    if ((arr = cJSON_GetObjectItemCaseSensitive(root, "function_calls")) != nullptr && cJSON_IsArray(arr)) {
      const int n_arr = cJSON_GetArraySize(arr);
      ctx->response->function_calls.reserve(ctx->response->function_calls.size() + n_arr);
      for (int i = 0; i < n_arr; ++i) {
        cJSON* a = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(a)) continue;
        wqn::WqnAiFunctionCallSummary fc{};
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "name"))) fc.name = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "status"))) fc.status = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "display"))) fc.display = v;
        if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "title"))) fc.title = v;
        ctx->response->function_calls.push_back(std::move(fc));
      }
    }
  } else if (k == wqn::WqnAiSseEvent::Kind::kAsrComplete && ctx->response != nullptr) {
    cJSON* asr = cJSON_GetObjectItemCaseSensitive(root, "asr");
    if (cJSON_IsObject(asr)) {
      if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(asr, "provider"))) {
        ctx->response->asr.provider = v;
      }
      if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(asr, "model"))) {
        ctx->response->asr.model = v;
      }
      if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(asr, "text"))) {
        ctx->response->asr.text = v;
      }
      if (const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(asr, "request_id"))) {
        ctx->response->asr.request_id = v;
      }
    }
    if (ev.elapsed_ms > 0) {
      ctx->response->asr.elapsed_ms = ev.elapsed_ms;
    }
    ctx->response->transcript = ev.text;
  }

  if (k == wqn::WqnAiSseEvent::Kind::kAsrComplete ||
      k == wqn::WqnAiSseEvent::Kind::kFinal ||
      k == wqn::WqnAiSseEvent::Kind::kError) {
    LogHttpTrace(ctx, k == wqn::WqnAiSseEvent::Kind::kAsrComplete ? "asr_complete" : "terminal", ESP_OK);
  }
  cJSON_Delete(root);
  ctx->request.callback(ev, ctx->request.user_ctx);
  if (ctx->terminal_event_seen) LogHttpTrace(ctx, "terminal_callback_return", ESP_OK);
}

bool CheckDeadline(StreamContext* ctx)
{
  if (ctx->terminal_event_seen || ctx->fatal) {
    return !ctx->timed_out;
  }
  if (ctx->deadline_us - esp_timer_get_time() < 1000) {
    ctx->fatal = true;
    ctx->timed_out = true;
    ctx->error_code = "chat_timeout";
    ctx->error_message = "stream request deadline exceeded";
    LogHttpTrace(ctx, "deadline", ESP_ERR_TIMEOUT);
    return false;
  }
  return true;
}

bool ApplyRemainingTimeout(StreamContext* ctx, esp_http_client_handle_t client)
{
  if (!CheckDeadline(ctx)) {
    return false;
  }
  const int64_t remaining_ms = (ctx->deadline_us - esp_timer_get_time()) / 1000;
  if (remaining_ms <= 0) {
    CheckDeadline(ctx);
    return false;
  }
  return esp_http_client_set_timeout_ms(client, static_cast<int>(remaining_ms)) == ESP_OK;
}

void CloseStream(StreamContext* ctx, esp_http_client_handle_t client)
{
  if (!ctx->close_requested) {
    ctx->close_requested = true;  // close dispatches DISCONNECTED synchronously
    // The HTTP owner closes the transport, keeping the parser/context alive
    // until the SDK read returns. cleanup() belongs exclusively to the caller.
    LogHttpTrace(ctx, "close_begin", ESP_OK);
    const esp_err_t result = esp_http_client_close(client);
    LogHttpTrace(ctx, "close_return", result);
  }
}

esp_err_t WriteRequestBody(StreamContext* ctx, esp_http_client_handle_t client,
                           const uint8_t* body, size_t body_size)
{
  size_t written = 0;
  while (written < body_size) {
    if (!ApplyRemainingTimeout(ctx, client)) {
      return ESP_FAIL;
    }
    const size_t chunk = std::min<size_t>(body_size - written, 2048);
    const int w = esp_http_client_write(
        client, reinterpret_cast<const char*>(body + written), chunk);
    if (w <= 0) {
      return ESP_FAIL;
    }
    written += static_cast<size_t>(w);
  }
  return ESP_OK;
}

// Capture after headers complete: ON_DATA may run inside fetch_headers(), so
// capture there too, before any body is dispatched to the business callback.
//
// Capturing from HTTP_EVENT_ON_HEADER cannot work: in IDF v5.5 the status code
// is assigned in http_on_headers_complete (esp_http_client.c:289), i.e. *after*
// http_on_header_event() dispatches the last ON_HEADER (:288), and
// http_on_status is a no-op (:237). fetch_headers() additionally resets
// status_code to -1 on entry (:1565), so every ON_HEADER callback observes
// -1 - latching it there (as this code once did) permanently suppressed the
// 4xx/5xx check.
void CaptureHttpStatus(StreamContext* ctx, esp_http_client_handle_t client)
{
  if (ctx == nullptr || client == nullptr || ctx->http_status > 0) {
    return;
  }
  const int status = esp_http_client_get_status_code(client);
  // A non-positive value means "not parsed yet", not "no status". Never latch
  // it, or every later check is suppressed.
  if (status <= 0) {
    return;
  }
  ctx->http_status = status;
  ESP_LOGI(kTag, "HTTP status=%d req=%s", status,
           wqn::voice_diagnostics::SafeKey(ctx->request.request_id.c_str()));
  if (status >= 400 && !ctx->fatal) {
    ctx->fatal = true;
    ctx->error_code = wqn::internal::AiStreamHttpErrorCode(status);
  }
}

esp_err_t OnHttpEvent(esp_http_client_event_t* evt)
{
  StreamContext* ctx = static_cast<StreamContext*>(evt->user_data);
  switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED: {
      LogHttpTrace(ctx, "socket_connected", ESP_OK);
      break;
    }
    case HTTP_EVENT_ON_HEADER: {
      // ESP-IDF v5.4+ removed HTTP_EVENT_HEADERS_RECEIVED in favour of
      // HTTP_EVENT_ON_HEADER, which fires once per header line. The status
      // code is deliberately not read here: it is still -1 at every ON_HEADER
      // (see CaptureHttpStatus). Clamp the next SDK wait to the same budget.
      if (!ApplyRemainingTimeout(ctx, evt->client)) {
        CloseStream(ctx, evt->client);
      }
      break;
    }
    case HTTP_EVENT_ON_DATA: {
      const int64_t now_us = esp_timer_get_time();
      ctx->last_data_us = now_us;
      ++ctx->data_callbacks;
      if (evt->data_len > 0) ctx->received_bytes += static_cast<uint64_t>(evt->data_len);
      if (ctx->first_data_us < 0) {
        ctx->first_data_us = now_us;
        LogHttpTrace(ctx, "first_data", ESP_OK);
      }
      CaptureHttpStatus(ctx, evt->client);
      if (!CheckDeadline(ctx) || ctx->fatal || ctx->terminal_event_seen) {
        CloseStream(ctx, evt->client);
        return ESP_OK;
      }
      if (!ctx->parser.feed(static_cast<const char*>(evt->data), evt->data_len)) {
        // A frame past the device's JSON budget cannot be parsed even when
        // complete, so the stream is broken rather than missing a frame.
        if (!ctx->fatal) {
          ctx->fatal = true;
          ctx->error_code = "frame_overflow";
          ctx->error_message = "stream frame exceeded the device limit";
        }
        CloseStream(ctx, evt->client);
        return ESP_OK;
      }
      std::string ev_name;
      uint64_t ev_id = 0;
      std::string ev_data;
      while (CheckDeadline(ctx)) {
        const auto frame = ctx->parser.extract(&ev_name, &ev_id, &ev_data);
        if (ctx->parser.overflowed()) {
          ctx->fatal = true;
          ctx->error_code = "frame_overflow";
          ctx->error_message = "stream frame exceeded the device limit";
          break;
        }
        if (frame != wqn::SseFrameBuffer::FrameState::kComplete) {
          break;
        }
        if (!ev_data.empty()) {
          DispatchEvent(ctx, ctx->parser, ev_name, ev_id, ev_data);
        }
        if (ctx->terminal_event_seen) {
          break;
        }
      }
      if (ctx->fatal || ctx->terminal_event_seen || !ApplyRemainingTimeout(ctx, evt->client)) {
        CloseStream(ctx, evt->client);
      }
      break;
    }
    case HTTP_EVENT_ON_FINISH: {
      // Unterminated frames are never delivered as completed business events.
      break;
    }
    case HTTP_EVENT_DISCONNECTED: {
      ESP_LOGD(kTag, "HTTP disconnected");
      if (!ctx->terminal_event_seen && ctx->error_code.empty()) {
        // transport-level disconnect before final/error frame
        ctx->error_code = "model_failed";
        ctx->error_message = "stream disconnected before completion";
      }
      break;
    }
    default:
      break;
  }
  return ESP_OK;
}

}  // namespace

namespace wqn {

namespace internal {

const char* AiStreamHttpErrorCode(int http_status)
{
  switch (http_status) {
    case 401: return "unauthorized";
    case 413: return "too_large";
    case 415:
    case 422: return "invalid_audio";
    case 429: return "rate_limited";
    case 504: return "chat_timeout";
    case 503: return "disabled";
    case 500: return "model_failed";
    case 502: return "provider_unavailable";
    default: return "bad_request";
  }
}

esp_err_t FinalizeAiStreamResult(bool fatal_http_status, esp_err_t transport_result)
{
  // A successfully fetched HTTP error response is still an application-level
  // failure. error_code is diagnostic output, not the success predicate.
  if (fatal_http_status) {
    return ESP_FAIL;
  }
  return transport_result;
}

}  // namespace internal

esp_err_t UploadAiAudioChatStream(const WqnAiStreamRequest& request,
                                  WqnAiChatResponse* response)
{
  if (request.callback == nullptr || response == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  *response = WqnAiChatResponse{};

  if (!wqn::IsValidAccessToken(request.token)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (request.pcm_data == nullptr || request.pcm_sample_count == 0 ||
      request.duration_ms <= 0 || request.pcm_sample_count > INT_MAX / sizeof(int16_t)) {
    return ESP_ERR_INVALID_ARG;
  }

  const int timeout_ms = request.timeout_ms > 0 ? request.timeout_ms : WQN_AI_SSE_TIMEOUT_MS;
  const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  const uint8_t* pcm_bytes =
      reinterpret_cast<const uint8_t*>(request.pcm_data);
  const size_t pcm_size = request.pcm_sample_count * sizeof(int16_t);

  // Build the URL with the protocol query so v2 servers unambiguously switch.
  std::string url = std::string(WQN_API_BASE) + WQN_AI_SSE_REQUEST_PATH +
                    "?protocol=v2-streaming";

  StreamContext ctx;
  ctx.request = request;
  ctx.response = response;
  ctx.mac = BuildMacAddress();
  ctx.deadline_us = deadline_us;
  ctx.started_us = deadline_us - static_cast<int64_t>(timeout_ms) * 1000;
  ESP_LOGI(kTag, "event=ai.http stage=begin req=%s budget_ms=%d pcm_bytes=%u",
           wqn::voice_diagnostics::SafeKey(request.request_id.c_str()), timeout_ms,
           static_cast<unsigned>(pcm_size));

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = timeout_ms;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 1024;                  // SSE lines are short; keep the buffer small
  cfg.buffer_size_tx = 4096;
  cfg.event_handler = OnHttpEvent;
  cfg.user_data = &ctx;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    LogHttpTrace(&ctx, "client_init_failed", ESP_ERR_NO_MEM);
    return ESP_ERR_NO_MEM;
  }

  std::string auth = "Bearer " + request.token;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
  esp_http_client_set_header(client, "X-WQN-Audio-Sample-Rate", "16000");
  esp_http_client_set_header(client, "X-WQN-Audio-Sample-Format", "s16le");
  esp_http_client_set_header(client, "X-WQN-Audio-Channels", "1");
  const std::string duration_ms = std::to_string(request.duration_ms);
  esp_http_client_set_header(client, "X-WQN-Audio-Duration-Ms", duration_ms.c_str());
  esp_http_client_set_header(client, WQN_AI_SSE_HEADER_PROTOCOL,
                             WQN_AI_SSE_PROTOCOL_VALUE);
  esp_http_client_set_header(client, WQN_AI_SSE_HEADER_ACCEPT,
                             WQN_AI_SSE_ACCEPT_VALUE);
  esp_http_client_set_header(client, "X-WQN-Device-Id", ctx.mac.c_str());
  esp_http_client_set_header(client, "X-WQN-Client-Version",
                             WQN_FIRMWARE_NAME "@" WQN_FIRMWARE_VERSION);
  if (!request.conversation_id.empty()) {
    esp_http_client_set_header(client, "X-WQN-Conversation-Id",
                               request.conversation_id.c_str());
  }
  if (!request.tier.empty()) {
    esp_http_client_set_header(client, "X-WQN-Ai-Tier", request.tier.c_str());
  }
  if (!request.request_id.empty()) {
    esp_http_client_set_header(client, "X-WQN-Request-Id", request.request_id.c_str());
  }
  esp_http_client_set_header(
      client, "X-WQN-Enable-Thinking",
      request.enable_thinking ? "true" : "false");
  if (!request.reasoning_effort.empty()) {
    esp_http_client_set_header(
        client, "X-WQN-Reasoning-Effort", request.reasoning_effort.c_str());
  }

  ctx.phase = "open";
  esp_err_t err = ApplyRemainingTimeout(&ctx, client)
                      ? esp_http_client_open(client, static_cast<int>(pcm_size)) : ESP_FAIL;
  if (err == ESP_OK) {
    ctx.phase = "upload";
    err = WriteRequestBody(&ctx, client, pcm_bytes, pcm_size);
    LogHttpTrace(&ctx, "upload_return", err);
  }

  if (err == ESP_OK && ApplyRemainingTimeout(&ctx, client)) {
    ctx.phase = "headers";
    // fetch_headers returns the response Content-Length (0 for a chunked SSE
    // response), not esp_err_t. Treat every non-negative value as success so a
    // buffered/non-chunked SSE response is still consumed.
    const int64_t header_result = esp_http_client_fetch_headers(client);
    // Capture unconditionally: a failed fetch can still leave a parsed status
    // behind, and a missed 4xx/5xx would otherwise surface as a generic
    // transport error instead of its mapped error_code.
    CaptureHttpStatus(&ctx, client);
    err = header_result < 0 ? ESP_FAIL : ESP_OK;
    if (err == ESP_OK && ctx.http_status <= 0) {
      ctx.fatal = true;
      ctx.error_code = "bad_response";
      ctx.error_message = "HTTP response status unavailable";
      err = ESP_FAIL;
    }
  }
  if (err == ESP_OK && !ctx.fatal && !ctx.terminal_event_seen) {
    ctx.phase = "read";
    char buf[1024];
    while (!ctx.fatal && !ctx.terminal_event_seen && ApplyRemainingTimeout(&ctx, client)) {
      const int r = esp_http_client_read(client, buf, sizeof(buf));
      ctx.last_read_result = r;
      ctx.last_read_errno = r < 0 ? esp_http_client_get_errno(client) : 0;
      if (ctx.terminal_event_seen || ctx.fatal) {
        LogHttpTrace(&ctx, "read_return", r < 0 ? ESP_FAIL : ESP_OK);
      }
      if (r < 0) {
        err = ESP_FAIL;
        break;
      }
      if (r == 0) {
        break;  // EOF; either graceful or close
      }
    }
  }

  if (!ctx.fatal && !ctx.terminal_event_seen) {
    CheckDeadline(&ctx);
    if (!ctx.fatal) {
      ctx.fatal = true;
      if (ctx.error_code.empty()) {
        ctx.error_code = "model_failed";
        ctx.error_message = "stream ended before completion";
      }
    }
  }
  if (ctx.terminal_event_seen && !ctx.fatal) {
    err = ESP_OK;  // intentional close may make the SDK read return an error
  }
  CloseStream(&ctx, client);
  LogHttpTrace(&ctx, "cleanup_begin", ESP_OK);
  const esp_err_t cleanup_result = esp_http_client_cleanup(client);
  LogHttpTrace(&ctx, "cleanup_return", cleanup_result);

  // Populate caller-facing response fields.
  if (!ctx.error_code.empty() && response != nullptr) {
    response->error_code = ctx.error_code;
    response->error_message = ctx.error_message;
  }

  // The user callback may have already gotten a `final` event. Either way,
  // return ESP_OK when the stream completed; transport-level failures map to
  // ESP_FAIL and surface via `response->error_*` fields.
  const esp_err_t result =
      ctx.timed_out ? ESP_ERR_TIMEOUT : internal::FinalizeAiStreamResult(ctx.fatal, err);
  LogHttpTrace(&ctx, "return", result);
  return result;
}

}  // namespace wqn
