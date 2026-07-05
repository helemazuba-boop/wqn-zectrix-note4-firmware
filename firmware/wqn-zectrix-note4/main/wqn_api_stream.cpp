// wqn_api_stream.cpp - Implementation of the v2 SSE streaming AI client.
//
// Design notes:
//   * We use a single esp_http_client in chunked mode. Post body is the
//     multipart buffer built by MultipartEncoder.
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
#include <ctime>
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
};

bool ShouldKeepEvent(const wqn::WqnAiSseEvent& event)
{
  return true;
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

void DispatchEvent(StreamContext* ctx, const wqn::SseFrameBuffer& frame,
                   const std::string& event_name, uint64_t event_id,
                   const std::string& data_json)
{
  ctx->last_event_id = event_id;
  if (ctx->request.callback == nullptr) {
    return;
  }
  wqn::WqnAiSseEvent ev;
  ev.event_id = event_id;
  ev.raw_json = data_json;
  cJSON* root = cJSON_ParseWithLength(data_json.c_str(), data_json.size());
  if (root == nullptr) {
    ESP_LOGW(kTag, "SSE JSON parse failed: %s",
             data_json.size() > 80 ? "<large>" : data_json.c_str());
    return;
  }

  const std::string ev_name = event_name;
  // event_kind lookup
  auto kind_from = [&](const char* n, wqn::WqnAiSseEvent::Kind k){
    return ev_name == n ? k : wqn::WqnAiSseEvent::Kind::kReady;
  };
  wqn::WqnAiSseEvent::Kind k = wqn::WqnAiSseEvent::Kind::kReady;
  if      (ev_name == "ready")       k = wqn::WqnAiSseEvent::Kind::kReady;
  else if (ev_name == "stage")       k = wqn::WqnAiSseEvent::Kind::kStage;
  else if (ev_name == "asr.delta")   k = wqn::WqnAiSseEvent::Kind::kAsrDelta;
  else if (ev_name == "asr.complete") k = wqn::WqnAiSseEvent::Kind::kAsrComplete;
  else if (ev_name == "asr.failed")   k = wqn::WqnAiSseEvent::Kind::kAsrFailed;
  else if (ev_name == "text.start")   k = wqn::WqnAiSseEvent::Kind::kTextStart;
  else if (ev_name == "text.delta")   k = wqn::WqnAiSseEvent::Kind::kTextDelta;
  else if (ev_name == "text.end")     k = wqn::WqnAiSseEvent::Kind::kTextEnd;
  else if (ev_name == "tool.start")   k = wqn::WqnAiSseEvent::Kind::kToolStart;
  else if (ev_name == "tool.result")  k = wqn::WqnAiSseEvent::Kind::kToolResult;
  else if (ev_name == "tool.error")   k = wqn::WqnAiSseEvent::Kind::kToolError;
  else if (ev_name == "state")        k = wqn::WqnAiSseEvent::Kind::kState;
  else if (ev_name == "error")        k = wqn::WqnAiSseEvent::Kind::kError;
  else if (ev_name == "final")        k = wqn::WqnAiSseEvent::Kind::kFinal;
  ev.kind = k;

  // Now copy relevant fields.
  cJSON* n = nullptr;

  if ((n = cJSON_GetObjectItemCaseSensitive(root, "delta")) != nullptr && cJSON_IsString(n)) {
    ev.delta = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "text")) != nullptr && cJSON_IsString(n)) {
    ev.text = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "full_text")) != nullptr && cJSON_IsString(n)) {
    ev.full_text = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "sentence_id")) != nullptr && cJSON_IsString(n)) {
    ev.sentence_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "tool_call_id")) != nullptr && cJSON_IsString(n)) {
    ev.tool_call_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "name")) != nullptr && cJSON_IsString(n)) {
    ev.tool_name = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "display")) != nullptr && cJSON_IsString(n)) {
    ev.tool_display = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "ok")) != nullptr && cJSON_IsBool(n)) {
    ev.tool_ok = cJSON_IsTrue(n);
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "items_count")) != nullptr && cJSON_IsNumber(n)) {
    ev.tool_items_count = n->valueint;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "elapsed_ms")) != nullptr && cJSON_IsNumber(n)) {
    ev.elapsed_ms = n->valueint;
    ev.tool_elapsed_ms = n->valueint;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "stage")) != nullptr && cJSON_IsString(n)) {
    ev.stage = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "text_chars")) != nullptr && cJSON_IsNumber(n)) {
    ev.text_chars = n->valueint;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "error_code")) != nullptr && cJSON_IsString(n)) {
    ev.error_code = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "message")) != nullptr && cJSON_IsString(n)) {
    ev.error_message = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "stage")) != nullptr && cJSON_IsString(n) && k == wqn::WqnAiSseEvent::Kind::kError) {
    ev.error_stage = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "conversation_id")) != nullptr && cJSON_IsString(n)) {
    ev.conversation_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "latency_ms")) != nullptr && cJSON_IsNumber(n)) {
    ev.latency_ms = n->valueint;
  }

  // Snapshot conversation_id into the response (used by the final callback).
  if (!ev.conversation_id.empty() && ctx->response != nullptr) {
    ctx->response->conversation_id = ev.conversation_id;
  }

  // Drive ctx-level error state if this is an `error` frame.
  if (k == wqn::WqnAiSseEvent::Kind::kError) {
    ctx->error_code = ev.error_code.empty() ? "model_failed" : ev.error_code;
    ctx->error_message = ev.error_message;
  } else if (k == wqn::WqnAiSseEvent::Kind::kFinal && ctx->response != nullptr) {
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

esp_err_t WriteMultipartBody(esp_http_client_handle_t client,
                             const std::string& body)
{
  size_t written = 0;
  while (written < body.size()) {
    const size_t chunk = std::min<size_t>(body.size() - written, 2048);
    const int w = esp_http_client_write(client, body.data() + written, chunk);
    if (w <= 0) {
      return ESP_FAIL;
    }
    written += static_cast<size_t>(w);
  }
  return ESP_OK;
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
      if (ctx->http_status == 0) {
        ctx->http_status = esp_http_client_get_status_code(evt->client);
        ESP_LOGI(kTag, "HTTP status=%d", ctx->http_status);
        if (ctx->http_status >= 400) {
          ctx->fatal = true;
          switch (ctx->http_status) {
            case 401: ctx->error_code = "unauthorized"; break;
            case 413: ctx->error_code = "too_large"; break;
            case 415:
            case 422: ctx->error_code = "invalid_audio"; break;
            case 429: ctx->error_code = "rate_limited"; break;
            case 504: ctx->error_code = "chat_timeout"; break;
            case 503: ctx->error_code = "disabled"; break;
            case 500: ctx->error_code = "model_failed"; break;
            case 502: ctx->error_code = "provider_unavailable"; break;
            default:  ctx->error_code = "bad_request";
          }
        }
      }
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
      if (ctx->error_code.empty()) {
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

  // Build the multipart body.
  wqn::MultipartEncoder mp;
  mp.begin("ai-audio");

  char audio_meta[256];
  std::snprintf(audio_meta, sizeof(audio_meta),
                "{\"sample_rate\":16000,\"sample_format\":\"s16le\",\"channels\":1,\"duration_ms\":%d,\"byte_size\":%u}",
                request.duration_ms,
                static_cast<unsigned>(request.pcm.size() * sizeof(int16_t)));
  mp.part_json("audio_meta", std::string(audio_meta));

  char session_meta[256];
  std::time_t now = 0;
  std::time(&now);
  int64_t boot_ms = esp_timer_get_time() / 1000;
  std::snprintf(session_meta, sizeof(session_meta),
                "{\"started_at_ms\":%lld,\"ui_locale\":\"zh-CN\",\"active_tier\":\"%s\",\"boot_ms\":%lld}",
                static_cast<long long>(now) * 1000LL,
                request.tier.empty() ? "std" : request.tier.c_str(),
                static_cast<long long>(boot_ms));
  mp.part_json("session_meta", std::string(session_meta));

  const uint8_t* pcm_bytes = reinterpret_cast<const uint8_t*>(request.pcm.data());
  const size_t pcm_size = request.pcm.size() * sizeof(int16_t);
  const std::string fname =
      "turn-" + std::to_string(static_cast<long long>(now)) + ".pcm";
  mp.part_binary("audio", fname, pcm_bytes, pcm_size);
  mp.end();

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
  cfg.buffer_size_tx = 4096;                // multipart body is large
  cfg.event_handler = OnHttpEvent;
  cfg.user_data = &ctx;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  std::string auth = "Bearer " + request.token;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", nullptr);  // we override via the body
  esp_http_client_set_header(client, "Content-Type", mp.boundary_for_header().c_str());
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

  const std::string& body = mp.buffer();
  esp_err_t err = esp_http_client_open(client, body.size());
  if (err == ESP_OK) {
    err = WriteMultipartBody(client, body);
  }

  if (err == ESP_OK) {
    err = esp_http_client_fetch_headers(client);
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
  if (ctx.fatal && response->error_code.empty()) {
    return ESP_FAIL;
  }
  if (err != ESP_OK) {
    return err;
  }
  return ESP_OK;
}

}  // namespace wqn
