#include "sse_chunk.h"

#include <cstdlib>
#include <cstring>
#include <utility>
#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"

namespace wqn {

bool LineStreamingBuffer::feed(const char* data, size_t len)
{
  if (data == nullptr || len == 0) {
    return true;
  }
  if (buffer_.size() + len > kMaxSseLineBytes) {
    // An unterminated line this long is not a frame we could ever parse, and
    // letting it keep growing spends internal RAM on garbage.
    buffer_.clear();
    return false;
  }
  buffer_.append(data, len);
  return true;
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

bool SseFrameBuffer::feed(const char* data, size_t len)
{
  return lines_.feed(data, len);
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
      // A frame's payload is the sum of its data: lines, so the cap has to be
      // checked on the accumulation rather than per line.
      if (data_.size() + value.size() + 1 > kMaxSseFrameBytes) {
        // Sticky flag first: clear() deliberately does not reset it, so the
        // caller can see the drop and fail the stream instead of reading on
        // through the spliced remainder of the oversized frame.
        overflowed_ = true;
        clear();
        return FrameState::kPartial;
      }
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

  cJSON* root = protocol::JsonNestingWithinLimit(
                    data_json.data(), data_json.size())
      ? cJSON_ParseWithLength(data_json.c_str(), data_json.size())
      : nullptr;
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
  else if (ev_name == "interaction.request")
    k = Kind::kInteractionRequest;
  else if (ev_name == "interaction.waiting")
    k = Kind::kInteractionWaiting;
  else if (ev_name == "interaction.closed")
    k = Kind::kInteractionClosed;
  else if (ev_name == "tool.start")    k = Kind::kToolStart;
  else if (ev_name == "tool.result")   k = Kind::kToolResult;
  else if (ev_name == "tool.error")    k = Kind::kToolError;
  else if (ev_name == "state")         k = Kind::kState;
  else if (ev_name == "turn.done" || ev_name == "response.done")
                                            k = Kind::kTurnDone;
  else if (ev_name == "error")         k = Kind::kError;
  else if (ev_name == "final")         k = Kind::kFinal;
  if (k == Kind::kInteractionClosed || k == Kind::kInteractionWaiting) {
    const char* id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "interaction_id"));
    if (id != nullptr)
      out_ev->interaction.id = id;
  }
  if (k == Kind::kInteractionRequest) {
    const auto field = [&](const char* name) {
      const char* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name));
      return std::string(value != nullptr ? value : "");
    };
    AiInteraction next;
    next.id = field("interaction_id");
    next.kind = field("kind");
    next.prompt = field("prompt");
    next.allow_custom = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "allow_custom"));
    cJSON* options = cJSON_GetObjectItemCaseSensitive(root, "options");
    bool valid_id = next.id.size() == 36;
    if (valid_id)
      for (size_t i = 0; i < next.id.size(); ++i) {
        const char ch = next.id[i];
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        valid_id = valid_id && (separator ? ch == '-'
                                          : (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                                                (ch >= 'A' && ch <= 'F'));
      }
    bool valid = valid_id && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "allow_custom")) &&
                 (next.kind == "write" || next.kind == "question") && !next.prompt.empty() &&
                 next.prompt.size() <= 8192 && cJSON_IsArray(options) &&
                 cJSON_GetArraySize(options) >= 1 && cJSON_GetArraySize(options) <= 8;
    if (valid)
      for (int i = 0; i < cJSON_GetArraySize(options); ++i) {
        cJSON* option = cJSON_GetArrayItem(options, i);
        const char* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(option, "value"));
        const char* label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(option, "label"));
        if (value == nullptr || label == nullptr || value[0] == 0 || label[0] == 0 ||
            std::strlen(value) > 512 || std::strlen(label) > 320 ||
            std::strcmp(value, "__custom__") == 0) {
          valid = false;
          break;
        }
        for (const auto& prior : next.options)
          if (prior.value == value)
            valid = false;
        next.options.push_back({value, label});
      }
    if (next.kind == "write" &&
        (next.allow_custom || next.options.size() != 2 || next.options[0].value != "approve" ||
         next.options[1].value != "reject"))
      valid = false;
    if (!valid) {
      cJSON_Delete(root);
      return false;
    }
    out_ev->interaction = std::move(next);
  }
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
