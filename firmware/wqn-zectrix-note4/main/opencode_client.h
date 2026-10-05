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

/**
 * Whether a session has a run in flight, and how its last run ended.
 * `contracts/agent-gateway-v0` (2.2) `$defs/session.outcome`.
 *
 * The gateway resolves this from two reads; the device receives the answer
 * and does not re-derive it. The only rule the device has to get right is
 * about the two "not running" spellings: `kUnknown` and a field the relay
 * never sent both mean NO run in flight. Reading either as running would
 * hold the sleep lease on a session that is merely fresh, and the device
 * would never sleep again -- which is why the mapping is one-way here and
 * the fallback is the safe direction.
 */
enum class OpenCodeSessionOutcome : uint8_t {
    // No run in flight and no settle recorded: never prompted, an upstream
    // vocabulary this contract predates, or a relay that predates the field.
    kUnknown = 0,
    kRunning,
    kSucceeded,
    kInterrupted,
    kFailed,
};

struct OpenCodeSessionInfo {
    std::string id;
    std::string title;
    int64_t updated_at = 0;
    // Absent from a relay predating the field; see OpenCodeSessionOutcome.
    OpenCodeSessionOutcome outcome = OpenCodeSessionOutcome::kUnknown;
};

// One answerable option of an `agent.question` ask. The gateway projects one
// field of the upstream form per ask -- at most kMaxOpenCodeQuestionOptions
// options -- and the device only ever sends the chosen `value` back. An ask
// with no options is the abort-only projection of a field the device cannot
// answer; it still reaches the UI so the user can escape via 自定义回答.
struct OpenCodeQuestionOption {
    std::string value;
    std::string label;
};

// The contract's `questionData.options` maxItems. A frame with more is a
// gateway contract violation and is refused at parse time rather than
// half-rendered: the option bar walks the list two slots at a time and a
// truncated list would silently hide answers.
inline constexpr int kMaxOpenCodeQuestionOptions = 8;

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
    // The session an `agent.permission` / `agent.question` ask belongs to. It is
    // not necessarily the session the device attached to: a subagent has its own
    // id and raises its asks against it, and the reply routes are session-scoped
    // upstream, so answering on the attached session is a 404. Empty means "the
    // attached session", which is what a relay that omits the field still wants.
    std::string session_id;
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
    // The session that raised the ask this reply answers, carried from the
    // `agent.permission` / `agent.question` frame that delivered it. The stream
    // request only knows the attached session, so without this a subagent's ask
    // would be POSTed against a session that never raised it.
    std::string session_id;
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

// The session a reply is POSTed against. The ask's own session wins, because both
// reply routes are scoped to it and a subagent raises its asks against its own
// id -- answering on the attached session is a 404 against the ownership check,
// which made a subagent's ask discoverable but never answerable. A value that is
// not a session id (a relay that omitted the field, or a frame the contract
// already refuses) falls back to the attached session: that is what the device
// did before the field existed, and it keeps an ask from being silently
// unroutable when the fallback is in fact the right session.
inline const std::string& OpenCodeReplySessionId(
    const OpenCodeOutboundReply& reply,
    const std::string& attached_session_id)
{
    return reply.session_id.rfind("ses_", 0) == 0 ? reply.session_id
                                                  : attached_session_id;
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
//
// They also share a *switch* contract, which is the same shape with the
// opposite intent: `switch_requested` detaches the device from the stream
// without asking upstream to stop anything, so the run keeps executing in the
// cloud while only the local view moves on. `switch_delivered` then reads back
// as ESP_OK rather than `stream_incomplete`.
//
// Both can be armed at once, and the INTERRUPT is the one checked first. That
// ordering is the point: an explicit cancel must never be swallowed by a
// pending switch, or the run the user asked to stop keeps going. A switch left
// armed under a cancel is still honoured by the caller's tail, so the user gets
// the picker they asked for as well as the stop they asked for.
//
// `detail` is the requested cloud detail tier (kOpenCodeDetailDefault in
// opencode_model.h). It rides the request as `?detail=N` and only shapes what
// the gateway projects onto the stream -- never the run itself.
esp_err_t RunOpenCodePrompt(
    const std::string& token,
    const std::string& session_id,
    uint8_t detail,
    const std::string& prompt,
    // Run idempotency key: 16 hex chars. Empty omits the field, which is the
    // pre-P2 body the cloud treats as "no idempotency".
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
    OpenCodeResult* result);
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
    OpenCodeResult* result);
// Backfill one session's transcript, oldest message first. The gateway has
// already truncated it to fit the device's bounded-JSON budget, and projects it
// at `detail` (kOpenCodeDetailDefault).
esp_err_t GetOpenCodeHistory(
    const std::string& token,
    const std::string& session_id,
    uint8_t detail,
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
