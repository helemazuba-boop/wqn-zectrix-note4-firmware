// Host matrix for main/agent_round_policy.h -- tier L1 of
// doc/1005-opencode-bidi-gap-plan.md ("纯函数提取 + host 端跑").
//
// Run it with scripts/run_agent_round_policy_test.sh; it needs nothing but a
// C++17 compiler, because the policy header pulls in no ESP-IDF.
//
// What is NOT duplicated here: the decisions. Which entry a write lands in,
// whether an empty segment retires an id, how much of a delta fits, and what a
// tool boundary drops are all decided in the policy header, and this file calls
// that header. What IS duplicated is the ~25 lines that turn a returned op into
// a history write (`Session::Frame` below, mirroring ApplyAgentRoundFrameLocked
// in main/opencode_session.cpp) -- that half cannot be compiled on the host
// because it needs FreeRTOS, AiHistory and g_lock. The duplication is the ORDER
// of the operations and nothing more; both copies say so in a comment. If the
// order here is ever wrong the op-flag assertions still pass and the bubbles
// come out wrong, which is why the expected bubble lists are written out
// literally below rather than derived.

#include "agent_round_policy.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Fail(const char* expr, int line)
{
    std::printf("  FAIL line %d: %s\n", line, expr);
    ++g_failures;
}

#define CHECK(expr)                \
    do {                           \
        ++g_checks;                \
        if (!(expr)) {             \
            Fail(#expr, __LINE__); \
        }                          \
    } while (0)

// The thinking prefix the renderer draws from the message kind alone; the marker
// is the caller's, matching ai_session.cpp / flash_session.cpp. Duplicated from
// AgentThinkingLabel in opencode_session.cpp for the same reason as the bridge.
std::string ThinkingLabel(const std::string& text)
{
    return text.empty() ? std::string() : std::string("\xE2\x9C\x8D ") + text;
}

// ---------------------------------------------------------------------------
// A stand-in for the Agent channel of AiHistory. It records the entry list in
// order and holds the two ids the mirror writes through, so a Replace against a
// retired id is a test failure rather than a silent out-of-bounds write.
// ---------------------------------------------------------------------------
class FakeHistory {
  public:
    struct Entry {
        std::string kind;
        std::string text;
    };

    int assistant = -1;
    int thinking = -1;

    const std::vector<Entry>& entries() const { return entries_; }

    void AppendAssistant(std::string_view text)
    {
        entries_.push_back({"assistant", std::string(text)});
        assistant = static_cast<int>(entries_.size()) - 1;
    }
    void ReplaceAssistant(std::string_view text)
    {
        CHECK(assistant >= 0);
        if (assistant >= 0) {
            entries_[static_cast<size_t>(assistant)].text = std::string(text);
        }
    }
    void AppendThinking(std::string_view text)
    {
        entries_.push_back({"thinking", std::string(text)});
        thinking = static_cast<int>(entries_.size()) - 1;
    }
    void ReplaceThinking(std::string_view text)
    {
        CHECK(thinking >= 0);
        if (thinking >= 0) {
            entries_[static_cast<size_t>(thinking)].text = std::string(text);
        }
    }
    // CloseAgentToolBlockLocked: closes the open placeholder into a result
    // block, retires both entries, and drops the open block. The firmware calls
    // it unconditionally and it early-returns when nothing is open.
    void CloseToolBlock()
    {
        if (tool_open_) {
            entries_.push_back({"tool-result", std::string()});
            tool_open_ = false;
        }
        assistant = -1;
        thinking = -1;
    }
    void AppendToolStart()
    {
        entries_.push_back({"tool-start", std::string()});
        tool_open_ = true;
    }

    // The assistant entries' text, in order -- i.e. the bubbles the user sees.
    std::string Bubbles() const
    {
        std::string out;
        for (const Entry& entry : entries_) {
            if (entry.kind != "assistant") {
                continue;
            }
            if (!out.empty()) {
                out += '|';
            }
            out += entry.text;
        }
        return out;
    }

  private:
    std::vector<Entry> entries_;
    bool tool_open_ = false;
};

// ---------------------------------------------------------------------------
// The bridge, mirroring ApplyAgentRoundFrameLocked in main/opencode_session.cpp
// statement for statement. See the file header for why this is duplicated.
// ---------------------------------------------------------------------------
class Session {
  public:
    std::string answer;
    std::string thinking;
    bool assistant_open = false;
    bool thinking_open = false;
    bool tool_open = false;
    FakeHistory history;
    wqn::AgentRoundBudgets budgets;

    Session() = default;
    explicit Session(wqn::AgentRoundBudgets config) : budgets(config) {}

    void Frame(wqn::AgentRoundFrameKind kind, std::string_view text)
    {
        const wqn::AgentRoundOps ops = wqn::ApplyAgentRoundFrame(
            answer, thinking, assistant_open, thinking_open, tool_open,
            wqn::AgentRoundFrame{kind, text}, budgets);
        if (ops.close_tool_block) {
            history.CloseToolBlock();
        }
        if (!assistant_open) {
            history.assistant = -1;
        }
        if (!thinking_open) {
            history.thinking = -1;
        }
        if (ops.append_assistant) {
            history.AppendAssistant(answer);
            // The Append hands back a valid id, so the entry is now open. The
            // policy cannot do this half: it does not own the id.
            assistant_open = true;
        } else if (ops.replace_assistant) {
            history.ReplaceAssistant(answer);
        }
        if (ops.append_thinking) {
            history.AppendThinking(ThinkingLabel(thinking));
            thinking_open = true;
        } else if (ops.replace_thinking) {
            history.ReplaceThinking(ThinkingLabel(thinking));
        }
    }

    void TextDelta(std::string_view text)
    {
        Frame(wqn::AgentRoundFrameKind::kTextDelta, text);
    }
    void Text(std::string_view text)
    {
        Frame(wqn::AgentRoundFrameKind::kText, text);
    }
    void ReasoningDelta(std::string_view text)
    {
        Frame(wqn::AgentRoundFrameKind::kReasoningDelta, text);
    }
    void Reasoning(std::string_view text)
    {
        Frame(wqn::AgentRoundFrameKind::kReasoning, text);
    }

    // The kTool branch's "different call" path: close the previous block
    // unconditionally (CloseAgentToolBlockLocked early-returns when nothing is
    // open), drop the accumulated answer text, then open a new block.
    void ToolBoundary()
    {
        history.CloseToolBlock();
        bool assistant_flag = assistant_open;
        bool thinking_flag = thinking_open;
        wqn::AgentRoundToolBoundary(answer, assistant_flag, thinking_flag);
        CHECK(!assistant_flag);
        CHECK(!thinking_flag);
        CHECK(answer.empty());
        assistant_open = false;
        thinking_open = false;
        history.assistant = -1;
        history.thinking = -1;
        history.AppendToolStart();
        tool_open = true;
    }
};

wqn::AgentRoundBudgets BudgetsFor(uint8_t detail)
{
    return wqn::AgentRoundBudgets{detail, wqn::kMaxAgentTextBytes, wqn::kMaxThinkingBytes};
}

// The cloud's FIXED frame shape for three parts of one reply, each part a
// separate model round: a delta stream, then the whole-segment frame that starts
// the next round over.
void ThreePartRounds(Session& session)
{
    session.TextDelta("PART-ONE.");
    session.Text("");
    session.TextDelta("PART-TWO.");
    session.Text("");
    session.TextDelta("PART-THREE.");
}

// ---------------------------------------------------------------------------
// 1. H1 -- the clear and the retirement are one decision.
//
// The failure this guards is asymmetric and invisible without it: retire the id
// and keep the buffer, and the next delta AppendAssistant's a duplicate of the
// round that was just rendered (N copies of a growing answer). Clear the buffer
// and keep the id, and the next round's text is glued behind the previous one's
// (symptom 4). Only the pair is correct.
// ---------------------------------------------------------------------------
void TestEmptyFrameRetiresAndClearsTogether()
{
    for (uint8_t detail = 0; detail <= 2; ++detail) {
        Session session(BudgetsFor(detail));
        session.TextDelta("PART-ONE.");
        session.Text("");
        CHECK(session.answer.empty());
        if (detail >= 1) {
            CHECK(!session.assistant_open);
            CHECK(session.history.Bubbles() == "PART-ONE.");
        } else {
            // Brief tier never retires on the empty frame, and the entry it
            // leaves open is what collapses the answer into one bubble.
            CHECK(session.assistant_open);
            CHECK(session.history.Bubbles() == "PART-ONE.");
        }
    }

    // The reasoning twin, and the one place the two channels deliberately
    // differ. The answer channel gates its retire on the run's tier; the
    // thinking channel does not, because reasoning only reaches the device at
    // detail >= 2 at all -- the cloud is what filters it -- so there is no tier
    // at which that frame arrives unexpectedly. A gate added here would be dead
    // code that looks load-bearing, so it is asserted absent at every tier.
    for (uint8_t detail = 0; detail <= 2; ++detail) {
        Session reasoning(BudgetsFor(detail));
        reasoning.ReasoningDelta("THINK-A");
        CHECK(reasoning.thinking_open);
        reasoning.Reasoning("");
        CHECK(reasoning.thinking.empty());
        CHECK(!reasoning.thinking_open);
        CHECK(reasoning.history.entries().size() == 1);
        CHECK(reasoning.history.entries()[0].kind == "thinking");
        CHECK(reasoning.history.entries()[0].text == ThinkingLabel("THINK-A"));
        // ...and the next delta opens a new entry at the current tail rather
        // than rewriting the previous round's thinking in place.
        reasoning.ReasoningDelta("THINK-B");
        CHECK(reasoning.history.entries().size() == 2);
        CHECK(reasoning.history.entries()[1].text == ThinkingLabel("THINK-B"));
    }
}

// ---------------------------------------------------------------------------
// 2. H2 -- the gate reads the run's tier, and nothing else can reach it.
// ---------------------------------------------------------------------------
void TestGateReadsTheRunTier()
{
    // Structural: AgentRoundBudgets carries exactly one tier field and it is
    // named for the run, so a live setting has no way into the policy. The
    // firmware is the only place that can get this wrong (it must pass
    // g_run_detail, never g_state.detail_level), and ApplyAgentRoundFrameLocked
    // names the field it builds.
    wqn::AgentRoundBudgets run_tier;
    run_tier.run_detail = 1;
    CHECK(run_tier.run_detail == 1);

    Session session(BudgetsFor(0));
    session.TextDelta("PART-ONE.");
    session.Text("");
    // Brief tier: the frame still clears the buffer, but the entry stays open,
    // so the next round replaces it rather than opening a second bubble.
    CHECK(session.answer.empty());
    CHECK(session.assistant_open);
    session.TextDelta("PART-TWO.");
    CHECK(session.history.Bubbles() == "PART-TWO.");
}

// ---------------------------------------------------------------------------
// 3. The matrix: {no tool, tool-separated, mid-run attach} x {detail 0, 1, 2}.
// ---------------------------------------------------------------------------
void TestRoundShapeMatrix()
{
    for (uint8_t detail = 0; detail <= 2; ++detail) {
        // (a) No tool: three model rounds in one message.
        {
            Session session(BudgetsFor(detail));
            ThreePartRounds(session);
            if (detail >= 1) {
                // One bubble per round. This is the shape that makes a round
                // boundary visible instead of a splice.
                CHECK(session.history.Bubbles() == "PART-ONE.|PART-TWO.|PART-THREE.");
            } else {
                // Brief tier is byte-for-byte the pre-fix behaviour: the last
                // round wins the single bubble.
                CHECK(session.history.Bubbles() == "PART-THREE.");
            }
        }

        // (b) Tool inside a round: "text, then a tool, then more text" is ONE
        // round to the cloud (its round key is the assistant message id alone),
        // so no empty frame arrives between the two text parts. Only the tool
        // boundary can separate them, and dropping it reproduces symptom 4
        // exactly: the second part is glued behind the first's.
        {
            Session session(BudgetsFor(detail));
            session.TextDelta("11111");
            session.ToolBoundary();
            session.TextDelta("22222");
            session.ToolBoundary();
            session.TextDelta("3333");
            CHECK(session.history.Bubbles() == "11111|22222|3333");

            // The same frames WITHOUT the boundary clear -- the pre-C5 shape,
            // which is the splice the plan was opened for.
            Session spliced(BudgetsFor(detail));
            spliced.TextDelta("11111");
            spliced.TextDelta("22222");
            spliced.TextDelta("3333");
            CHECK(spliced.history.Bubbles() == "11111222223333");
        }

        // (c) Mid-run attach: the stream joins a run already in progress, so
        // nothing is open yet and the first delta has to Append. A policy that
        // assumed an entry here would ReplaceText against a retired id.
        {
            Session session(BudgetsFor(detail));
            CHECK(!session.assistant_open);
            CHECK(!session.thinking_open);
            session.TextDelta("JOINED-MID-RUN");
            CHECK(session.history.Bubbles() == "JOINED-MID-RUN");
            CHECK(session.assistant_open);
            session.TextDelta("-MORE");
            CHECK(session.history.Bubbles() == "JOINED-MID-RUN-MORE");
        }
    }
}

// ---------------------------------------------------------------------------
// 4. The bounded append, and why the mirror must stay in place.
//
// The gateway emits dozens of deltas per reply. Appending a history entry per
// delta would evict the ring buffer's head -- the rest of the conversation --
// before the reply finished, so a long reply is one entry replaced in place.
// ---------------------------------------------------------------------------
void TestBoundedAppendMirrorsInPlace()
{
    Session session(BudgetsFor(1));
    for (int i = 0; i < 900; ++i) {
        session.TextDelta("0123456789abcdef");
    }
    CHECK(session.answer.size() == wqn::kMaxAgentTextBytes);
    CHECK(session.answer.size() <= wqn::kMaxAgentTextBytes);
    // One entry for the whole reply, replaced 900 times rather than appended.
    CHECK(session.history.entries().size() == 1);
    CHECK(session.history.entries()[0].text.size() == wqn::kMaxAgentTextBytes);

    // A delta that does not fit is truncated to a whole number of characters,
    // never sliced through one. Drive the budget with 3-byte characters so the
    // cut lands inside a sequence.
    const size_t answer_budget = 10;
    Session utf8(wqn::AgentRoundBudgets{1, answer_budget, wqn::kMaxThinkingBytes});
    // 3 x 3 bytes = 9, then 3 more would make 12 > 10, so the second character
    // is dropped whole and the buffer stops at 9.
    utf8.TextDelta("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD");
    CHECK(utf8.answer.size() == 9);
    CHECK(utf8.answer == "\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD");
    CHECK(utf8.history.entries().size() == 1);

    // The one asymmetry between the two channels, preserved on purpose: the
    // answer channel mirrors even when a delta did not fit (the mirror is also
    // what closes an open tool block), while the thinking channel skips the
    // mirror entirely once its own budget is spent. A chain-of-thought that
    // overran its budget therefore stops advancing both buffers and history,
    // instead of closing a tool block on a frame that carried no text.
    const size_t thinking_budget = 8;
    Session full(wqn::AgentRoundBudgets{2, wqn::kMaxAgentTextBytes, thinking_budget});
    full.ReasoningDelta("0123456789");
    CHECK(full.thinking.size() == thinking_budget);
    CHECK(full.history.entries().size() == 1);
    full.ToolBoundary();
    CHECK(full.tool_open);
    full.ReasoningDelta("MORE");
    CHECK(full.thinking.size() == thinking_budget);
    // The tool block is still open: the thinking channel did not mirror, so it
    // did not close it either.
    CHECK(full.tool_open);
    CHECK(full.history.entries().size() == 2);

    // The answer channel on the same shape DOES mirror, and does close it.
    Session answer_side(wqn::AgentRoundBudgets{2, thinking_budget, wqn::kMaxThinkingBytes});
    answer_side.TextDelta("0123456789");
    CHECK(answer_side.answer.size() == thinking_budget);
    answer_side.ToolBoundary();
    CHECK(answer_side.tool_open);
    answer_side.TextDelta("MORE");
    CHECK(!answer_side.tool_open);
    CHECK(answer_side.answer == "MORE");
    CHECK(answer_side.history.entries().size() == 4);
    CHECK(answer_side.history.Bubbles() == "01234567|MORE");
}

// ---------------------------------------------------------------------------
// 5. Ordering and exclusivity, asserted on the returned ops rather than on the
//    bubbles, because these are the properties the bubbles cannot show.
// ---------------------------------------------------------------------------
void TestOpOrderAndExclusivity()
{
    Session session(BudgetsFor(1));
    session.TextDelta("BEFORE");
    session.ToolBoundary();
    CHECK(session.tool_open);

    // With a tool block open, a text frame must close it first -- that close is
    // what retires both entries, so a policy that skipped it would ReplaceText
    // into the entry that sits above the tool block.
    wqn::AgentRoundOps ops = wqn::ApplyAgentRoundFrame(
        session.answer, session.thinking, session.assistant_open, session.thinking_open,
        session.tool_open, wqn::AgentRoundFrame{wqn::AgentRoundFrameKind::kTextDelta, "AFTER"},
        BudgetsFor(1));
    CHECK(ops.close_tool_block);
    CHECK(!session.tool_open);
    CHECK(!session.assistant_open);
    CHECK(ops.append_assistant);
    CHECK(!ops.replace_assistant);
    CHECK(!ops.append_thinking);
    CHECK(!ops.replace_thinking);

    // Exclusivity and the "a close leaves only appends behind it" rule, over
    // every frame kind, tier and text shape.
    const wqn::AgentRoundFrameKind kinds[] = {
        wqn::AgentRoundFrameKind::kTextDelta,
        wqn::AgentRoundFrameKind::kText,
        wqn::AgentRoundFrameKind::kReasoningDelta,
        wqn::AgentRoundFrameKind::kReasoning,
    };
    const char* texts[] = {"", "x", "\xE4\xB8\xAD"};
    for (uint8_t detail = 0; detail <= 2; ++detail) {
        for (wqn::AgentRoundFrameKind kind : kinds) {
            for (const char* text : texts) {
                Session probe(BudgetsFor(detail));
                probe.assistant_open = true;
                probe.thinking_open = true;
                probe.tool_open = (text[0] == 'x');
                const wqn::AgentRoundOps one = wqn::ApplyAgentRoundFrame(
                    probe.answer, probe.thinking, probe.assistant_open, probe.thinking_open,
                    probe.tool_open, wqn::AgentRoundFrame{kind, text}, BudgetsFor(detail));
                CHECK(!(one.append_assistant && one.replace_assistant));
                CHECK(!(one.append_thinking && one.replace_thinking));
                CHECK(!(one.append_assistant && one.append_thinking));
                CHECK(!(one.replace_assistant && one.replace_thinking));
                if (one.close_tool_block) {
                    CHECK(!one.replace_assistant);
                    CHECK(!one.replace_thinking);
                }
            }
        }
    }
}

}  // namespace

int main()
{
    std::printf("agent_round_policy host matrix\n");
    TestEmptyFrameRetiresAndClearsTogether();
    TestGateReadsTheRunTier();
    TestRoundShapeMatrix();
    TestBoundedAppendMirrorsInPlace();
    TestOpOrderAndExclusivity();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
