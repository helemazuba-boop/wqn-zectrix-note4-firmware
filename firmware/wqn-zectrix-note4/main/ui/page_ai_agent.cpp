// Agent tier on the AI page: the OpenCode gateway rendered through the shared
// chat viewport, plus the two interaction surfaces the gateway needs that the
// STD/Pro tiers do not have --
//
//   1. a session picker (the gateway's conversations are server-side and
//      long-lived, so the user must be able to switch and to re-attach), and
//   2. an option bar (发送/重新输入 for the voice transcript, 同意/拒绝 for a
//      permission ask) driven by ↑/↓ + confirm. A question ask walks the
//      gateway's projected options as a two-slot window over the whole list,
//      with a trailing 自定义回答 pseudo-option that aborts the run.
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
#include "ui/assets/font_wqn_inline_12_1.h"
#include "ui/assets/font_wqn_ui_16_1.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

// [agent] Option bar geometry. It lives inside the same 22 px bottom band the
// scroll indicator uses (kAiViewportBottomPad), so adding it cost zero
// viewport height. It still leaves the ▼ chevron its column: the band clears
// and lays out only the width left of kAgentIndicatorColumn below, so a pending
// decision and the scroll position stay legible together.
constexpr int kAgentBarY = wqn::kEpdHeight - kAiViewportBottomPad;  // 278
constexpr int kAgentBarH = kAiViewportBottomPad;                    // 22
constexpr int kAgentBarTextY = kAgentBarY + 3;                      // baseline
constexpr int kAgentBarMarkerX = 8;
constexpr int kAgentBarLabelX = kAgentBarMarkerX + 16;
constexpr int kAgentBarSlotStep = 96;
// [scroll-indicator] The right-hand column page_ai.cpp reserves for the ▼/▲
// scroll chevrons (kEpdWidth - 40, 36 px wide). This tier's bottom band sits in
// the same 22 px strip the ▼ lives in, so the band clears and lays out only the
// width LEFT of that column.
//
// It used to clear and fill the full 400 px, which wiped the ▼ on every frame it
// drew -- and the band is drawn on nearly every frame, because every phase
// projects a gesture hint (B4's whole point). The Agent tier had therefore lost
// the "more below" indicator entirely while keeping "more above", and the only
// trace was the top-of-file comment claiming the chevron was "suppressed for
// that frame", which was true of the erasure and false as a description of a
// deliberate design.
//
// 44 px leaves the 36 px reserve plus an 8 px gutter from the band's own
// right-aligned text. Everything the band prints still fits: the longest
// right-aligned string is "↑↓切换 确认执行" at ~120 px, which now starts at
// ~228, clear of the option labels ending at 208.
constexpr int kAgentIndicatorColumn = 44;
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
    // blocked on a decision is the more urgent of the two states. An ask with
    // no id is unanswerable and falls through; a question with no options is
    // still a question -- the bar then offers only the 自定义回答 escape.
    if (agent.ui.phase == wqn::AiFeaturePhase::kAwaitingPermission &&
        !agent.pending_permission_id.empty()) {
        return AgentOptionMode::kPermission;
    }
    if (agent.ui.phase == wqn::AiFeaturePhase::kAwaitingQuestion &&
        !agent.pending_question_id.empty()) {
        return AgentOptionMode::kQuestion;
    }
    if (agent.ui.requires_confirmation) {
        return AgentOptionMode::kConfirmSend;
    }
    return AgentOptionMode::kNone;
}

// The question bar walks `options + 1` items: every projected option, plus a
// trailing 自定义回答 pseudo-option that escapes the ask by interrupting the
// run (a pending ask swallows every key, so a long-press cannot reach the
// interrupt path). Two of them are visible at a time.
int AgentQuestionItemCount(const wqn::AgentSessionState& agent)
{
    return static_cast<int>(agent.pending_question_options.size()) + 1;
}

int AgentQuestionWindowStart(const wqn::AgentSessionState& agent, uint8_t focused)
{
    const int count = AgentQuestionItemCount(agent);
    if (count <= 2) {
        return 0;
    }
    const int item = std::min<int>(focused, count - 1);
    // Keep the focused item in the window with the two slots available: the
    // start trails one behind the focus and stops one short of the end.
    return std::min(std::max(0, item - 1), count - 2);
}

int AgentQuestionFocusedItem(const wqn::AgentSessionState& agent, uint8_t focused)
{
    const int count = AgentQuestionItemCount(agent);
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
    // option bar cannot leave a ghost on the E-ink panel. Only the width left of
    // the scroll-indicator column is cleared -- see kAgentIndicatorColumn.
    FillRect(0, kAgentBarY, wqn::kEpdWidth - kAgentIndicatorColumn, kAgentBarH, false);

    // A rule above the bar separates it from the transcript above it.
    DrawHorizontalLine(0, kAgentBarY, wqn::kEpdWidth);

    const bool is_question = (mode == AgentOptionMode::kQuestion);
    const int item_count = is_question ? AgentQuestionItemCount(agent) : 2;
    const int window_start = is_question
        ? AgentQuestionWindowStart(agent, focused)
        : 0;
    const int focused_item = is_question
        ? AgentQuestionFocusedItem(agent, focused)
        : (AgentFocusedOption(mode, focused) == AgentFocusedOption(mode, 1) ? 1 : 0);
    const int slot_count = is_question ? std::min(item_count, 2) : 2;

    for (int i = 0; i < slot_count; ++i) {
        const int item = window_start + i;
        const int x = kAgentBarLabelX + i * kAgentBarSlotStep;
        if (item == focused_item) {
            // ▣ marker: a filled square says "this slot is armed" without
            // borrowing a directional chevron that would imply something else.
            FillRect(kAgentBarMarkerX + i * kAgentBarSlotStep, kAgentBarTextY + 3,
                     8, 8, true);
        }
        std::string label;
        if (is_question) {
            if (item < static_cast<int>(agent.pending_question_options.size())) {
                // The gateway projects {value,label}; an unlabelled option
                // still shows its value rather than an empty slot.
                label = agent.pending_question_options[item].label.empty()
                    ? agent.pending_question_options[item].value
                    : agent.pending_question_options[item].label;
            } else {
                label = "自定义回答";
            }
        } else {
            label = AgentOptionLabel(
                AgentFocusedOption(mode, static_cast<uint8_t>(i)));
        }
        AGENT_TEXT(x, kAgentBarTextY, AgentOneLine(label, 88).c_str(), true);
    }

    if (is_question) {
        // Position sense for the walk list: 2/9 while the window moves. It is
        // right-aligned inside the indicator column's left edge, so the key hint
        // shifts left to make room.
        const int right = wqn::kEpdWidth - kAgentIndicatorColumn - 8;
        char counter[16];
        snprintf(counter, sizeof(counter), "%d/%d", focused_item + 1, item_count);
        const int counter_w = wqn::MeasureUtf8TextWidth(counter);
        const char* hint = "↑↓ 确认";
        const int hint_w = wqn::MeasureUtf8TextWidth(hint);
        AGENT_TEXT(right - counter_w, kAgentBarTextY, counter, true);
        AGENT_TEXT(right - counter_w - hint_w - 16, kAgentBarTextY, hint, true);
        return;
    }

    // Key legend, right-aligned inside the indicator column's left edge.
    const int right = wqn::kEpdWidth - kAgentIndicatorColumn - 8;
    const char* hint = "↑↓切换 确认执行";
    const int hint_w = wqn::MeasureUtf8TextWidth(hint);
    AGENT_TEXT(right - hint_w, kAgentBarTextY, hint, true);
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
        // [D-which] The running marker. Item B1 of doc/1005 §0.1 makes attaching
        // the default, so the picker is no longer where the user goes to start
        // watching -- it is where they decide which session to look at. Before
        // this marker, a session with a run in flight was indistinguishable
        // from one that had never been prompted: the only way to find out was
        // to lock it and watch nothing happen, and with the picker clearing the
        // current session, doing that to the wrong row cost a detach.
        //
        // Only kRunning gets a marker. `outcome` also carries settled values
        // (succeeded / interrupted / failed / unknown), and marking those would
        // make the list say "this one ran" about rows that ran days ago -- a
        // list where most rows are marked is a list the user learns to ignore.
        // An absent or unrecognised value is kUnknown, which is no marker; that
        // direction is the same fail-safe the lease criterion in
        // opencode_session.cpp uses, and it is why a typo in that table costs a
        // missing glyph rather than a device that never sleeps.
        const bool running = agent.sessions[index].outcome ==
                            wqn::OpenCodeSessionOutcome::kRunning;
        // [D-which] Marker geometry, measured rather than assumed. The block is
        // the cloud's own "in flight" icon plus the label, right-aligned against
        // the panel edge; the title budget is then whatever room is actually
        // left, so the two can never collide whichever way the title wraps.
        //
        // Two things were wrong with the literal that was here:
        //
        //   1. The glyph was the character "◆" (U+25C6), which this font does
        //      not contain. SourceHanSansSC_Regular_slim covers ASCII, a
        //      ~70-codepoint punctuation/math/arrow set, and GB2312 -- U+25C6
        //      is in none of them (nor are the obvious substitutes: no U+25CF,
        //      U+26A1, U+2605). MeasureGlyphWidthInFont returns 0 on a miss and
        //      DrawGlyphFromFont skips it, so the marker drew as whitespace and
        //      the label alone carried the meaning -- which is why nobody saw
        //      the second problem until the glyph was made visible.
        //   2. The offset was the literal 16 + 6 * 16 + 8 = 120 px: a fixed
        //      column on a 400 px panel, not a right edge. The title was
        //      truncated to 300 px at x=16, so it can reach x=316 -- past it. Any
        //      title longer than ~104 px overlapped the (invisible) glyph.
        //
        // m02_ai_tool_running is the icon the tool blocks already use for a
        // running tool, so the picker says "in flight" with the same mark the
        // transcript does.
        static const char kRunningLabel[] = "运行中";
        const int label_w = wqn::MeasureUtf8TextWidth(kRunningLabel);
        constexpr int kMarkerIconW = 12;      // m02_ai_tool_running_12
        constexpr int kMarkerGap = 4;         // icon -> label
        constexpr int kMarkerRightPad = 8;
        constexpr int kMarkerTitleGap = 8;
        const int marker_x = wqn::kEpdWidth - kMarkerRightPad - kMarkerIconW -
                             kMarkerGap - label_w;
        const int title_max_w = marker_x - 16 - kMarkerTitleGap;
        const std::string title = AgentOneLine(
            agent.sessions[index].title.empty() ? agent.sessions[index].id
                                                : agent.sessions[index].title,
            running ? title_max_w : 360);
        AGENT_TEXT(16, y, title.c_str(), !focused);
        if (running) {
            // 12 px icon centred in the 16 px line box.
            //
            // `!focused`, like the title and the label beside it. The focused row
            // is an INK reverse-fill (FillRoundedRect above is the [rowfill] style
            // -- see graphics.cpp:132), so every element on that row has to be
            // painted in paper colour. Hardcoding black here drew a black icon
            // inside a black filled row: on the one row the user is looking at,
            // the marker said nothing.
            DrawWqnBitmapAsset(marker_x, y + 2, m02_ai_tool_running_12_asset, !focused);
            AGENT_TEXT(marker_x + kMarkerIconW + kMarkerGap, y, kRunningLabel, !focused);
        }
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
        // [visibility] While a run is live the cloud keeps writing
        // activity_text (已批准权限 / 已回答 / 已请求中止 ...), but without a
        // surface of its own a silent run reads as a hung one. Draw it in the
        // same bottom band the option bar owns; the option bar, when it
        // appears, clears the band itself.
        //
        // [B4] The gesture hint rides the same line, right-aligned, and it is
        // the reason the band is drawn at all for the phases it now covers.
        // `action_hint` was previously the only place the device said what the
        // confirm key was about to do, and its sole consumer was the frame
        // signature in ui_refresh.cpp -- so every word of it was invisible, and
        // the one gesture with two meanings (long press = capture when idle,
        // long press = interrupt while a run is in flight) was undiscoverable.
        // The failure it hid is the worst kind: a user who long-presses to send
        // into a running session gets silence for one gesture and an interrupt
        // for the next.
        //
        // Hence the phases below. A run that is live must SAY that the long
        // press will stop it; a finished one must say the long press starts a
        // new task. Both are drawn from action_hint so there is exactly one
        // string per state, set where the state is decided.
        //
        // Only the phases AgentOptionModeFor projects as kNone can reach here:
        // the two ask phases render the option bar below instead, so they get
        // their hint from it. A capture (kLoading / kRecording) draws its own
        // partial above and leaves the band to activity_text.
        std::string hint;
        switch (agent.ui.phase) {
            case wqn::AiFeaturePhase::kRunning:
                // Fixed, not taken from action_hint: at kRunning that field holds
                // whatever the last frame happened to leave in it (a permission
                // was just answered, a status line came through), and a long
                // press here interrupts the run no matter what the cloud said.
                // B4's requirement is that the one gesture with two meanings is
                // named in the state where the meaning is the surprising one.
                hint = "长按=中止";
                break;
            case wqn::AiFeaturePhase::kIdle:
            case wqn::AiFeaturePhase::kComplete:
            case wqn::AiFeaturePhase::kError:
                // Terminal states: the state's owner set the precise gesture
                // (发起新任务 / 重试新任务), and those differ by phase. Fall back
                // to the ordinary capture for a state nobody labelled.
                hint = agent.ui.action_hint.empty() ? std::string("长按=录音")
                                                    : agent.ui.action_hint;
                break;
            default:
                break;
        }
        if (!agent.ui.activity_text.empty() || !hint.empty()) {
            const int right = wqn::kEpdWidth - kAgentIndicatorColumn - 8;
            const int hint_w = hint.empty() ? 0 : wqn::MeasureUtf8TextWidth(hint.c_str());
            FillRect(0, kAgentBarY, wqn::kEpdWidth - kAgentIndicatorColumn, kAgentBarH, false);
            DrawHorizontalLine(0, kAgentBarY, wqn::kEpdWidth);
            if (!hint.empty()) {
                AGENT_TEXT(right - hint_w, kAgentBarTextY, hint.c_str(), true);
            }
            if (!agent.ui.activity_text.empty()) {
                // The cloud's own line, truncated to the room the hint leaves.
                // A capture owns its own band above, so the marker column is
                // where the run's activity line starts.
                const std::string body =
                    hint.empty()
                        ? AgentOneLine(agent.ui.activity_text, right - 16)
                        : AgentOneLine(agent.ui.activity_text,
                                       right - hint_w - 16);
                AGENT_TEXT(kAgentBarMarkerX, kAgentBarTextY, body.c_str(), true);
            }
        }
        return RefreshFrame(frame, schedule);
    }

    if (mode == AgentOptionMode::kPermission) {
        const std::string ask = agent.ui.activity_text.empty()
            ? std::string("OpenCode 请求权限")
            : agent.ui.activity_text;
        DrawAgentPendingBubble(ask, "权限请求");
    } else if (mode == AgentOptionMode::kQuestion) {
        std::string ask = agent.pending_question_title.empty()
            ? std::string("OpenCode 请求回答")
            : agent.pending_question_title;
        if (agent.pending_question_options.empty()) {
            // A field with nothing the device can choose between (free text,
            // an over-long list). Say so instead of showing a bar that looks
            // broken; 自定义回答 aborts the run so the request can be retyped.
            //
            // [font-fix] The gap was an ideographic space (U+3000), which this
            // font does not contain: MeasureGlyphWidthInFont returns 0 on a miss,
            // so it contributed no width and no ink, and the advisory butted
            // straight onto the question title it is appended to. A plain ASCII
            // space is the smallest separator the font can actually draw. There is
            // only one candidate here -- `ask` is the title, so the advisory has to
            // be visually separate from it.
            ask += " 设备无法显示选项；选“自定义回答”将中止本次任务";
        }
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
