// AI v2 page: status-bar (no battery, merges toast state) + scrollable chat
// viewport. The previous single-bubble layout (user @ y=44 / assistant @ y=116
// / input bar @ y=252) is gone. The 24-px top toast slot was removed in v2.1:
// double-border (status-bar bottom + toast bottom) was eating 24 px of usable
// vertical space and producing a stale E-ink band. Toast state now lives
// inside the status bar itself (replaces the time column when active).
//
// Tool-use blocks render expanded (default, no interaction): name + args +
// result + elapsed.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ai_history.h"
#include "ai_session.h"
#include "display_service.h"
#include "esp_log.h"
#include "flash_session.h"
#include "ui/assets/font_wqn_inline_12_1.h"
#include "ui/assets/font_wqn_ui_16_1.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

constexpr int kAiStatusBarY = 0;
// [v2] Status bar height aligned to the global kStatusBarHeight(28) so the
// bottom divider lands on the shared kStatusBarDividerY(27) like every other
// page (was 27, putting the line at 26 -- the 1px outlier).
constexpr int kAiStatusBarH = 28;
// [v2.1] No more dedicated toast region. The status-bar bottom rule still
// draws, but the viewport starts immediately after it, recovering 24 px of
// vertical space we previously lost to the redundant toast strip.
constexpr int kAiViewportY = kAiStatusBarY + kAiStatusBarH;     // 27
constexpr int kAiViewportH = wqn::kEpdHeight - kAiViewportY;     // 273
constexpr int kAiLineH = kCjkLineHeight;                                   // single line height
constexpr int kAiLineGap = 4;                                  // vertical gap between bubbles
constexpr int kAiAssistantLeftBorder = 4;                      // assistant left edge 4px inset
constexpr int kAiHistoryLeftPad = 6;
constexpr int kAiHistoryRightPad = 6;
constexpr int kAiHistoryRightEdge = wqn::kEpdWidth - kAiHistoryRightPad;
constexpr int kAiHistoryUsableW = kAiHistoryRightEdge - kAiHistoryLeftPad;  // 388
constexpr int kAiUserPillMaxW = kAiHistoryUsableW * 78 / 100;                // ~302
constexpr int kAiAssistantW = kAiHistoryUsableW;
constexpr int kAiRowTopPad = 2;
// [scroll-fix] Reserve 22 px at the bottom of the viewport for the ▼ scroll
// indicator + breathing room so the latest message never overlaps it.
constexpr int kAiViewportBottomPad = 22;

const char* AiStatusLabel(wqn::AiSessionStatus status)
{
    switch (status) {
        case wqn::AiSessionStatus::kPreparingCapture:
            return "准备录音";
        case wqn::AiSessionStatus::kListening:
            return "录音";
        case wqn::AiSessionStatus::kWaitingReply:
            return "识别";
        case wqn::AiSessionStatus::kStreaming:
            return "流式";
        case wqn::AiSessionStatus::kReplyReady:
            return "完成";
        case wqn::AiSessionStatus::kError:
            return "错误";
        case wqn::AiSessionStatus::kIdle:
        default:
            return "空闲";
    }
}

// ---------------------------------------------------------------------------
// Section helpers
// ---------------------------------------------------------------------------

#define DRC(...) do { (void)wqn::DrawUtf8Text(__VA_ARGS__); } while (0)

// Shared 1bpp status-bar assets. selected = 反白 (ink bg + paper glyph).
// Delegated to DrawSelectableIcon (ui_widgets) so the reverse-fill focus
// language is owned by the decoration layer. padding=0 keeps the legacy
// 16x16 cell semantics (background square == asset size, glyph at (x,y)).
static void DrawStatusAsset(int x, int y, const WqnBitmapAsset& asset, bool selected) {
    DrawSelectableIcon(x, y, asset, selected, /*padding=*/0);
}

static void DrawThinkingIcon(int x, int y, wqn::ThinkingLevel level, bool selected) {
    const WqnBitmapAsset* asset = &a04_ai_thinking_off_16_asset;
    switch (level) {
        case wqn::ThinkingLevel::kLow: asset = &a05_ai_thinking_low_16_asset; break;
        case wqn::ThinkingLevel::kMed: asset = &a06_ai_thinking_medium_16_asset; break;
        case wqn::ThinkingLevel::kHigh: asset = &a07_ai_thinking_high_16_asset; break;
        case wqn::ThinkingLevel::kOff:
        default: break;
    }
    DrawStatusAsset(x, y, *asset, selected);
}
static void DrawTtsIcon(int x, int y, bool on, bool selected) {
    DrawStatusAsset(x, y, on ? a08_ai_tts_on_16_asset : a09_ai_tts_off_16_asset, selected);
}
static void DrawExpandIcon(int x, int y, bool on, bool selected) {
    DrawStatusAsset(x, y, on ? a10_ai_detail_expand_on_16_asset : a11_ai_detail_expand_off_16_asset, selected);
}

// [tier-icon] Status-bar tier indicator (16px, display only - tier switch is
// double-press Up/Down). Flash=lightning, STD=hourglass, Pro=brain.
static void DrawLightningIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a01_ai_tier_flash_16_asset, selected);
}
static void DrawHourglassIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a02_ai_tier_standard_16_asset, selected);
}
static void DrawBrainIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a03_ai_tier_pro_16_asset, selected);
}
// [trash] Clear-context action button (edit-mode index 3, STD/Pro only).
static void DrawTrashIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a12_ai_clear_context_16_asset, selected);
}

// [toggle-cluster] The four status-bar toggles (thinking/TTS/expand/trash)
// pack tightly right after the tier icon. tier occupies x=6..22 (16px); the
// toggle cluster starts at kAiToggleX with a small gap, and consecutive
// toggles are kAiToggleStep apart (18 = 16px icon + 2px gap). The edit-mode
// zone rect spans exactly the four icons plus a 2px pad on each side.
constexpr int kAiToggleX = 30;
constexpr int kAiToggleY = 5;
constexpr int kAiToggleStep = 18;

void DrawAiStatusBar(const wqn::AiSessionState& ai, const wqn::HomeSummary& home, const wqn::StatusBarEditState& status_edit)
{
    // Status bar occupies y=0..27 with a bottom divider line.
    DrawHorizontalLine(0, kAiStatusBarH - 1, wqn::kEpdWidth);
    // Left: page title + tier chip.
    // [tier-icon] Tier indicator + edit-mode button 0 (replaces "AI" + tier text):
    // Flash=lightning, STD=hourglass, Pro=brain. confirm cycles tier (all tiers).
    const bool tier_sel = status_edit.active && status_edit.selected == 0;
    switch (ai.tier) {
        case wqn::AiTier::kFlash: DrawLightningIcon(6, kAiToggleY, tier_sel); break;
        case wqn::AiTier::kPro:   DrawBrainIcon(6, kAiToggleY, tier_sel); break;
        case wqn::AiTier::kStd:
        default:                  DrawHourglassIcon(6, kAiToggleY, tier_sel); break;
    }

    // [shell] Toggle zone (STD/Pro only): thinking(1)/TTS(2)/expand(3)/trash(4).
    // Flash hides the whole zone (only the tier icon, button 0, is editable).
    if (ai.tier != wqn::AiTier::kFlash) {
        if (status_edit.active) {
            // Zone rect hugs the four tightly-packed toggle icons (2px pad).
            DrawRect(kAiToggleX - 2, kAiToggleY - 2, kAiToggleStep * 3 + 16 + 4, 20);
        }
        DrawThinkingIcon(kAiToggleX + 0 * kAiToggleStep, kAiToggleY, ai.thinking_level, status_edit.active && status_edit.selected == 1);
        DrawTtsIcon(kAiToggleX + 1 * kAiToggleStep, kAiToggleY, ai.tts_on, status_edit.active && status_edit.selected == 2);
        DrawExpandIcon(kAiToggleX + 2 * kAiToggleStep, kAiToggleY, ai.expand_content, status_edit.active && status_edit.selected == 3);
        DrawTrashIcon(kAiToggleX + 3 * kAiToggleStep, kAiToggleY, status_edit.active && status_edit.selected == 4);
    }

    // Center column: reserved for the toast label. When the toast is visible
    // the label is centred horizontally and the status readout moves out of the
    // status bar; when the toast is absent the centre column is left empty
    // (the idle status label lives on the right, joining the WiFi glyph there).
    // The clock is intentionally not shown on the AI status bar -- AI omits the
    // time column the other pages show, so the three-way (toast/clock/status)
    // contest for the centre column shrinks to just toast here.
    if (ai.toast_visible && !ai.toast_label.empty()) {
        const int w = wqn::MeasureUtf8TextWidth(ai.toast_label.c_str());
        const int cx = std::max(70, (wqn::kEpdWidth - w) / 2);
        DRC(cx, 6, ai.toast_label.c_str(), true);
        // Blinker square on the right side when active.
        FillRect(wqn::kEpdWidth - 34, 8, 4, 4, true);
    }

    // Right-of-status: idle-state status label only (skipped while toast
    // owns the centre column to avoid double-drawing).
    if (!ai.toast_visible) {
        const std::string status =
            (ai.tier == wqn::AiTier::kFlash && !ai.flash_status_label.empty())
                ? ai.flash_status_label
                : AiStatusLabel(ai.status);
        if (!status.empty()) {
            const int status_width = wqn::MeasureUtf8TextWidth(status.c_str());
            const int status_x = std::max(0, 320 - status_width);
            DRC(status_x, 6, status.c_str(), true);
        }
    }

    // Reuse the same four-state WiFi glyph as the other pages; AI omits battery.
    DrawWifiStatusIcon(wqn::kEpdWidth - 6, 6, home);
}

void DrawHistoryClear(int y0, int y1)
{
    if (y1 <= y0) {
        return;
    }
    FillRect(0, y0, wqn::kEpdWidth, y1 - y0, false);
}

// Wrap a UTF-8 string to fit within `max_w_px`, capping at `max_lines` rows.
// Returns the (possibly truncated) wrapped text plus the row count used.
std::vector<std::string> WrapForViewport(const std::string& text, int max_w_px, size_t max_lines)
{
    if (text.empty()) {
        return {""};
    }
    auto lines = wqn::WrapUtf8TextToWidth(text, max_w_px, max_lines);
    if (lines.empty()) {
        return {""};
    }
    return lines;
}

int DrawUserPill(int x, int y, int max_w, const std::string& text)
{
    const auto lines = WrapForViewport(text, max_w - 16, 4);
    const int row_count = static_cast<int>(lines.size());
    const int row_h = kAiLineH;
    const int total_h = row_count * row_h + 4;

    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(wqn::kEpdHeight, y + total_h);

    if (visible_top < visible_bottom) {
        // [v2] User pill uses the rounded container language (r4) like every
        // other container; the square DrawRect outline was the last square
        // container in the product. Inner paper fill keeps the text legible.
        DrawRoundedRect(x, visible_top, max_w, visible_bottom - visible_top, 4);
        FillRect(x + 1, visible_top + 1, max_w - 2, visible_bottom - visible_top - 2, false);
    }
    for (int r = 0; r < row_count; ++r) {
        const int line_y = y + r * row_h + 4;
        if (line_y >= kAiViewportY && line_y + row_h <= wqn::kEpdHeight) {
            DRC(x + 8, line_y, lines[r].c_str(), true);
        }
    }
    return total_h;
}

// [md-rows] Assistant markdown row-count cache for the measure pass. Old
// messages never change, so after the first frame the layout loop costs O(1)
// per message; the streaming reply invalidates via its growing byte length.
// Bounded memory (two words per message); reset when the history shrinks (new
// session) so stale indices cannot alias. A same-index same-length different
// text would briefly reuse a stale count -- it self-heals on the next size
// change and can only happen across a session swap that kept message count.
int CachedAssistantMdRows(size_t msg_idx, const std::string& text, size_t total_msgs)
{
    struct Entry {
        size_t text_size = SIZE_MAX;
        int rows = 1;
    };
    static std::vector<Entry> s_rows;
    if (s_rows.size() > total_msgs) {
        s_rows.clear();
    }
    if (msg_idx >= s_rows.size()) {
        s_rows.resize(msg_idx + 1);
    }
    Entry& entry = s_rows[msg_idx];
    if (entry.text_size != text.size()) {
        entry.rows = std::max<int>(
            1,
            static_cast<int>(CountMarkdownLines(
                text, kAiAssistantW - kAiAssistantLeftBorder - 6)));
        entry.text_size = text.size();
    }
    return entry.rows;
}

int DrawAssistantBlock(int x, int y, int max_w, const std::string& text)
{
    // [markdown] Assistant replies render through the shared Markdown row
    // pipeline (headings, lists, tables, inline adornments -- same engine as
    // the note body). Only viewport-intersecting blocks reach this function,
    // so the per-call layout stays bounded; the measure pass counts rows via
    // CachedAssistantMdRows at the SAME width, keeping this block's height
    // equal to its virtual-canvas slot.
    const int content_w = max_w - kAiAssistantLeftBorder - 6;
    const std::vector<MdLine> md_lines = LayoutMarkdown(text, content_w);
    const int row_count = std::max(1, static_cast<int>(md_lines.size()));
    const int row_h = kAiLineH;
    const int total_h = row_count * row_h + 6;

    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(wqn::kEpdHeight, y + total_h);

    if (visible_top < visible_bottom) {
        FillRect(x, visible_top, kAiAssistantLeftBorder, visible_bottom - visible_top, true);
    }
    const int text_x = x + kAiAssistantLeftBorder + 6;
    for (size_t r = 0; r < md_lines.size(); ++r) {
        const int line_y = y + static_cast<int>(r) * row_h + 4;
        if (line_y >= kAiViewportY && line_y + row_h <= wqn::kEpdHeight) {
            DrawMarkdownLine(md_lines[r], text_x, line_y, content_w, row_h);
        }
    }
    const int rule_y = y + total_h - 2;
    if (rule_y >= kAiViewportY && rule_y < wqn::kEpdHeight) {
        DrawHorizontalLine(x, rule_y, max_w);
    }
    return total_h;
}

std::vector<std::string> BuildThinkingLines(const std::string& text, int max_w, bool expanded)
{
    constexpr int kThinkingTextInset = 28;  // border + 12px marker + gap
    if (expanded) {
        return text.empty() ? std::vector<std::string>{""}
                            : WrapForViewport(text, max_w - kThinkingTextInset, 1024);
    }
    std::string one = wqn::TruncateUtf8TextToWidth(text, max_w - kThinkingTextInset);
    if (one.size() < text.size() && one.find("...") == std::string::npos) {
        one = wqn::TruncateUtf8TextToWidth(text, max_w - kThinkingTextInset - 18) + "...";
    }
    return {one};
}

std::vector<std::string> BuildToolResultLines(const wqn::ChatMessageSnapshot& tool, int max_w)
{
    if (tool.tool_result_json.empty()) {
        return {};
    }
    return WrapForViewport("result: " + tool.tool_result_json, max_w - 34, 2);
}

int ToolBlockHeight(const wqn::ChatMessageSnapshot& tool, int max_w, bool expanded)
{
    const int row_h = kAiLineH;
    int expected_h = 6 + row_h;
    if (expanded) {
        if (!tool.tool_args_json.empty()) {
            expected_h += static_cast<int>(WrapForViewport(
                "args: " + tool.tool_args_json, max_w - 12, 2).size()) * row_h;
        }
        expected_h += static_cast<int>(BuildToolResultLines(tool, max_w).size()) * row_h;
        expected_h += row_h;
    }
    return expected_h + 6;
}

int DrawThinkingBlock(int x, int y, int max_w, const std::string& text, bool expanded)
{
    const auto lines = BuildThinkingLines(text, max_w, expanded);
    const int row_count = static_cast<int>(lines.size());
    const int row_h = kAiLineH;
    const int total_h = row_count * row_h + 6;

    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(wqn::kEpdHeight, y + total_h);

    if (visible_top < visible_bottom) {
        DrawVerticalLine(x, visible_top, visible_bottom - visible_top);
    }
    const int text_x = x + 22;
    for (int r = 0; r < row_count; ++r) {
        const int line_y = y + r * row_h + 4;
        if (line_y >= kAiViewportY && line_y + row_h <= wqn::kEpdHeight) {
            if (r == 0) {
                DrawWqnBitmapAsset(x + 6, line_y + 2, m01_ai_thinking_marker_12_asset, true);
            }
            DRC(text_x, line_y, lines[r].c_str(), true);
        }
    }
    return total_h;
}

int DrawToolBlock(int x, int y, int max_w, const wqn::ChatMessageSnapshot& tool, bool expanded)
{
    char header[64];
    std::snprintf(header, sizeof(header), "[Tool: %s]",
                  tool.tool_name.c_str()[0] ? tool.tool_name.c_str() : "tool");
    char elapsed[32] = {};
    std::snprintf(elapsed, sizeof(elapsed), "elapsed: %ld ms", static_cast<long>(tool.tool_elapsed_ms));

    const int row_h = kAiLineH;
    const int expected_h = ToolBlockHeight(tool, max_w, expanded);

    std::vector<std::string> args_lines;
    std::vector<std::string> result_lines;
    if (expanded) {
        if (!tool.tool_args_json.empty()) {
            args_lines = WrapForViewport("args: " + tool.tool_args_json, max_w - 12, 2);
        }
        result_lines = BuildToolResultLines(tool, max_w);
    }

    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(wqn::kEpdHeight, y + expected_h);

    if (visible_top < visible_bottom) {
        DrawVerticalLine(x, visible_top, visible_bottom - visible_top);
        DrawVerticalLine(x + max_w - 1, visible_top, visible_bottom - visible_top);
    }
    if (y >= kAiViewportY && y < wqn::kEpdHeight) {
        DrawHorizontalLine(x, y, max_w);
    }
    const int bottom_y = y + expected_h - 1;
    if (bottom_y >= kAiViewportY && bottom_y < wqn::kEpdHeight) {
        DrawHorizontalLine(x, bottom_y, max_w);
    }

    // Header row.
    if (y + 4 >= kAiViewportY && y + 4 + row_h <= wqn::kEpdHeight) {
        DrawWqnBitmapAsset(x + 4, y + 6,
                           expanded ? m06_chevron_down_12_asset : m05_chevron_right_12_asset, true);
        DrawWqnBitmapAsset(x + 18, y + 6, m02_ai_tool_running_12_asset, true);
        DRC(x + 34, y + 4, header, true);
    }

    if (expanded) {
        int cur_y = y + 4 + row_h;
        // Args row.
        for (const auto& l : args_lines) {
            if (cur_y >= kAiViewportY && cur_y + row_h <= wqn::kEpdHeight) {
                DRC(x + 8, cur_y, l.c_str(), true);
            }
            cur_y += row_h;
        }

        // Result row.
        for (size_t i = 0; i < result_lines.size(); ++i) {
            if (cur_y >= kAiViewportY && cur_y + row_h <= wqn::kEpdHeight) {
                if (i == 0) {
                    DrawWqnBitmapAsset(
                        x + 8, cur_y + 2,
                        tool.tool_ok ? m03_status_success_12_asset : m04_status_error_12_asset, true);
                }
                DRC(x + 24, cur_y, result_lines[i].c_str(), true);
            }
            cur_y += row_h;
        }

        // Elapsed row.
        if (cur_y >= kAiViewportY && cur_y + row_h <= wqn::kEpdHeight) {
            DRC(x + 8, cur_y, elapsed, true);
        }
    }

    return expected_h;
}

void RenderAiHistoryViewport(const wqn::AiSessionState& ai,
                            const std::shared_ptr<const wqn::AiHistorySnapshot>& snapshot,
                            int32_t scroll_offset_lines)
{
    // Clear the viewport region explicitly (partial-region contract).
    DrawHistoryClear(kAiViewportY, wqn::kEpdHeight);

    const int line_h = kAiLineH;
    static const std::vector<wqn::ChatMessageSnapshot> kEmpty;
    const auto& messages = snapshot ? snapshot->messages : kEmpty;
    if (messages.empty() && ai.status == wqn::AiSessionStatus::kIdle) {
        const std::string hint = "长按确认键开始语音提问";
        const auto lines = WrapForViewport(hint, kAiAssistantW - kAiAssistantLeftBorder - 6, 4);
        int y = kAiViewportY + 30;
        for (const auto& l : lines) {
            DRC(kAiHistoryLeftPad + kAiAssistantLeftBorder + 6, y, l.c_str(), true);
            y += line_h;
        }
        return;
    }

    // 1. Calculate heights and layout of all messages
    struct Layout {
        int height;
        size_t msg_idx;
        int rows_consumed;
    };
    std::vector<Layout> layout;
    const size_t n = messages.size();
    
    // Index 0 is oldest, n-1 is newest. We build layout from newest (n-1) to oldest (0).
    for (size_t i = 0; i < n; ++i) {
        const size_t msg_idx = n - 1 - i;
        const wqn::ChatMessageSnapshot& msg = messages[msg_idx];
        int h = 0;
        int rows_consumed = 0;
        const std::string body(msg.text.empty() ? std::string("-")
                                               : std::string(msg.text.data(), msg.text.size()));
        if (msg.kind == wqn::ChatMessageKind::kUser) {
            const auto lines = WrapForViewport(body, kAiUserPillMaxW - 16, 4);
            h = static_cast<int>(lines.size()) * line_h + 4;
            rows_consumed = static_cast<int>(lines.size());
        } else if (msg.kind == wqn::ChatMessageKind::kAssistant) {
            // Layout pass -- markdown row count, cached per message so only
            // the streaming reply is re-laid out while it grows. MUST use the
            // same width as DrawAssistantBlock or block heights drift.
            const int rows = CachedAssistantMdRows(msg_idx, body, n);
            h = rows * line_h + 6;
            rows_consumed = rows;
        } else if (msg.kind == wqn::ChatMessageKind::kThinking) {
            const auto lines = BuildThinkingLines(body, kAiAssistantW, ai.expand_content);
            h = static_cast<int>(lines.size()) * line_h + 6;
            rows_consumed = static_cast<int>(lines.size());
        } else {
            h = ToolBlockHeight(msg, kAiAssistantW, ai.expand_content);
            rows_consumed = h / line_h;
        }
        layout.push_back({h, msg_idx, rows_consumed});
    }

    // 2. Determine the visible subset of messages and place them inside the
    // viewport.
    //
    // Layout strategy: chat conversation reads top→bottom in chronological
    // order (oldest at top, newest at bottom — standard chat convention).
    // After each AI reply finishes we want the user to land on the most
    // recent question + reply pair, with the user's question sitting at the
    // top of the visible viewport and the AI's reply stacked directly
    // underneath. Older turns are above the user question and get scrolled
    // off the top.
    //
    // scroll_offset_lines is then a relative adjustment on top of this
    // anchor: positive means "scroll up into older history", negative means
    // "scroll down past the latest content" (clamped at 0).
    // (line_h already declared above.)

    // 2. Determine the visible subset of messages and place them inside the
    // viewport.
    //
    // Layout strategy: chat conversation reads top→bottom in chronological
    // order (oldest at top, newest at bottom — standard chat convention).
    // All messages are laid out in a virtual canvas from oldest to newest.
    const int layout_size = static_cast<int>(layout.size());
    std::vector<int> virtual_tops(layout_size);
    int current_y = 0;
    for (int i = layout_size - 1; i >= 0; --i) {
        virtual_tops[i] = current_y;
        current_y += layout[i].height + kAiLineGap;
    }
    int total_content_h = current_y - kAiLineGap;
    if (total_content_h < 0) {
        total_content_h = 0;
    }

    const int effective_bottom = wqn::kEpdHeight - kAiViewportBottomPad;
    const int viewport_h = effective_bottom - kAiViewportY;

    // Locate the most-recent user message for default top-viewport anchoring.
    int newest_user_idx = -1;
    for (size_t i = 0; i < layout.size(); ++i) {
        const Layout& L = layout[i];
        if (messages[L.msg_idx].kind == wqn::ChatMessageKind::kUser) {
            newest_user_idx = static_cast<int>(i);
            break;
        }
    }

    int anchor_top = 0;
    if (newest_user_idx >= 0) {
        anchor_top = virtual_tops[newest_user_idx];
    }

    int max_window_top = total_content_h - viewport_h;
    if (max_window_top < 0) {
        max_window_top = 0;
    }

    // Scroll limits in line units
    int max_scroll = anchor_top / line_h;
    int min_scroll = (anchor_top - max_window_top) / line_h;
    if (min_scroll > 0) {
        min_scroll = 0;
    }

    // Clamp the scroll offset lines to the valid range
    int clamped_scroll = scroll_offset_lines;
    if (clamped_scroll < min_scroll) {
        clamped_scroll = min_scroll;
    }
    if (clamped_scroll > max_scroll) {
        clamped_scroll = max_scroll;
    }
    if (clamped_scroll != scroll_offset_lines) {
        wqn::SetAiScrollOffsetLines(clamped_scroll);
    }

    int window_top = anchor_top - clamped_scroll * line_h;
    if (window_top < 0) {
        window_top = 0;
    } else if (window_top > max_window_top) {
        window_top = max_window_top;
    }

    const bool oldest_fully_visible = (window_top == 0);

    // Draw all messages that intersect with the viewport
    for (int i = layout_size - 1; i >= 0; --i) {
        const Layout& L = layout[i];
        const wqn::ChatMessageSnapshot& msg = messages[L.msg_idx];
        const int block_top = kAiViewportY + (virtual_tops[i] - window_top);

        // Clip completely above viewport
        if (block_top + L.height <= kAiViewportY) {
            continue;
        }
        // Clip completely below viewport
        if (block_top >= effective_bottom) {
            continue;
        }

        // Draw block
        const std::string body(msg.text.empty() ? std::string("-")
                                               : std::string(msg.text.data(), msg.text.size()));
        const int x_left = kAiHistoryLeftPad;
        if (msg.kind == wqn::ChatMessageKind::kUser) {
            const auto lines = WrapForViewport(body, kAiUserPillMaxW - 16, 4);
            int actual_w = 16;
            for (const auto& l : lines) {
                actual_w = std::max(actual_w, wqn::MeasureUtf8TextWidth(l.c_str()) + 16);
            }
            const int px_right = wqn::kEpdWidth - 8;
            const int px_left = std::max(kAiHistoryLeftPad, px_right - actual_w);
            DrawUserPill(px_left, block_top, px_right - px_left, body);
        } else if (msg.kind == wqn::ChatMessageKind::kAssistant) {
            DrawAssistantBlock(x_left, block_top, kAiAssistantW, body);
        } else if (msg.kind == wqn::ChatMessageKind::kThinking) {
            DrawThinkingBlock(x_left, block_top, kAiAssistantW, body, ai.expand_content);
        } else {
            DrawToolBlock(x_left, block_top, kAiAssistantW, msg, ai.expand_content);
        }
    }

    // 3. Scroll chevrons. We place ▼ at the bottom-right corner inside the
    // `kAiViewportBottomPad` slack so it never overlaps content. ▲ sits in
    // the top-right corner when there's more above. The indicator strip is
    // cleared each tick to avoid ghosting on the E-ink panel.
    if (messages.size() > 1) {
        const bool more_above = !oldest_fully_visible;
        const bool more_below = (window_top < max_window_top);
        // Reserve a right-edge indicator region; clear it before
        // drawing so a disappearing indicator does not leave a phantom.
        FillRect(wqn::kEpdWidth - 40, wqn::kEpdHeight - kAiViewportBottomPad + 2,
                 36, kAiViewportBottomPad - 4, false);
        FillRect(wqn::kEpdWidth - 40, kAiViewportY + 1, 36, 14, false);
        if (more_below) {
            char count[12];
            std::snprintf(count, sizeof(count), "%ld",
                          static_cast<long>((max_window_top - window_top + line_h - 1) / line_h));
            const int count_w = wqn::MeasureUtf8TextWidth(count);
            const int x = wqn::kEpdWidth - count_w - 18;
            // Bottom-right, 2 px above the panel bottom.
            DrawWqnBitmapAsset(x, wqn::kEpdHeight - 16, m06_chevron_down_12_asset, true);
            DRC(x + 14, wqn::kEpdHeight - 18, count, true);
        }
        if (more_above) {
            // Show the index of the oldest message that's hidden above so
            // the user has a rough sense of how many turns are above.
            char count[12];
            std::snprintf(count, sizeof(count), "%ld", static_cast<long>(window_top / line_h));
            const int count_w = wqn::MeasureUtf8TextWidth(count);
            const int x = wqn::kEpdWidth - count_w - 18;
            DrawWqnBitmapAsset(x, kAiViewportY + 3, m07_chevron_up_12_asset, true);
            DRC(x + 14, kAiViewportY + 4, count, true);
        }
    }

    // 4. "已最新" hint — shown briefly when the user pressed Down at the
    // bottom of the scroll. Stamped by the input handler, expires after 1s.
    if (ai.scroll_no_op_hint_ms > 0) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - ai.scroll_no_op_hint_ms <= 1000) {
            // Centre the label horizontally, just above the bottom pad band.
            const char* hint = "\xe5\xb7\xb2\xe6\x9c\x80\xe6\x96\xb0";  // 已最新
            const int w = wqn::MeasureUtf8TextWidth(hint);
            const int cx = std::max(70, (wqn::kEpdWidth - w) / 2);
            DRC(cx, wqn::kEpdHeight - kAiViewportBottomPad - 18, hint, true);
        }
    }
}

esp_err_t RenderAiToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::AiSessionState& ai = frame.ai;
    wqn::ClearEpdFramebuffer(true);

    // Section 1: status bar (no battery, merges toast state).
    DrawAiStatusBar(ai, frame.home, frame.status_edit);

    // Section 2: viewport (no bottom input bar, no separate toast strip).
    RenderAiHistoryViewport(ai, frame.ai_history, ai.scroll_offset_lines);

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
