// Agent tier on the AI page: the OpenCode gateway rendered through the shared
// chat viewport, plus the two interaction surfaces the gateway needs that the
// STD/Pro tiers do not have --
//
//   1. a session picker (the gateway's conversations are server-side and
//      long-lived, so the user must be able to switch and to re-attach), and
//   2. a two-choice option bar (发送/重新输入 for the voice transcript,
//      同意/拒绝 for a permission ask) driven by ↑/↓ + confirm.
//
// Both are drawn with a dashed outline: they are uncommitted. The transcript
// and the permission ask only enter history once the user acts on them, so
// they must not read as settled chat entries.
//
// The status bar keeps the same edit-mode slot numbering STD/Pro uses (0 =
// tier, 1..5 = cluster), so the input path's index arithmetic stays tier-
// independent -- the cluster LENGTH and the trash index are the two
// tier-dependent values (five slots here, four on STD/Pro). Slot 4 is this
// tier's 详细程度 control (the ?detail=N tier the gateway projects at); slot 5
// (clear) is shared verbatim with STD/Pro.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ai_feature.h"
#include "ai_history.h"
#include "display_service.h"
#include "esp_log.h"
#include "opencode_session.h"
#include "ui/assets/font_wqn_ui_16_1.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

// [agent] Option bar geometry. It lives inside the same 22 px bottom band the
// scroll indicator uses (kAiViewportBottomPad), so adding it cost zero
// viewport height. While it is visible it owns the band outright -- the ▼
// chevron is suppressed for that frame because the pending state, not the
// scroll position, is what the user is acting on.
constexpr int kAgentBarY = wqn::kEpdHeight - kAiViewportBottomPad;  // 278
constexpr int kAgentBarH = kAiViewportBottomPad;                    // 22
constexpr int kAgentBarTextY = kAgentBarY + 3;                      // baseline
constexpr int kAgentBarMarkerX = 8;
constexpr int kAgentBarLabelX = kAgentBarMarkerX + 16;
constexpr int kAgentBarSlotStep = 96;
// Gap between the pending bubble's bottom edge and the option bar.
constexpr int kAgentBubbleGap = 6;
constexpr int kAgentBubbleMaxLines = 3;

// Session picker geometry (relocated from the retired standalone page).
constexpr int kAgentPickerTitleY = 34;
constexpr int kAgentPickerListY = 60;
constexpr int kAgentPickerRowStep = 28;
constexpr int kAgentPickerMaxRows = 7;

#define AGENT_TEXT(...) do { (void)wqn::DrawUtf8Text(__VA_ARGS__); } while (0)

std::string AgentOneLine(const std::string& text, int width)
{
    const std::vector<std::string> lines = wqn::WrapUtf8TextToWidth(text, width, 1);
    return lines.empty() ? std::string() : lines.front();
}

// ---------------------------------------------------------------------------
// Option bar
// ---------------------------------------------------------------------------

AgentOptionMode AgentOptionModeFor(const wqn::AgentSessionState& agent)
{
    // An ask first: either kind can only arrive while a run is live, and a run
    // blocked on a decision is the more urgent of the two states. A malformed
    // ask (no id, or a question the gateway projected no options for) is not
    // answerable, so it falls through instead of drawing an unusable bar.
    if (agent.ui.phase == wqn::AiFeaturePhase::kAwaitingPermission &&
        !agent.pending_permission_id.empty()) {
        return AgentOptionMode::kPermission;
    }
    if (agent.ui.phase == wqn::AiFeaturePhase::kAwaitingQuestion &&
        !agent.pending_question_id.empty() && !agent.pending_question_options.empty()) {
        return AgentOptionMode::kQuestion;
    }
    if (agent.ui.requires_confirmation) {
        return AgentOptionMode::kConfirmSend;
    }
    return AgentOptionMode::kNone;
}

// Two is the bar's hard ceiling, not the gateway's: the gateway already caps a
// projected form at two options for exactly this reason.
int AgentQuestionSlotCount(const wqn::AgentSessionState& agent)
{
    return static_cast<int>(
        std::min<size_t>(agent.pending_question_options.size(), 2));
}

int AgentQuestionFocusedSlot(const wqn::AgentSessionState& agent, uint8_t focused)
{
    const int count = AgentQuestionSlotCount(agent);
    if (count <= 0) {
        return 0;
    }
    return std::min<int>(focused, count - 1);
}

const char* AgentOptionLabel(AgentOption option)
{
    switch (option) {
        case AgentOption::kSend: return "发送";
        case AgentOption::kReinput: return "重新输入";
        case AgentOption::kApprove: return "同意";
        case AgentOption::kDeny: return "拒绝";
        case AgentOption::kCount:
        default: return "";
    }
}

// The focused slot, clamped to the two the current mode actually offers so a
// focus left over from the other mode cannot index past the end.
AgentOption AgentFocusedOption(AgentOptionMode mode, uint8_t focused)
{
    const AgentOption first = (mode == AgentOptionMode::kPermission)
        ? AgentOption::kApprove
        : AgentOption::kSend;
    const AgentOption second = (mode == AgentOptionMode::kPermission)
        ? AgentOption::kDeny
        : AgentOption::kReinput;
    return (focused == 0) ? first : second;
}

static void DrawAgentOptionBar(AgentOptionMode mode, uint8_t focused,
                                const wqn::AgentSessionState& agent)
{
    // The band is cleared here rather than by the caller so a disappearing
    // option bar cannot leave a ghost on the E-ink panel.
    FillRect(0, kAgentBarY, wqn::kEpdWidth, kAgentBarH, false);

    // A rule above the bar separates it from the transcript above it.
    DrawHorizontalLine(0, kAgentBarY, wqn::kEpdWidth);

    const int slot_count = (mode == AgentOptionMode::kQuestion)
        ? AgentQuestionSlotCount(agent)
        : 2;
    const int focused_slot = (mode == AgentOptionMode::kQuestion)
        ? AgentQuestionFocusedSlot(agent, focused)
        : (AgentFocusedOption(mode, focused) == AgentFocusedOption(mode, 1) ? 1 : 0);

    for (int i = 0; i < slot_count; ++i) {
        const int x = kAgentBarLabelX + i * kAgentBarSlotStep;
        if (i == focused_slot) {
            // ▣ marker: a filled square says "this slot is armed" without
            // borrowing a directional chevron that would imply something else.
            FillRect(kAgentBarMarkerX + i * kAgentBarSlotStep, kAgentBarTextY + 3,
                     8, 8, true);
        }
        std::string label;
        if (mode == AgentOptionMode::kQuestion) {
            // The gateway projects {value,label}; an unlabelled option still
            // shows its value rather than an empty slot.
            label = agent.pending_question_options[i].label.empty()
                ? agent.pending_question_options[i].value
                : agent.pending_question_options[i].label;
        } else {
            label = AgentOptionLabel(
                AgentFocusedOption(mode, static_cast<uint8_t>(i)));
        }
        AGENT_TEXT(x, kAgentBarTextY, AgentOneLine(label, 88).c_str(), true);
    }

    // Key legend, right-aligned, leaving the far-right 40 px clear.
    const char* hint = "↑↓切换 确认执行";
    const int hint_w = wqn::MeasureUtf8TextWidth(hint);
    AGENT_TEXT(wqn::kEpdWidth - hint_w - 8, kAgentBarTextY, hint, true);
}

// ---------------------------------------------------------------------------
// Pending bubble
// ---------------------------------------------------------------------------

// Dashed overlay card anchored to the bottom of the viewport, just above the
// option bar. It deliberately overlays history rather than scrolling it: the
// user is mid-decision and the newest committed turn is not what they are
// reading.
static void DrawAgentPendingBubble(const std::string& text, const char* title)
{
    if (text.empty()) {
        return;
    }
    const auto lines = wqn::WrapUtf8TextToWidth(
        text, kAiAssistantW - 2 * kAiAssistantLeftBorder - 8, kAgentBubbleMaxLines);
    if (lines.empty()) {
        return;
    }

    const int body_h = static_cast<int>(lines.size()) * kAiLineH;
    const int height = body_h + 8 + 4;  // 4px pad top/bottom + 8px title row
    const int top = kAgentBarY - kAgentBubbleGap - height;
    const int x = kAiHistoryLeftPad;
    const int width = kAiAssistantW;

    // White fill first: this is an overlay, and any history underneath it must
    // not show through the gaps in the dashed outline.
    FillRect(x + 1, top + 1, width - 2, height - 2, false);
    DrawDashedRect(x, top, width, height);

    AGENT_TEXT(x + kAiAssistantLeftBorder, top + 4, title, true);
    int y = top + 4 + 8;
    for (const std::string& line : lines) {
        AGENT_TEXT(x + kAiAssistantLeftBorder, y, line.c_str(), true);
        y += kAiLineH;
    }
}

// ---------------------------------------------------------------------------
// Session picker
// ---------------------------------------------------------------------------

static void DrawAgentSessionPicker(const wqn::AgentSessionState& agent)
{
    AGENT_TEXT(12, kAgentPickerTitleY, "选择 Session · 确认锁定 · 长按新建", true);
    DrawHorizontalLine(8, 52, 384);
    if (agent.sessions.empty()) {
        const std::string empty = agent.ui.activity_text.empty()
            ? "暂无 Session"
            : agent.ui.activity_text;
        AGENT_TEXT(12, 82, AgentOneLine(empty, 360).c_str(), true);
        return;
    }
    const size_t selected = std::min(agent.selected_session, agent.sessions.size() - 1);
    // Window the list so the focused row stays visible without scrolling the
    // whole set; the retired standalone page used the same 4-row look-behind.
    const size_t start = selected > 3 ? selected - 3 : 0;
    const size_t end = std::min(agent.sessions.size(),
                                start + static_cast<size_t>(kAgentPickerMaxRows));
    int y = kAgentPickerListY;
    for (size_t index = start; index < end; ++index) {
        const bool focused = index == selected;
        if (focused) {
            FillRoundedRect(8, y - 3, 384, 25, 5);
        }
        const std::string title = AgentOneLine(
            agent.sessions[index].title.empty() ? agent.sessions[index].id
                                                : agent.sessions[index].title,
            360);
        AGENT_TEXT(16, y, title.c_str(), !focused);
        y += kAgentPickerRowStep;
    }
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

// [detail] Glyph for a detail tier: 0 简要 / 1 标准 / 2 详细. Unknown values fall
// back to 详细 -- the same fallback the request path applies to ?detail=N.
static const WqnBitmapAsset& AgentDetailAsset(uint8_t level)
{
    switch (level) {
        case 0: return a18_ai_detail_brief_16_asset;
        case 1: return a19_ai_detail_standard_16_asset;
        case 2:
        default: return a20_ai_detail_full_16_asset;
    }
}

// [agent] Agent-tier status bar. Same edit-mode slot numbering as STD/Pro
// (0 = tier, 1 = session, 2 = turn up, 3 = turn down, 4 = 详细程度, 5 = clear),
// so ApplyStatusBarEditEvent's index arithmetic does not need a tier branch --
// only the trash index and the cluster length differ. The thinking/TTS/expand
// slots STD/Pro fills are repurposed because they configure the STD/Pro text
// turn and mean nothing to the gateway; slot 4 carries the detail tier every
// Agent request is projected at.
static void DrawAgentStatusBar(const wqn::AgentSessionState& agent,
                               const wqn::HomeSummary& home,
                               const wqn::StatusBarEditState& status_edit)
{
    DrawHorizontalLine(0, kAiStatusBarH - 1, wqn::kEpdWidth);

    auto selected = [&status_edit](int index) {
        return status_edit.active && status_edit.selected == index;
    };

    // Slot 0: tier indicator (brain -- the retired Pro glyph, reused).
    DrawSelectableIcon(6, kAiToggleY, a03_ai_tier_pro_16_asset, selected(0), 0);

    if (status_edit.active) {
        DrawRect(kAiToggleX - kAiToggleZonePad, kAiToggleY - kAiToggleZonePad,
                 kAiToggleZoneWAgent, 16 + 2 * kAiToggleZonePad);
    }
    // Slot 1: open the session picker / switch / new session.
    DrawSelectableIcon(kAiToggleX + 0 * kAiToggleStep, kAiToggleY,
                       a13_ai_session_list_16_asset, selected(1), 0);
    // Slots 2/3: jump to the previous / next answer.
    DrawSelectableIcon(kAiToggleX + 1 * kAiToggleStep, kAiToggleY,
                       a14_ai_turn_up_16_asset, selected(2), 0);
    DrawSelectableIcon(kAiToggleX + 2 * kAiToggleStep, kAiToggleY,
                       a15_ai_turn_down_16_asset, selected(3), 0);
    // Slot 4: the detail tier (0 简要 / 1 标准 / 2 详细). Agent-only: ?detail=N
    // shapes the /agent/* projections, which STD/Pro never reads.
    DrawSelectableIcon(kAiToggleX + 3 * kAiToggleStep, kAiToggleY,
                       AgentDetailAsset(agent.detail_level), selected(4), 0);
    // Slot 5: clear the mirrored Agent transcript.
    DrawSelectableIcon(kAiToggleX + 4 * kAiToggleStep, kAiToggleY,
                       a12_ai_clear_context_16_asset, selected(5), 0);

    // Centre column: which session is live. This is the one piece of context
    // the Agent tier cannot do without -- the same prompt means something
    // different in a different session. The clamp keeps the title clear of the
    // five-icon cluster, which still ends at x=118: STD/Pro dropped to four
    // icons when the follow toggle moved to the settings page, this tier did
    // not (slot 4 became the detail control).
    if (!agent.current_session_title.empty()) {
        const std::string title = AgentOneLine(agent.current_session_title, 150);
        const int w = wqn::MeasureUtf8TextWidth(title.c_str());
        const int cx = std::max(122, (wqn::kEpdWidth - w) / 2);
        AGENT_TEXT(cx, 6, title.c_str(), true);
    }

    // Right: phase label (status_label overrides the generic phase text).
    const std::string status = agent.ui.status_label.empty()
        ? std::string(wqn::AiFeaturePhaseLabel(agent.ui.phase))
        : agent.ui.status_label;
    if (!status.empty()) {
        const std::string line = AgentOneLine(status, 130);
        const int w = wqn::MeasureUtf8TextWidth(line.c_str());
        const int x = std::max(0, wqn::kEpdWidth - w - 26);
        AGENT_TEXT(x, 6, line.c_str(), true);
    }

    DrawWifiStatusIcon(wqn::kEpdWidth - 6, 6, home);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

esp_err_t RenderAgentAiToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::AgentSessionState& agent = frame.agent;
    wqn::ClearEpdFramebuffer(true);

    DrawAgentStatusBar(agent, frame.home, frame.status_edit);

    if (!agent.session_locked) {
        // Picker mode owns the whole viewport: there is no conversation to
        // scroll and no pending decision to act on.
        DrawAgentSessionPicker(agent);
        return RefreshFrame(frame, schedule);
    }

    RenderAiHistoryViewport(frame.ai, frame.ai_history, agent.ui.scroll_offset_lines);

    // [voice-pipe] While the ASR result streams in, the partial transcript is
    // the only new content on screen: draw it in the same pending bubble the
    // asks use, with no option bar (there is nothing to choose yet). This must
    // come before the kNone early return below -- kTranscribing projects no
    // option mode, so without this branch the partial would never render.
    if (agent.ui.phase == wqn::AiFeaturePhase::kTranscribing) {
        if (!agent.ui.voice_partial.empty()) {
            DrawAgentPendingBubble(agent.ui.voice_partial, "转写中");
        }
        return RefreshFrame(frame, schedule);
    }

    const AgentOptionMode mode = AgentOptionModeFor(agent);
    if (mode == AgentOptionMode::kNone) {
        return RefreshFrame(frame, schedule);
    }

    if (mode == AgentOptionMode::kPermission) {
        const std::string ask = agent.ui.activity_text.empty()
            ? std::string("OpenCode 请求权限")
            : agent.ui.activity_text;
        DrawAgentPendingBubble(ask, "权限请求");
    } else if (mode == AgentOptionMode::kQuestion) {
        const std::string ask = agent.pending_question_title.empty()
            ? std::string("OpenCode 请求回答")
            : agent.pending_question_title;
        DrawAgentPendingBubble(ask, "提问");
    } else {
        const std::string prompt = agent.ui.prompt_text.empty()
            ? std::string("(空)")
            : agent.ui.prompt_text;
        DrawAgentPendingBubble(prompt, "确认发送");
    }
    DrawAgentOptionBar(mode, frame.agent_option.focused, agent);
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
