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
#include <functional>
#include <mutex>

#include "ai_native_text.h"
#include "ai_scroll_geometry.h"
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

// [agent] All AI-page geometry lives in ui_layout.h so page_ai_agent.cpp (the
// Agent tier, which renders through this same viewport) cannot drift from it.

const char* AiStatusLabel(wqn::AiSessionStatus status)
{
    switch (status) {
        case wqn::AiSessionStatus::kPreparingCapture:
            return "准备录音";
        case wqn::AiSessionStatus::kTranscribing:
            return "转写中";
        case wqn::AiSessionStatus::kAwaitingConfirmation:
            return "确认发送";
        case wqn::AiSessionStatus::kAwaitingDecision:
            return "等你回答";
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

// [tier-icon] Status-bar tier indicator (16px, display only - the tier is
// cycled from the status-bar edit mode). Flash=lightning, STD=hourglass,
// Agent=brain (the retired Pro glyph, reused: the model behind the Agent tier
// is chosen server-side by the WQN OpenCode binding, so it needs no new asset).
static void DrawLightningIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a01_ai_tier_flash_16_asset, selected);
}
static void DrawHourglassIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a02_ai_tier_standard_16_asset, selected);
}
static void DrawBrainIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a03_ai_tier_pro_16_asset, selected);
}
// [trash] Clear-context action button (edit-mode index 4 on STD/Pro, 5 on the
// Agent tier -- the cluster is one slot shorter here since the follow toggle
// moved to the settings page).
static void DrawTrashIcon(int x, int y, bool selected) {
    DrawStatusAsset(x, y, a12_ai_clear_context_16_asset, selected);
}

// [toggle-cluster] The status-bar toggles pack tightly right after the tier
// icon. tier occupies x=6..22 (16px); the toggle cluster starts at
// kAiToggleX with a small gap, and consecutive toggles are kAiToggleStep
// apart (18 = 16px icon + 2px gap). The edit-mode zone rect (kAiToggleZoneWStd)
// spans exactly the icons plus a kAiToggleZonePad pad on each side. STD/Pro use
// the four slots for thinking/TTS/expand/trash; the Agent tier uses its five for
// session/turn-up/turn-down/detail/trash (page_ai_agent.cpp). The follow toggle
// moved to the settings page, so no tier draws it in the cluster any more.

void DrawAiStatusBar(const wqn::AiSessionState& ai, const wqn::HomeSummary& home, const wqn::StatusBarEditState& status_edit)
{
    // Status bar occupies y=0..27 with a bottom divider line.
    DrawHorizontalLine(0, kAiStatusBarH - 1, wqn::kEpdWidth);
    // Left: page title + tier chip.
    // [tier-icon] Tier indicator + edit-mode button 0: Flash=lightning,
    // STD=hourglass, Agent=brain. confirm cycles tier (all tiers).
    const bool tier_sel = status_edit.active && status_edit.selected == 0;
    switch (ai.tier) {
        case wqn::AiTier::kFlash: DrawLightningIcon(6, kAiToggleY, tier_sel); break;
        case wqn::AiTier::kAgent: DrawBrainIcon(6, kAiToggleY, tier_sel); break;
        case wqn::AiTier::kStd:
        default:                  DrawHourglassIcon(6, kAiToggleY, tier_sel); break;
    }

    // [shell] Toggle zone (STD/Pro only): thinking(1)/TTS(2)/expand(3)/trash(4).
    // Flash hides the whole zone (only the tier icon, button 0, is editable).
    // The Agent tier has its own cluster (page_ai_agent.cpp), so it never
    // reaches this branch -- RenderAiToEpd dispatches it away first.
    if (ai.tier == wqn::AiTier::kStd) {
        if (status_edit.active) {
            // Zone rect hugs the four tightly-packed toggle icons.
            DrawRect(kAiToggleX - kAiToggleZonePad, kAiToggleY - kAiToggleZonePad,
                     kAiToggleZoneWStd, 16 + 2 * kAiToggleZonePad);
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
    auto lines = WrapAiNativeText(text, max_w_px, AiNativeFont(), max_lines);
    if (lines.empty()) {
        return {""};
    }
    return lines;
}

int DrawUserPill(int x, int y, int max_w, const std::vector<std::string>& lines, int bottom)
{
    const int row_h = kAiLineH;
    const int total_h = static_cast<int>(lines.size()) * row_h + 4;
    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(bottom, y + total_h);
    if (visible_top < visible_bottom) {
        DrawRoundedRect(x, visible_top, max_w, visible_bottom - visible_top, 4);
        FillRect(x + 1, visible_top + 1, max_w - 2, visible_bottom - visible_top - 2, false);
    }
    const int first = std::max(0, AiCeilDivide(kAiViewportY - y - 4, row_h));
    const int end = std::min<int>(lines.size(), std::max(0, (bottom - y - 4) / row_h));
    for (int r = first; r < end; ++r)
        AiNativeText(x + 8, y + r * row_h + 4, max_w - 16, lines[r]);
    return total_h;
}

int DrawAssistantBlock(int x, int y, int max_w, const std::vector<MdLine>& md_lines, int bottom)
{
    const int content_w = max_w - kAiAssistantLeftBorder - 6;
    const int row_h = kAiLineH;
    const int total_h = std::max<int>(1, md_lines.size()) * row_h + 6;
    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(bottom, y + total_h);
    if (visible_top < visible_bottom)
        FillRect(x, visible_top, kAiAssistantLeftBorder, visible_bottom - visible_top, true);
    const int text_x = x + kAiAssistantLeftBorder + 6;
    const int first = std::max(0, AiCeilDivide(kAiViewportY - y - 4, row_h));
    const int end = std::min<int>(md_lines.size(), std::max(0, (bottom - y - 4) / row_h));
    for (int r = first; r < end; ++r)
        DrawMarkdownLine(md_lines[r], text_x, y + r * row_h + 4, content_w, row_h, AiNativeFont());
    const int rule_y = y + total_h - 2;
    if (rule_y >= kAiViewportY && rule_y < bottom) DrawHorizontalLine(x, rule_y, max_w);
    return total_h;
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

int DrawThinkingBlock(int x, int y, const std::vector<std::string>& lines, int bottom)
{
    const int row_count = static_cast<int>(lines.size());
    const int row_h = kAiLineH;
    const int total_h = row_count * row_h + 6;

    const int visible_top = std::max(kAiViewportY, y);
    const int visible_bottom = std::min(bottom, y + total_h);

    if (visible_top < visible_bottom) {
        DrawVerticalLine(x, visible_top, visible_bottom - visible_top);
    }
    const int text_x = x + 22;
    const int first = std::max(0, AiCeilDivide(kAiViewportY - y - 4, row_h));
    const int end = std::min(row_count, std::max(0, (bottom - y - 4) / row_h));
    for (int r = first; r < end; ++r) {
        const int line_y = y + r * row_h + 4;
        if (line_y >= kAiViewportY && line_y + row_h <= bottom) {
            if (r == 0) {
                DrawWqnBitmapAsset(x + 6, line_y + 2, m01_ai_thinking_marker_12_asset, true);
            }
            AiNativeText(text_x, line_y, kAiAssistantW - 28, lines[r]);
        }
    }
    return total_h;
}

int DrawToolBlock(int x, int y, int max_w, const wqn::ChatMessageSnapshot& tool, bool expanded, int bottom)
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
    const int visible_bottom = std::min(bottom, y + expected_h);

    if (visible_top < visible_bottom) {
        DrawVerticalLine(x, visible_top, visible_bottom - visible_top);
        DrawVerticalLine(x + max_w - 1, visible_top, visible_bottom - visible_top);
    }
    if (y >= kAiViewportY && y < bottom) {
        DrawHorizontalLine(x, y, max_w);
    }
    const int bottom_y = y + expected_h - 1;
    if (bottom_y >= kAiViewportY && bottom_y < bottom) {
        DrawHorizontalLine(x, bottom_y, max_w);
    }

    // Header row.
    if (y + 4 >= kAiViewportY && y + 4 + row_h <= bottom) {
        DrawWqnBitmapAsset(x + 4, y + 6,
                           expanded ? m06_chevron_down_12_asset : m05_chevron_right_12_asset, true);
        DrawWqnBitmapAsset(x + 18, y + 6, m02_ai_tool_running_12_asset, true);
        AiNativeText(x + 34, y + 4, max_w - 38, header);
    }

    if (expanded) {
        int cur_y = y + 4 + row_h;
        // Args row.
        for (const auto& l : args_lines) {
            if (cur_y >= kAiViewportY && cur_y + row_h <= bottom) {
                AiNativeText(x + 8, cur_y, max_w - 16, l);
            }
            cur_y += row_h;
        }

        // Result row.
        for (size_t i = 0; i < result_lines.size(); ++i) {
            if (cur_y >= kAiViewportY && cur_y + row_h <= bottom) {
                if (i == 0) {
                    DrawWqnBitmapAsset(
                        x + 8, cur_y + 2,
                        tool.tool_ok ? m03_status_success_12_asset : m04_status_error_12_asset, true);
                }
                AiNativeText(x + 24, cur_y, max_w - 34, result_lines[i]);
            }
            cur_y += row_h;
        }

        // Elapsed row.
        if (cur_y >= kAiViewportY && cur_y + row_h <= bottom) {
            AiNativeText(x + 8, cur_y, max_w - 16, elapsed);
        }
    }

    return expected_h;
}

// Single source of truth for the AI history vertical geometry. Both the
// renderer (RenderAiHistoryViewport) and the input-path scroll bounds
// (GetAiScrollBounds) MUST consume this pass, or the scroll clamp desyncs
// from what is actually drawn (same invariant as kMarkdownWidthDense on the
// note/problem pages). The struct itself lives in ui_internal.h so the
// turn-jump helper can share it without a second definition.
std::shared_ptr<const AiHistoryLayout> GetAiHistoryLayout(
    const std::shared_ptr<const wqn::AiHistorySnapshot>& snapshot, bool expand_content)
{
    struct Entry {
        std::shared_ptr<const wqn::AiHistorySnapshot> source;
        bool expanded;
        std::shared_ptr<const AiHistoryLayout> layout;
    };
    static std::mutex mutex;
    static std::vector<Entry> cache;
    const auto find = [&]() {
        return std::find_if(cache.begin(), cache.end(), [&](const Entry& entry) {
            return entry.source == snapshot && entry.expanded == expand_content;
        });
    };
    {
        const std::lock_guard<std::mutex> guard(mutex);
        const auto found = find();
        if (found != cache.end()) return found->layout;
    }
    auto next = std::make_shared<AiHistoryLayout>();
    if (snapshot) {
        const auto& messages = snapshot->messages;
        next->heights.assign(messages.size(), 0);
        next->virtual_tops.assign(messages.size(), 0);
        next->text.resize(messages.size());
        int y = 0;
        static const std::string empty_body = "-";
        for (size_t i = 0; i < messages.size(); ++i) {
            const auto& msg = messages[i];
            const std::string& body = msg.text.empty() ? empty_body : msg.text;
            int height = 0;
            if (msg.kind == wqn::ChatMessageKind::kUser) {
                next->text[i] = GetAiTextLayout(msg.id, body, AiTextKind::kPlain,
                    kAiUserPillMaxW - 16, AiNativeFont());
                height = static_cast<int>(next->text[i]->plain.size()) * kAiLineH + 4;
                next->anchor_top = y;
            } else if (msg.kind == wqn::ChatMessageKind::kAssistant) {
                next->text[i] = GetAiTextLayout(msg.id, body, AiTextKind::kMarkdown,
                    kAiAssistantW - kAiAssistantLeftBorder - 6, AiNativeFont());
                height = std::max<int>(1, next->text[i]->markdown.size()) * kAiLineH + 6;
            } else if (msg.kind == wqn::ChatMessageKind::kThinking) {
                next->text[i] = GetAiTextLayout(msg.id, body, AiTextKind::kPlain,
                    kAiAssistantW - 28, AiNativeFont(), expand_content ? 1024 : 1);
                height = static_cast<int>(next->text[i]->plain.size()) * kAiLineH + 6;
            } else height = ToolBlockHeight(msg, kAiAssistantW, expand_content);
            next->heights[i] = height;
            next->virtual_tops[i] = y;
            y += height + kAiLineGap;
        }
        next->total_content_h = std::max(0, y - (messages.empty() ? 0 : kAiLineGap));
    }
    const std::lock_guard<std::mutex> guard(mutex);
    const auto existing = find();
    if (existing != cache.end()) return existing->layout;
    // Three immutable history snapshots suffice for active UI/EPD consumers.
    // Eviction never invalidates a reader's shared reference.
    if (cache.size() >= 3) cache.erase(cache.begin());
    cache.push_back({snapshot, expand_content, next});
    return next;
}

void RenderAiHistoryViewport(const wqn::AiSessionState& ai,
                            const std::shared_ptr<const wqn::AiHistorySnapshot>& snapshot,
                            int32_t scroll_offset_lines,
                            int bottom_pad)
{
    // Clear the viewport region explicitly (partial-region contract).
    DrawHistoryClear(kAiViewportY, wqn::kEpdHeight);

    const int line_h = kAiLineH;
    static const std::vector<wqn::ChatMessageSnapshot> kEmpty;
    const auto& messages = snapshot ? snapshot->messages : kEmpty;
    if (messages.empty() && ai.status == wqn::AiSessionStatus::kIdle) {
        // [agent] This branch is shared with the STD/Flash tiers, and that is
        // how the Agent tier came to be told "长按确认键开始语音提问" -- the STD
        // tier's gesture, offered by a tier whose unit of input is a task
        // rather than a question. Its own empty state says what is actually
        // true instead. Gesture instructions have left this tier entirely; the
        // manual owns them.
        const std::string hint = ai.tier == wqn::AiTier::kAgent
            ? std::string("这个任务还没有记录")
            : std::string("长按确认键开始语音提问");
        const auto lines = WrapForViewport(hint, kAiAssistantW - kAiAssistantLeftBorder - 6, 4);
        int y = kAiViewportY + 30;
        for (const auto& l : lines) {
            AiNativeText(kAiHistoryLeftPad + kAiAssistantLeftBorder + 6, y,
                         kAiAssistantW - kAiAssistantLeftBorder - 6, l);
            y += line_h;
        }
        return;
    }

    // 1. Lay out all messages through the shared geometry pass. It is the
    // single source of truth for GetAiScrollBounds as well: the input-path
    // scroll clamp MUST agree with what is actually drawn, or the viewport
    // desyncs from its own bounds.
    const auto layout_snapshot = GetAiHistoryLayout(snapshot, wqn::AiHistoryContentExpanded(ai));
    const AiHistoryLayout& layout = *layout_snapshot;
    const int layout_size = static_cast<int>(messages.size());

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
    // All messages are laid out in a virtual canvas from oldest to newest.
    //
    // `bottom_pad` is what this frame leaves clear at the panel's bottom: the
    // Agent tier's option bar while there is something to answer, 0 otherwise
    // (see kAiBottomBandH). It arrives as a parameter because three other
    // functions derive the scroll clamp from this same number, and the two must
    // not be allowed to answer the question differently.
    const int effective_bottom = wqn::kEpdHeight - bottom_pad;
    const int viewport_h = effective_bottom - kAiViewportY;

    const auto geometry = ComputeAiScrollGeometry(layout.total_content_h, layout.anchor_top,
        viewport_h, line_h, scroll_offset_lines);
    const int max_window_top = geometry.max_window_top;
    const int window_top = geometry.window_top;

    const bool oldest_fully_visible = (window_top == 0);

    // Draw all messages that intersect with the viewport
    for (int i = layout_size - 1; i >= 0; --i) {
        const wqn::ChatMessageSnapshot& msg = messages[i];
        const int block_top = kAiViewportY + (layout.virtual_tops[i] - window_top);

        // Clip completely above viewport
        if (block_top + layout.heights[i] <= kAiViewportY) {
            continue;
        }
        // Clip completely below viewport
        if (block_top >= effective_bottom) {
            continue;
        }

        const int x_left = kAiHistoryLeftPad;
        const auto& text = layout.text[i];
        if (msg.kind == wqn::ChatMessageKind::kUser) {
            const int actual_w = text->measured_width + 16;
            const int px_right = wqn::kEpdWidth - 8;
            const int px_left = std::max(kAiHistoryLeftPad, px_right - actual_w);
            DrawUserPill(px_left, block_top, px_right - px_left, text->plain, effective_bottom);
        } else if (msg.kind == wqn::ChatMessageKind::kAssistant) {
            DrawAssistantBlock(x_left, block_top, kAiAssistantW, text->markdown, effective_bottom);
        } else if (msg.kind == wqn::ChatMessageKind::kThinking) {
            DrawThinkingBlock(x_left, block_top, text->plain, effective_bottom);
        } else DrawToolBlock(x_left, block_top, kAiAssistantW, msg, wqn::AiHistoryContentExpanded(ai), effective_bottom);
    }

    // 3. Scroll chevrons. ▲ sits in the top-right corner when there's more
    // above; ▼ sits in the bottom-right corner when there's more below. Each
    // owns its own strip: the strip is cleared every tick before the icon is
    // drawn into it, which is what keeps a disappearing indicator from leaving a
    // phantom on the E-ink panel.
    //
    // Neither strip reserves viewport height, and ▲ never did -- it has always
    // cleared the top-right corner over whatever the first line put there. That
    // is the established design here, not an oversight. ▼ does the same now that
    // the bottom is reserved only while the Agent tier's band is up: deriving
    // its clear from the bottom pad made the clear and the chevron drift apart
    // the moment a frame reserved nothing, and a ▼ drawn outside its own clear
    // would eat the tail of the last line nobody cleared.
    //
    // [scroll-indicator] Each icon's left edge is clamped to the strip the clear
    // above reserves. Both were `kEpdWidth - count_w - 18`, which walks LEFT as
    // the hidden-line count grows: a 3-digit count puts the icon at x=358,
    // outside the [360, 400) strip just cleared, so it drew over message text
    // nobody cleared. Clamping keeps the icon inside its strip and lets the
    // count clip at the panel edge -- the icon is what says "there is more" and
    // the number is the refinement, so losing digits is the cheaper loss.
    constexpr int kIndicatorColumnLeft = wqn::kEpdWidth - 40;
    // ▼'s strip: bottom-right corner, 2 px above the panel's bottom edge. This
    // is exactly where it has always been; only its source changed.
    constexpr int kIndicatorBottomStripH = 20;
    if (!messages.empty()) {
        const bool more_above = !oldest_fully_visible;
        const bool more_below = (window_top < max_window_top);
        // Clear a right-edge indicator region; clear it before
        // drawing so a disappearing indicator does not leave a phantom.
        // The viewport was cleared above. Only reserve an indicator patch
        // when that indicator exists; unconditional clears erased the final
        // row's right-hand text even after the tail became reachable.
        if (more_below) {
            FillRect(wqn::kEpdWidth - 40, wqn::kEpdHeight - kIndicatorBottomStripH,
                     36, kIndicatorBottomStripH - 2, false);
            char count[12];
            std::snprintf(count, sizeof(count), "%ld",
                          static_cast<long>((max_window_top - window_top + line_h - 1) / line_h));
            const int count_w = wqn::MeasureTextWithFont(AiNativeFont(12), count);
            const int x = std::max<int>(kIndicatorColumnLeft,
                                        wqn::kEpdWidth - count_w - 18);
            // Bottom-right, 2 px above the panel bottom.
            DrawWqnBitmapAsset(x, wqn::kEpdHeight - 16, m06_chevron_down_12_asset, true);
            AiNativeText(x + 14, wqn::kEpdHeight - 18, wqn::kEpdWidth - x - 14, count, 12);
        }
        if (more_above) {
            FillRect(wqn::kEpdWidth - 40, kAiViewportY + 1, 36, 14, false);
            // Show the index of the oldest message that's hidden above so
            // the user has a rough sense of how many turns are above.
            char count[12];
            std::snprintf(count, sizeof(count), "%ld", static_cast<long>(AiCeilDivide(window_top, line_h)));
            const int count_w = wqn::MeasureTextWithFont(AiNativeFont(12), count);
            const int x = std::max<int>(kIndicatorColumnLeft,
                                        wqn::kEpdWidth - count_w - 18);
            DrawWqnBitmapAsset(x, kAiViewportY + 3, m07_chevron_up_12_asset, true);
            AiNativeText(x + 14, kAiViewportY + 4, wqn::kEpdWidth - x - 14, count, 12);
        }
    }

    // 4. "已最新" hint — shown briefly when the user pressed Down at the
    // bottom of the scroll. Stamped by the input handler, expires after 1s.
    if (ai.scroll_no_op_hint_ms > 0) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - ai.scroll_no_op_hint_ms <= 1000) {
            // Centre the label horizontally, sitting on the last line the
            // bottom pad leaves clear. With no pad that is the panel's last
            // line, which is where "you are at the newest" belongs.
            const char* hint = "\xe5\xb7\xb2\xe6\x9c\x80\xe6\x96\xb0";  // 已最新
            const int w = wqn::MeasureUtf8TextWidth(hint);
            const int cx = std::max(70, (wqn::kEpdWidth - w) / 2);
            DRC(cx, wqn::kEpdHeight - bottom_pad - kAiLineH, hint, true);
        }
    }
}

void GetAiScrollBounds(
    std::shared_ptr<const wqn::AiHistorySnapshot> snapshot,
    bool expand_content,
    int bottom_pad,
    int32_t* out_min_scroll,
    int32_t* out_max_scroll)
{
    if (out_min_scroll) *out_min_scroll = 0;
    if (out_max_scroll) *out_max_scroll = 0;
    if (!snapshot || snapshot->messages.empty()) return;

    // Same geometry pass as the renderer -- never re-measure here.
    const auto layout_snapshot = GetAiHistoryLayout(snapshot, expand_content);
    const AiHistoryLayout& layout = *layout_snapshot;

    const int line_h = kAiLineH;
    const int viewport_h = wqn::kEpdHeight - kAiViewportY - bottom_pad;
    const auto geometry = ComputeAiScrollGeometry(layout.total_content_h, layout.anchor_top,
        viewport_h, line_h, 0);
    const int min_scroll = geometry.min_scroll;
    const int max_scroll = geometry.max_scroll;

    if (out_min_scroll) *out_min_scroll = min_scroll;
    if (out_max_scroll) *out_max_scroll = max_scroll;
}

// [agent] Turn navigation: bring the previous/next ANSWER to the top of the
// viewport instead of scrolling by a fixed number of lines, so one keypress
// lands on a readable boundary no matter how long a reply is.
//
// The scroll model is `window_top = anchor_top - scroll * line_h`, so a target
// block top maps to `scroll = (anchor_top - top) / line_h`. Reuses
// GetAiHistoryLayout (never re-measures) and clamps through the same
// min/max the renderer uses, so a jump can never ask for a window the viewport
// cannot draw.
bool GetAiTurnJumpOffsetLines(
    std::shared_ptr<const wqn::AiHistorySnapshot> snapshot,
    bool expand_content,
    int bottom_pad,
    int32_t current_scroll,
    int direction,
    int32_t* out_scroll)
{
    if (out_scroll == nullptr || direction == 0 || !snapshot || snapshot->messages.empty()) {
        return false;
    }
    const auto layout_snapshot = GetAiHistoryLayout(snapshot, expand_content);
    const AiHistoryLayout& layout = *layout_snapshot;

    const int line_h = kAiLineH;
    const int viewport_h = wqn::kEpdHeight - kAiViewportY - bottom_pad;
    const auto geometry = ComputeAiScrollGeometry(layout.total_content_h, layout.anchor_top,
        viewport_h, line_h, current_scroll);
    const int min_scroll = geometry.min_scroll;
    const int max_scroll = geometry.max_scroll;
    const int clamped = geometry.scroll;
    const int window_top = geometry.window_top;

    // Answers are assistant blocks; tool/thinking blocks belong to the reply
    // above them and would jump to a half-rendered position.
    const int n = static_cast<int>(snapshot->messages.size());
    int target = -1;
    if (direction < 0) {
        for (int i = n - 1; i >= 0; --i) {
            if (snapshot->messages[i].kind != wqn::ChatMessageKind::kAssistant) {
                continue;
            }
            if (layout.virtual_tops[i] < window_top) {
                target = layout.virtual_tops[i];
                break;
            }
        }
    } else {
        for (int i = 0; i < n; ++i) {
            if (snapshot->messages[i].kind != wqn::ChatMessageKind::kAssistant) {
                continue;
            }
            if (layout.virtual_tops[i] > window_top) {
                target = layout.virtual_tops[i];
                break;
            }
        }
    }
    if (target < 0) {
        return false;  // no answer in that direction
    }

    int32_t next = AiOffsetForBlockTop(layout.anchor_top, target, line_h, geometry);
    if (next < min_scroll) {
        next = min_scroll;
    }
    if (next > max_scroll) {
        next = max_scroll;
    }
    if (next == clamped) {
        return false;  // already there
    }
    *out_scroll = next;
    return true;
}

// [follow] The newest answer's first line as a scroll offset. The per-tick
// auto-follow parks the viewport here when the answer body starts, so the user
// reads the reply from its beginning instead of chasing the streaming tail.
// Reuses GetAiHistoryLayout (never re-measures) and clamps through the same
// min/max the renderer uses.
//
// Returns false when the newest history entry is not a non-empty assistant body
// -- the answer has not started, or the turn is still thinking / running tools.
// The caller then keeps following the live tail instead.
bool GetAiNewestAnswerTopOffsetLines(
    std::shared_ptr<const wqn::AiHistorySnapshot> snapshot,
    bool expand_content,
    int bottom_pad,
    int32_t* out_scroll)
{
    if (out_scroll == nullptr || !snapshot || snapshot->messages.empty()) {
        return false;
    }
    const wqn::ChatMessageSnapshot& newest = snapshot->messages.back();
    if (newest.kind != wqn::ChatMessageKind::kAssistant || newest.text.empty()) {
        return false;
    }
    const auto layout_snapshot = GetAiHistoryLayout(snapshot, expand_content);
    const AiHistoryLayout& layout = *layout_snapshot;

    const int line_h = kAiLineH;
    const int viewport_h = wqn::kEpdHeight - kAiViewportY - bottom_pad;
    const auto geometry = ComputeAiScrollGeometry(layout.total_content_h, layout.anchor_top,
        viewport_h, line_h, 0);
    const int min_scroll = geometry.min_scroll;
    const int max_scroll = geometry.max_scroll;
    int32_t next = AiOffsetForBlockTop(layout.anchor_top, layout.virtual_tops.back(), line_h, geometry);
    if (next < min_scroll) {
        next = min_scroll;
    }
    if (next > max_scroll) {
        next = max_scroll;
    }
    *out_scroll = next;
    return true;
}

esp_err_t RenderAiToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::AiSessionState& ai = frame.ai;
    // [agent] The Agent tier owns the whole frame: its status bar carries a
    // different toggle cluster and its bottom band carries the option bar, so
    // it cannot share this function's body. It clears the framebuffer itself.
    if (ai.tier == wqn::AiTier::kAgent) {
        return RenderAgentAiToEpd(frame, schedule);
    }
    wqn::ClearEpdFramebuffer(true);

    // Section 1: status bar (no battery, merges toast state).
    DrawAiStatusBar(ai, frame.home, frame.status_edit);

    // Section 2: viewport (no bottom input bar, no separate toast strip).
    // 0: this tier has no bottom band, so it reserves nothing. It used to
    // reserve 22 px here, which was the Agent tier's option bar charged to a
    // page that never drew one -- a permanent blank strip at the bottom of every
    // STD/Flash transcript.
    RenderAiHistoryViewport(ai, frame.ai_history, ai.scroll_offset_lines,
                            ai.tier == wqn::AiTier::kStd ? StdAiBottomReserve(ai) : 0);
    if (ai.tier == wqn::AiTier::kStd)
        DrawStdAiInteraction(frame);

    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
