// The Agent answer round is decided by a handful of rules that are
// load-bearing and invisible on a device:
//
//   * an empty `agent.text` / `agent.reasoning` is the cloud's "this round
//     starts over" frame, and it must retire the history entry AND clear the
//     buffer *together* (H1 below). Retiring alone re-renders the previous
//     round as a duplicate bubble; clearing alone glues the next round's text
//     behind the last one's;
//   * the gate on that frame reads the detail tier the RUN was started with,
//     never the live setting (H2 below), or a tier change mid-run would change
//     how a run already in flight finishes rendering;
//   * every delta appends into a bounded buffer and mirrors in place, so a long
//     reply cannot evict the conversation out of the history ring;
//   * a tool boundary in the middle of a round drops the accumulated text;
//   * a stream that attaches mid-run starts with no open entries, so its first
//     delta must Append rather than Replace.
//
// Those rules used to live inside the event switch in opencode_session.cpp with
// the buffer, the history ids and the history handle all reachable from the
// middle of them, which made them testable only on hardware. They are a pure
// function here over plain state; opencode_session.cpp applies the returned
// operations to the real AiHistory. The host matrix in
// test/agent_round_policy_test.cpp (run it with
// scripts/run_agent_round_policy_test.sh) exercises THIS file, not a copy of it
// -- that is the point, so keep the two in step and add no second decision.

#ifndef WQN_AGENT_ROUND_POLICY_H_
#define WQN_AGENT_ROUND_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wqn {

// The answer budget. Its own, rather than shared with reasoning: a long
// chain-of-thought must not starve the reply the user actually asked for, and
// the history channel is evicted oldest-first, so an unbounded buffer would
// push the whole conversation out of the ring before the reply finished.
inline constexpr size_t kMaxAgentTextBytes = 12 * 1024;
inline constexpr size_t kMaxThinkingBytes = 2 * 1024;

// The frames of an agent stream that decide the answer and thinking channels.
// Status text, permissions and questions are handled by the caller and never
// reach here.
enum class AgentRoundFrameKind : uint8_t {
    // A fragment to append to the channel's buffer.
    kTextDelta = 0,
    // A whole segment, possibly empty. Empty is the round-boundary frame.
    kText,
    kReasoningDelta,
    kReasoning,
};

// The fields of one frame the policy reads.
struct AgentRoundFrame {
    AgentRoundFrameKind kind = AgentRoundFrameKind::kTextDelta;
    std::string_view text;
};

// The tier and the budgets this RUN was started with. `run_detail` is the tier
// the run was armed with, not the live setting (H2).
struct AgentRoundBudgets {
    uint8_t run_detail = 0;
    size_t answer_bytes = kMaxAgentTextBytes;
    size_t thinking_bytes = kMaxThinkingBytes;
};

// What the caller must do to history, in the order the flags are listed. At
// most one of {append, replace} is set per channel, because that choice -- which
// entry a write lands in -- is exactly what this policy exists to decide, and
// having it decided in two places is how the splice bugs got in.
struct AgentRoundOps {
    // Close the open tool block into a result block first. This is set before
    // any history write, and it retires both entries (see
    // CloseAgentToolBlockLocked), which is why the append/replace choice below
    // is made as if it had already run.
    bool close_tool_block = false;
    bool append_assistant = false;
    bool replace_assistant = false;
    bool append_thinking = false;
    bool replace_thinking = false;
};

// Applies one frame to the buffers and to the "is an entry open" flags, and
// returns the history operations the caller must perform in order.
//
// The three flags are the only half of a history id the policy needs. It can
// close an entry and never open one -- the Append operations hand the new id
// back to the caller -- so a flag that went false maps to "store the invalid
// id" and writing that over an already-invalid id is a no-op. That is the whole
// bridge; keep it that small on purpose.
//
// `tool_block_open` is in the signature because the mirror path closes an open
// tool block before it writes, and that close is what retires the two entries.
// Handing the policy the pre-close flag would let it dispatch a ReplaceText
// against an id the caller is about to invalidate.
AgentRoundOps ApplyAgentRoundFrame(std::string& answer,
                                   std::string& thinking,
                                   bool& assistant_entry_open,
                                   bool& thinking_entry_open,
                                   bool& tool_block_open,
                                   const AgentRoundFrame& frame,
                                   const AgentRoundBudgets& budgets);

// A tool boundary: a new tool block is about to open. Drops the answer text
// accumulated so far in this round and retires both entries, so the text that
// follows the tool opens a new assistant entry instead of appending to the one
// above the tool block.
//
// This is the one place a clear can land in the middle of a round, and it is
// safe because every pre-tool delta has already been mirrored into history by
// then, and the close that precedes it retired the entry. Clearing inside
// CloseAgentToolBlockLocked instead would be tidier and wrong: the mirror
// functions call that close *after* writing the current frame's text into the
// buffer, so clearing there erases the very frame that triggered the close --
// and because the gateway replays a whole segment when it believes a delta was
// lost, what gets eaten can be an entire part rather than one delta. A tool
// frame carries no text of its own.
void AgentRoundToolBoundary(std::string& answer,
                            bool& assistant_entry_open,
                            bool& thinking_entry_open);

// The longest prefix of `text` that fits `max_bytes` without ending inside a
// UTF-8 sequence. Truncating a Chinese answer at a byte count with substr()
// splits a 3-byte character into stray bytes, and the history channel then
// renders mojibake (or drops the tail of the message). Backing up from the cut
// to the lead byte and keeping the whole character when it fits keeps the
// buffer valid UTF-8 at every prefix length.
inline size_t Utf8SafePrefixBytes(std::string_view text, size_t max_bytes)
{
    if (text.size() <= max_bytes) {
        return text.size();
    }
    size_t start = max_bytes;
    while (start > 0 &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        --start;
    }
    const unsigned char lead = static_cast<unsigned char>(text[start]);
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
    }
    return start + length <= max_bytes ? start + length : start;
}

inline AgentRoundOps ApplyAgentRoundFrame(std::string& answer,
                                          std::string& thinking,
                                          bool& assistant_entry_open,
                                          bool& thinking_entry_open,
                                          bool& tool_block_open,
                                          const AgentRoundFrame& frame,
                                          const AgentRoundBudgets& budgets)
{
    AgentRoundOps ops;
    const bool text_channel =
        frame.kind == AgentRoundFrameKind::kTextDelta ||
        frame.kind == AgentRoundFrameKind::kText;
    std::string& buffer = text_channel ? answer : thinking;
    const size_t budget = text_channel ? budgets.answer_bytes : budgets.thinking_bytes;

    if (frame.kind == AgentRoundFrameKind::kTextDelta ||
        frame.kind == AgentRoundFrameKind::kReasoningDelta) {
        if (buffer.size() < budget) {
            const size_t remaining = budget - buffer.size();
            buffer.append(frame.text.data(),
                          Utf8SafePrefixBytes(frame.text, remaining));
        } else if (frame.kind == AgentRoundFrameKind::kReasoningDelta) {
            // The answer channel mirrors unconditionally even when the delta did
            // not fit, because the mirror is also what closes an open tool
            // block. The thinking channel has never done that: a thinking buffer
            // that is already full skips the mirror entirely. The asymmetry is
            // deliberate and is what the two branches below reproduce.
            return ops;
        }
    } else {
        // A whole segment replaces the buffer, and an empty one clears it. This
        // assign IS the clear half of H1 -- the other half is the retirement
        // below, and the two must not be separated.
        buffer.assign(frame.text.data(),
                      Utf8SafePrefixBytes(frame.text, budget));
    }

    // H1/H2. The empty segment is the cloud's "start this round over" frame.
    // Retiring the entry is what makes the next delta open a new one instead of
    // writing into the entry that still holds the previous round's answer.
    //
    // Gated on the run's tier because that is what decides whether the cloud
    // emits the frame at all: brief has always sent it, and the tiers that
    // render reasoning and tool blocks got it later. Reading a live setting
    // here would let a tier change mid-run change how the run in flight
    // finishes rendering.
    //
    // No gate on the reasoning twin: reasoning only reaches the device at
    // detail >= 2 at all, because the cloud is what filters it, so there is no
    // tier at which that frame arrives unexpectedly.
    if (frame.text.empty()) {
        if (text_channel) {
            if (budgets.run_detail >= 1) {
                assistant_entry_open = false;
            }
        } else {
            thinking_entry_open = false;
        }
    }

    // The mirror closes an open tool block first, and that close retires both
    // entries -- so the append/replace choice below is made as if it had
    // already run.
    if (tool_block_open) {
        ops.close_tool_block = true;
        tool_block_open = false;
        assistant_entry_open = false;
        thinking_entry_open = false;
    }

    if (text_channel) {
        if (buffer.empty()) {
            return ops;
        }
        if (assistant_entry_open) {
            ops.replace_assistant = true;
        } else {
            ops.append_assistant = true;
        }
    } else {
        if (buffer.empty()) {
            return ops;
        }
        if (thinking_entry_open) {
            ops.replace_thinking = true;
        } else {
            ops.append_thinking = true;
        }
    }
    return ops;
}

inline void AgentRoundToolBoundary(std::string& answer,
                                   bool& assistant_entry_open,
                                   bool& thinking_entry_open)
{
    answer.clear();
    assistant_entry_open = false;
    thinking_entry_open = false;
}

}  // namespace wqn

#endif  // WQN_AGENT_ROUND_POLICY_H_
