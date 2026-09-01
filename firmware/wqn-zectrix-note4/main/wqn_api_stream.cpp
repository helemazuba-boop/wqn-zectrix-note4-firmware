// wqn_api_stream.cpp - Implementation of the v2 SSE streaming AI client.
//
// Design notes:
//   * The request body is raw 16 kHz mono s16le PCM. Metadata travels in the
//     X-WQN-* headers shared with the cloud route's validated contract.
//   * The event handler drives an SSE frame parser that calls back into the
//     caller's WqnAiSseCallback on every complete frame.
//   * We tolerate mid-stream disconnects: a partial frame is discarded and
//     ESP_FAIL is returned after the 10 minute outer timeout.
//
// We intentionally DO NOT touch the existing v1 UploadAiAudioChat /
// HttpBinaryPost path so that re-enabling v1 fallback stays a one-line config.

#include "wqn_api.h"

#include <algorithm>
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
#include "sse_chunk.h"
#include "storage.h"
#include "wqn_api_stream_internal.h"

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
};

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
    ESP_LOGW(kTag, "SSE JSON parse failed: %s",
             data_json.size() > 80 ? "<large>" : data_json.c_str());
    return;
  }

  const auto k = ev.kind;

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

  cJSON_Delete(root);
  ctx->request.callback(ev, ctx->request.user_ctx);
}

esp_err_t WriteRequestBody(esp_http_client_handle_t client,
                           const uint8_t* body, size_t body_size)
{
  size_t written = 0;
  while (written < body_size) {
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

void CaptureHttpStatus(StreamContext* ctx, esp_http_client_handle_t client)
{
  if (ctx == nullptr || client == nullptr || ctx->http_status > 0) {
    return;
  }
  const int status = esp_http_client_get_status_code(client);
  // IDF can emit the first HTTP_EVENT_ON_HEADER before exposing the parsed
  // status code. Do not latch its transient -1 value and suppress all later
  // checks; fetch_headers() calls this helper again once parsing is complete.
  if (status <= 0) {
    return;
  }
  ctx->http_status = status;
  ESP_LOGI(kTag, "HTTP status=%d", status);
  if (status >= 400) {
    ctx->fatal = true;
    ctx->error_code = wqn::internal::AiStreamHttpErrorCode(status);
  }
}

esp_err_t OnHttpEvent(esp_http_client_event_t* evt)
{
  StreamContext* ctx = static_cast<StreamContext*>(evt->user_data);
  switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED: {
      ESP_LOGD(kTag, "HTTP connected");
      break;
    }
    case HTTP_EVENT_ON_HEADER: {
      // ESP-IDF v5.4+ removed HTTP_EVENT_HEADERS_RECEIVED in favour of
      // HTTP_EVENT_ON_HEADER which fires once per header line. We only care
      // about the status code, so grab it on the first header and ignore the
      // rest. Subsequent header events fall through to the parser unchanged.
      CaptureHttpStatus(ctx, evt->client);
      break;
    }
    case HTTP_EVENT_ON_DATA: {
      if (ctx->fatal) {
        // discard the body of an error response; we'll surface the code shortly
        return ESP_OK;
      }
      ctx->parser.feed(static_cast<const char*>(evt->data), evt->data_len);
      std::string ev_name;
      uint64_t ev_id = 0;
      std::string ev_data;
      while (ctx->parser.extract(&ev_name, &ev_id, &ev_data) ==
             wqn::SseFrameBuffer::FrameState::kComplete) {
        if (!ev_data.empty()) {
          DispatchEvent(ctx, ctx->parser, ev_name, ev_id, ev_data);
        }
      }
      break;
    }
    case HTTP_EVENT_ON_FINISH: {
      // flush a partial frame so the user callback sees `final` if it was the last
      // partial chunk of the stream.
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
      request.duration_ms <= 0) {
    return ESP_ERR_INVALID_ARG;
  }

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

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = request.timeout_ms > 0 ? request.timeout_ms : WQN_AI_SSE_TIMEOUT_MS;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 1024;                  // SSE lines are short; keep the buffer small
  cfg.buffer_size_tx = 4096;
  cfg.event_handler = OnHttpEvent;
  cfg.user_data = &ctx;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
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

  esp_err_t err = esp_http_client_open(client, pcm_size);
  if (err == ESP_OK) {
    err = WriteRequestBody(client, pcm_bytes, pcm_size);
  }

  if (err == ESP_OK) {
    // fetch_headers returns the response Content-Length (which may be a
    // positive byte count), not esp_err_t. Treat every non-negative value as
    // success so a buffered/non-chunked SSE response is still consumed.
    const int64_t header_result = esp_http_client_fetch_headers(client);
    err = header_result < 0 ? ESP_FAIL : ESP_OK;
    if (err == ESP_OK) {
      CaptureHttpStatus(&ctx, client);
      if (ctx.http_status <= 0) {
        ctx.error_code = "bad_response";
        ctx.error_message = "HTTP response status unavailable";
        err = ESP_FAIL;
      }
    }
  }
  if (err == ESP_OK) {
    // Pull the chunked stream until close.
    char buf[1024];
    while (true) {
      const int r = esp_http_client_read(client, buf, sizeof(buf));
      if (r < 0) {
        err = ESP_FAIL;
        break;
      }
      if (r == 0) {
        break;  // EOF; either graceful or close
      }
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  // Populate caller-facing response fields.
  if (!ctx.error_code.empty() && response != nullptr) {
    response->error_code = ctx.error_code;
    response->error_message = ctx.error_message;
  }

  // The user callback may have already gotten a `final` event. Either way,
  // return ESP_OK when the stream completed; transport-level failures map to
  // ESP_FAIL and surface via `response->error_*` fields.
  return internal::FinalizeAiStreamResult(ctx.fatal, err);
}

}  // namespace wqn
