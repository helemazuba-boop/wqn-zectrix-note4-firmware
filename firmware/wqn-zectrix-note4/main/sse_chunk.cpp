#include "sse_chunk.h"

#include <cstdlib>
#include <utility>

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

}  // namespace wqn
