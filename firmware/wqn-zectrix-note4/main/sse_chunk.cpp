#include "sse_chunk.h"

#include <cstdlib>
#include <utility>
#include "cJSON.h"

namespace wqn {

void LineStreamingBuffer::feed(const char* data, size_t len)
{
  if (data == nullptr || len == 0) {
    return;
  }
  buffer_.append(data, len);
}

bool LineStreamingBuffer::take_line(std::string* out)
{
  if (out == nullptr) {
    return false;
  }
  out->clear();
  const size_t n = buffer_.size();
  for (size_t i = 0; i < n; ++i) {
    if (buffer_[i] == '\n') {
      out->assign(buffer_.data(), i);
      // trim trailing CR
      if (!out->empty() && out->back() == '\r') {
        out->pop_back();
      }
      buffer_.erase(0, i + 1);
      return true;
    }
  }
  return false;
}

void SseFrameBuffer::clear()
{
  lines_.clear();
  event_.clear();
  data_.clear();
  id_ = 0;
  id_seen_ = false;
}

void SseFrameBuffer::feed(const char* data, size_t len)
{
  lines_.feed(data, len);
}

SseFrameBuffer::FrameState SseFrameBuffer::extract(std::string* event_name,
                                                   uint64_t* event_id,
                                                   std::string* data_json)
{
  if (event_name != nullptr) event_name->clear();
  if (event_id != nullptr) *event_id = 0;
  if (data_json != nullptr) data_json->clear();

  std::string line;
  while (lines_.take_line(&line)) {
    if (line.empty()) {
      // Empty line = frame boundary.
      if (event_.empty() && data_.empty()) {
        // Skip leading empty lines / keep-alive comments.
        continue;
      }
      if (event_name != nullptr) *event_name = event_;
      if (event_id != nullptr) *event_id = id_;
      if (data_json != nullptr) *data_json = std::move(data_);
      event_.clear();
      data_.clear();
      id_ = 0;
      id_seen_ = false;
      return FrameState::kComplete;
    }

    if (line[0] == ':') {
      // SSE comment / keep-alive. Discard.
      continue;
    }

    const size_t colon = line.find(':');
    const std::string field = (colon == std::string::npos) ? line : line.substr(0, colon);
    std::string value;
    if (colon != std::string::npos) {
      size_t start = colon + 1;
      if (start < line.size() && line[start] == ' ') {
        ++start;
      }
      value.assign(line.data() + start, line.size() - start);
    }

    if (field == "event") {
      event_ = std::move(value);
    } else if (field == "data") {
      if (!data_.empty()) data_.push_back('\n');
      data_ += value;
    } else if (field == "id") {
      // Per SSE spec: empty id is allowed and disables the Last-Event-ID header;
      // we still parse numeric ids for log recovery.  Tolerant of trailing garbage.
      char* endp = nullptr;
      const uint64_t parsed = std::strtoull(value.c_str(), &endp, 10);
      if (endp != value.c_str()) {
        id_ = parsed;
        id_seen_ = true;
      }
    }
  }
  return FrameState::kPartial;
}

bool DecodeSseEvent(const std::string& event_name,
                    uint64_t event_id,
                    const std::string& data_json,
                    WqnAiSseEvent* out_ev,
                    cJSON** out_root)
{
  if (out_ev == nullptr) {
    return false;
  }
  if (out_root != nullptr) {
    *out_root = nullptr;
  }

  out_ev->event_id = event_id;
  out_ev->raw_json = data_json;

  cJSON* root = cJSON_ParseWithLength(data_json.c_str(), data_json.size());
  if (root == nullptr) {
    return false;
  }

  const std::string ev_name = event_name;
  using Kind = WqnAiSseEvent::Kind;
  Kind k = Kind::kUnknown;
  if      (ev_name == "ready")        k = Kind::kReady;
  else if (ev_name == "stage")        k = Kind::kStage;
  else if (ev_name == "asr.delta")    k = Kind::kAsrDelta;
  else if (ev_name == "asr.complete") k = Kind::kAsrComplete;
  else if (ev_name == "asr.failed")   k = Kind::kAsrFailed;
  else if (ev_name == "thinking.start" || ev_name == "reasoning.start" ||
           ev_name == "response.thinking.start" || ev_name == "response.reasoning.start")
                                            k = Kind::kThinkingStart;
  else if (ev_name == "thinking.delta" || ev_name == "reasoning.delta" ||
           ev_name == "response.thinking.delta" || ev_name == "response.reasoning.delta" ||
           ev_name == "response.reasoning_summary_text.delta")
                                            k = Kind::kThinkingDelta;
  else if (ev_name == "thinking.done" || ev_name == "thinking.end" ||
           ev_name == "reasoning.done" || ev_name == "reasoning.end" ||
           ev_name == "response.thinking.done" || ev_name == "response.reasoning.done" ||
           ev_name == "response.reasoning_summary_text.done")
                                            k = Kind::kThinkingDone;
  else if (ev_name == "text.start")    k = Kind::kTextStart;
  else if (ev_name == "text.delta")    k = Kind::kTextDelta;
  else if (ev_name == "text.end")      k = Kind::kTextEnd;
  else if (ev_name == "tool.start")    k = Kind::kToolStart;
  else if (ev_name == "tool.result")   k = Kind::kToolResult;
  else if (ev_name == "tool.error")    k = Kind::kToolError;
  else if (ev_name == "state")         k = Kind::kState;
  else if (ev_name == "turn.done" || ev_name == "response.done")
                                            k = Kind::kTurnDone;
  else if (ev_name == "error")         k = Kind::kError;
  else if (ev_name == "final")         k = Kind::kFinal;
  out_ev->kind = k;

  cJSON* n = nullptr;
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "delta")) != nullptr && cJSON_IsString(n)) {
    out_ev->delta = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "text")) != nullptr && cJSON_IsString(n)) {
    out_ev->text = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "full_text")) != nullptr && cJSON_IsString(n)) {
    out_ev->full_text = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "sentence_id")) != nullptr && cJSON_IsString(n)) {
    out_ev->sentence_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "tool_call_id")) != nullptr && cJSON_IsString(n)) {
    out_ev->tool_call_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "name")) != nullptr && cJSON_IsString(n)) {
    out_ev->tool_name = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "display")) != nullptr && cJSON_IsString(n)) {
    out_ev->tool_display = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "ok")) != nullptr && cJSON_IsBool(n)) {
    out_ev->tool_ok = cJSON_IsTrue(n);
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "items_count")) != nullptr && cJSON_IsNumber(n)) {
    out_ev->tool_items_count = n->valueint;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "elapsed_ms")) != nullptr && cJSON_IsNumber(n)) {
    out_ev->elapsed_ms = n->valueint;
    out_ev->tool_elapsed_ms = n->valueint;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "stage")) != nullptr && cJSON_IsString(n)) {
    out_ev->stage = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "text_chars")) != nullptr && cJSON_IsNumber(n)) {
    out_ev->text_chars = n->valueint;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "error_code")) != nullptr && cJSON_IsString(n)) {
    out_ev->error_code = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "message")) != nullptr && cJSON_IsString(n)) {
    out_ev->error_message = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "stage")) != nullptr && cJSON_IsString(n) && k == Kind::kError) {
    out_ev->error_stage = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "conversation_id")) != nullptr && cJSON_IsString(n)) {
    out_ev->conversation_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "request_id")) != nullptr && cJSON_IsString(n)) {
    out_ev->request_id = n->valuestring;
  }
  if ((n = cJSON_GetObjectItemCaseSensitive(root, "latency_ms")) != nullptr && cJSON_IsNumber(n)) {
    out_ev->latency_ms = n->valueint;
  }

  if (out_root != nullptr) {
    *out_root = root;
  } else {
    cJSON_Delete(root);
  }
  return true;
}

}  // namespace wqn
