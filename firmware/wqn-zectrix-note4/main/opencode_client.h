#pragma once

#include <atomic>
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

// One answerable option of an `agent.question` ask. The gateway projects the
// upstream form's fields onto at most two of these, because the option bar has
// two slots; the device only ever sends the chosen `value` back.
struct OpenCodeQuestionOption {
    std::string value;
    std::string label;
};

// A tool call inside a backfilled turn. `status` is one of the three the
// gateway projects (`running`, `done`, `error`).
struct OpenCodeHistoryTool {
    std::string name;
    std::string status;
    std::string preview;
};

// One backfilled conversation row. `role` is `user` or `assistant`; `thinking`
// and `tools` are only present on assistant rows.
struct OpenCodeHistoryMessage {
    std::string role;
    std::string text;
    std::string thinking;
    std::vector<OpenCodeHistoryTool> tools;
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
    kReasoningDelta,
    kReasoning,
    kTool,
    kPermission,
    kQuestion,
    kError,
};

struct OpenCodeEvent {
    OpenCodeEventKind kind = OpenCodeEventKind::kStatus;
    std::string status;
    std::string text;
    std::string tool;
    std::string preview;
    // Upstream call id for an `agent.tool` frame. Empty when the gateway could
    // not learn it (a mid-run attach); blocks then fall back to merging by name.
    std::string call_id;
    // True when an `agent.error` ends the run. A retryable upstream step failure
    // is reported as an error with `fatal = false`: the run continues, so the
    // session keeps running instead of showing a failure. Absent means fatal.
    bool fatal = true;
    std::string permission_id;
    std::string question_id;
    std::string question_title;
    std::vector<OpenCodeQuestionOption> question_options;
};

using OpenCodeEventCallback = void (*)(const OpenCodeEvent& event, void* ctx);

// An outbound answer to an ask the gateway delivered over the event stream.
// Both kinds ride the same queue so a single stream drain loop can answer them
// without a second long-lived connection or a second task; `is_question`
// selects which POST the streaming worker makes.
struct OpenCodeOutboundReply {
    std::string permission_id;
    bool approve = true;
    bool is_question = false;
    std::string question_id;
    // The selected option's `value` as projected by the gateway. The device
    // never assembles the upstream `{[fieldKey]: value}` answer record.
    std::string answer;
};

// The id of whichever ask a queued reply answers, for logging and for restoring
// the pending-ask UI when the reply POST fails.
inline const std::string& OpenCodeReplyId(const OpenCodeOutboundReply& reply)
{
    return reply.is_question ? reply.question_id : reply.permission_id;
}

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
// Both stream entry points share one cancel contract: the caller owns the two
// atomics, sets `interrupt_requested` to end the stream, and reads
// `interrupt_delivered` afterwards to tell "stopped on request" from "the
// stream died". The streaming worker performs the interrupt POST itself, so no
// caller ever opens a connection of its own while a stream is attached.
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
    OpenCodeResult* result);
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
    OpenCodeResult* result);
// Backfill one session's transcript, oldest message first. The gateway has
// already truncated it to fit the device's bounded-JSON budget.
esp_err_t GetOpenCodeHistory(
    const std::string& token,
    const std::string& session_id,
    std::vector<OpenCodeHistoryMessage>* messages,
    OpenCodeResult* result);
// Answer an `agent.question` ask with the selected option's value. The gateway
// owns the mapping from that value back to the upstream form's field id.
esp_err_t PostQuestionReply(
    const std::string& token,
    const std::string& session_id,
    const std::string& question_id,
    const std::string& answer,
    OpenCodeResult* result);
// Stop a run the device already submitted. Only meaningful while a stream is
// attached; the caller also uses it to break that stream's read loop.
esp_err_t InterruptOpenCodeSession(
    const std::string& token,
    const std::string& session_id,
    OpenCodeResult* result);

// ---- Framing and payloads, split out of the stream so they can be tested ----
// The device's event vocabulary is a whitelist: an event name outside it is
// dropped, never passed through. Both return codes describe a frame the device
// will not act on, so a caller that only wants the kind can treat them alike.
esp_err_t ParseOpenCodeAgentFrame(
    const std::string& event_name,
    const std::string& data,
    OpenCodeEvent* out_event);
// Parse a `GET /agent/sessions/{id}/history` response body. Kept separate from
// GetOpenCodeHistory so the boot-time contract self-test can replay fixtures
// without a network: it enforces the same 16 KiB JSON ceiling the bounded HTTP
// reader does, because a history the device cannot hold must be refused rather
// than half-rendered.
esp_err_t ParseOpenCodeHistoryBody(
    const std::string& body,
    std::vector<OpenCodeHistoryMessage>* messages,
    OpenCodeResult* result);

}  // namespace wqn
