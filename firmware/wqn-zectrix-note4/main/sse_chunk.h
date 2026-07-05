// sse_chunk.h - small SSE frame parser and multipart builders used by the v2
// AI streaming client (wqn_api.cpp::UploadAiAudioChatStream).
//
// Kept as a separate header so ai_session.cpp and flash_session.cpp can share
// the parser without dragging in cJSON at every call site. Designed for the
// embedded constraints of the ESP32: no exceptions, no RTTI, ~2 KB stack.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wqn {

// Multipart encoder for `multipart/form-data` bodies.
//
// We feed `out` with a single contiguous buffer (no streaming writeback), then
// hand it to esp_http_client. Each part has the same Content-Disposition shape
// produced by curl/Firefox so SSE proxies that sniff the body type still work.
class MultipartEncoder {
public:
  void begin(const std::string& hint);

  void part_json(const std::string& name, const std::string& body);
  void part_string(const std::string& name, const std::string& body);
  void part_binary(const std::string& name, const std::string& filename,
                   const void* data, size_t size);

  void end();

  const std::string& buffer() const { return buffer_; }
  const std::string& boundary() const { return boundary_; }

  // Returns the Content-Type header value, e.g. "multipart/form-data; boundary=----WQN-AI-...".
  std::string boundary_for_header() const
  {
    return "multipart/form-data; boundary=" + boundary_;
  }

private:
  std::string boundary_;
  std::string buffer_;
};

// LineStreamingBuffer accumulates bytes from many esp_http_client_read()
// calls and yields one line at a time to the SSE parser.  Lines are split on
// both '\n' and "\r\n" so it works regardless of whether the HTTP stack emits
// chunked-encoding CRLFs or LF separators.
class LineStreamingBuffer {
public:
  void feed(const char* data, size_t len);
  bool take_line(std::string* out);   // returns false when no full line remains
  void clear() { buffer_.clear(); }

private:
  std::string buffer_;
};

// SseFrameBuffer parses SSE frames out of line-delimited input:
//   event: ready
//   id: 17
//   data: {...}
//
//   event: text.delta
//   data: {"delta":"先"}
//
// A frame is considered complete when an empty line (\n\n or \r\n\r\n) is seen
// or when the underlying `data:` line ends with our sentinel "\n\n" injected
// by the server for keep-alive.
class SseFrameBuffer {
public:
  enum class FrameState {
    kPartial,
    kComplete,
  };

  void feed(const char* data, size_t len);
  FrameState extract(std::string* event_name, uint64_t* event_id, std::string* data_json);

  void clear();

private:
  LineStreamingBuffer lines_;
  std::string event_;
  uint64_t id_ = 0;
  std::string data_;
  bool id_seen_ = false;
};

}  // namespace wqn
