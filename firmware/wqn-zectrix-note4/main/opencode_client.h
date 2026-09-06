#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio_capture.h"
#include "esp_err.h"

namespace wqn {

struct OpenCodeSessionInfo {
    std::string id;
    std::string title;
    int64_t updated_at = 0;
};

struct OpenCodeResult {
    int http_status = 0;
    std::string error_code;
    std::string detail;
};

enum class OpenCodeEventKind : uint8_t {
    kAccepted,
    kAttached,
    kStatus,
    kTextDelta,
    kText,
    kTool,
    kPermission,
    kError,
};

struct OpenCodeEvent {
    OpenCodeEventKind kind = OpenCodeEventKind::kStatus;
    std::string status;
    std::string text;
    std::string tool;
    std::string preview;
    std::string permission_id;
};

using OpenCodeEventCallback = void (*)(const OpenCodeEvent& event, void* ctx);

struct OpenCodeOutboundReply {
    std::string permission_id;
    bool approve = true;
};

// Invoked on the streaming worker when an outbound permission reply could not
// be delivered, so the session layer can restore the pending-ask UI instead of
// silently leaving the run blocked.
using OpenCodeReplyFailedCallback = void (*)(const OpenCodeOutboundReply& reply,
                                             esp_err_t error,
                                             void* user_ctx);

// Thread-safe handoff for permission replies issued while an agent event
// stream is open. The UI thread pushes; the streaming worker drains between
// reads (the gateway keep-alive guarantees a read returns at least every ~15
// s) and performs the reply POST itself, so no second task or long-lived TLS
// connection ever runs alongside the stream.
class OpenCodeOutboundQueue {
public:
    void Push(OpenCodeOutboundReply reply);
    bool Pop(OpenCodeOutboundReply* out);

private:
    std::mutex mutex_;
    std::vector<OpenCodeOutboundReply> items_;
};

esp_err_t ListOpenCodeSessions(
    const std::string& token,
    std::vector<OpenCodeSessionInfo>* sessions,
    OpenCodeResult* result);
esp_err_t CreateOpenCodeSession(
    const std::string& token,
    OpenCodeSessionInfo* session,
    OpenCodeResult* result);
esp_err_t TranscribeOpenCodeAudio(
    const std::string& token,
    const AudioCaptureChunk& audio,
    std::string* transcript,
    OpenCodeResult* result);
esp_err_t RunOpenCodePrompt(
    const std::string& token,
    const std::string& session_id,
    const std::string& prompt,
    OpenCodeOutboundQueue* outbound_replies,
    OpenCodeReplyFailedCallback reply_failed,
    void* reply_failed_ctx,
    OpenCodeEventCallback callback,
    void* callback_ctx,
    OpenCodeResult* result);
esp_err_t WatchOpenCodeSession(
    const std::string& token,
    const std::string& session_id,
    OpenCodeOutboundQueue* outbound_replies,
    OpenCodeReplyFailedCallback reply_failed,
    void* reply_failed_ctx,
    OpenCodeEventCallback callback,
    void* callback_ctx,
    OpenCodeResult* result);

}  // namespace wqn
