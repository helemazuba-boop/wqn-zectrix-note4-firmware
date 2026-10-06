// Device UI layout tokens - single source of truth for screen geometry.
// Replaces scattered magic numbers across main/ui/page_*.cpp.
// See ESP32DOC/wqn-cloud-relay/docs/13-ui-design-language.md §13.4.1.
//
// Pure constants only (no externs), safe to include from any translation unit
// that already pulls in display_service.h.

#pragma once

#include "display_service.h"  // wqn::kEpdWidth / kEpdHeight
#include "typography.h"       // kCjkLineHeight

namespace device_ui_internal {

// ---- Screen (re-exported for ui/ consumers) ----
constexpr int kScreenWidth = wqn::kEpdWidth;    // 400
constexpr int kScreenHeight = wqn::kEpdHeight;  // 300

// ---- Status bar ----
// Replaces the dead kStatusBarRect={0,0,400,30} (height 30 was never used).
// Real divider line is at y=27 across all pages (AI's y=26 is the 1px outlier
// to be fixed in L2).
constexpr int kStatusBarHeight = 28;            // visual height of the top status band
constexpr int kStatusBarDividerY = 27;          // y of the separator line under the status bar
constexpr int kStatusBarTitleX = 10;            // left x of the status bar title (= kMarginX)
constexpr int kStatusBarTitleY = 6;             // text baseline y of the status bar title
constexpr int kStatusBarRightInset = 10;        // right inset for status text (x = kScreenWidth - inset)

// ---- Content margins (two tiers, by page density) ----
// 标准页(卡片/对话框/配置): 视觉宽松.
// 密度页(AI/note/problem/word 正文): 信息密度优先, 边距收窄到 6.
constexpr int kMarginX = 10;                    // 全局标准水平边距(卡片/对话框页)
constexpr int kMarginPage = kMarginX;           // 标准页边距别名 (=10)
constexpr int kMarginDense = 6;                 // 密度页边距 (note/problem/word/AI 正文)
constexpr int kEdgeFlushX = 6;                  // [legacy] AI 助手 role-bar 贴边框阴影专用, == kMarginDense
constexpr int kContentTopY = 35;                // 状态栏下第一内容行 (divider 27 + 8px gap)
constexpr int kContentWidth = kScreenWidth - 2 * kMarginX;  // 380 (标准页内容宽)
constexpr int kContentWidthDense = kScreenWidth - 2 * kMarginDense;  // 388 (密度页内容宽)
constexpr int kMarkdownWidthDense = kContentWidthDense - 14;  // 374 (密度页 Markdown 绘制宽度)
constexpr int kBottomHintY = 278;               // 底部提示/脚注行 y

// ---- 组件间距 (gutter) ----
constexpr int kGutterCard = 12;                 // 卡片垂直间距
constexpr int kGutterRow = 8;                   // 列表行垂直间距

// ---- AI page ---------------------------------------------------------------
// [agent] Shared by page_ai.cpp (STD/Pro + Flash tiers) and page_ai_agent.cpp
// (Agent tier): the Agent tier renders through the same viewport, so the two
// files MUST agree on where the status bar ends and where the bottom band
// begins. Duplicating them locally would let one file drift from the other.

constexpr int kAiStatusBarY = 0;
constexpr int kAiStatusBarH = kStatusBarHeight;  // 28, divider on kStatusBarDividerY
constexpr int kAiViewportY = kAiStatusBarY + kAiStatusBarH;  // 27
constexpr int kAiViewportH = kScreenHeight - kAiViewportY;   // 273
constexpr int kAiLineH = kCjkLineHeight;                     // 18
constexpr int kAiLineGap = 4;
constexpr int kAiAssistantLeftBorder = 4;
constexpr int kAiHistoryLeftPad = kMarginDense;                        // 6
constexpr int kAiHistoryRightPad = kMarginDense;                       // 6
constexpr int kAiHistoryRightEdge = kScreenWidth - kAiHistoryRightPad;  // 394
constexpr int kAiHistoryUsableW = kAiHistoryRightEdge - kAiHistoryLeftPad;  // 388
constexpr int kAiUserPillMaxW = kAiHistoryUsableW * 78 / 100;              // ~302
constexpr int kAiAssistantW = kAiHistoryUsableW;
constexpr int kAiRowTopPad = 2;
// [band] The bottom band of the AI transcript. The Agent tier's option bar --
// asks, confirmations, the question counter -- is exactly this tall.
//
// It is reserved ONLY while it is actually up. It used to be reserved
// unconditionally on every tier, which was right while the band always drew
// something; once the band became interactive-only (4169b1f) the reservation
// turned into a permanent blank strip that no answer could use. With the band
// clear the transcript runs to the panel's last pixel, and the ▼ indicator
// lives in its own cleared strip down there -- the same way ▲ already does at
// the top, over content it never had to reserve away.
//
// Nothing else in the transcript reserves the bottom: the STD/Flash tier has no
// bottom band at all, so it reserves 0 too.
constexpr int kAiBottomBandH = 22;
// [toggle-cluster] Status-bar toggle cluster geometry. tier occupies x=6..22
// (16px); the cluster starts at kAiToggleX and each icon is kAiToggleStep apart
// (18 = 16px icon + 2px gap). Applies to every tier -- STD/Pro uses four slots
// for thinking/TTS/expand/trash, the Agent tier five for
// session/turn-up/turn-down/detail/trash.
constexpr int kAiToggleX = 30;
constexpr int kAiToggleY = 5;
constexpr int kAiToggleStep = 18;
constexpr int kAiToggleZonePad = 2;   // pad around the cluster for the edit-mode rect
// Slots in the edit-mode toggle list, index 0 = tier icon. The cluster width is
// NOT tier-independent any more: the follow toggle moved to the settings page,
// so STD/Pro keeps four cluster slots and only the Agent tier fills five.
constexpr int kAiToggleClusterCountStd = 4;
constexpr int kAiToggleClusterCountAgent = 5;
// The edit-mode zone rect must HUG the icons: slot i starts at
// kAiToggleX + i*step, so the last icon ends at kAiToggleX + (slots-1)*step +
// 16, plus one pad on each side. (The pre-tier formula overshot by a whole
// step: 4 icons drew the rect out to x=120 while the glyphs ended at x=100.)
// The Agent cluster's five glyphs reach x=118, so the Agent session title's
// clamp starts at x=122 for exactly this reason.
constexpr int kAiToggleZoneWidth(int cluster_slots) {
    return kAiToggleStep * (cluster_slots - 1) + 16 + 2 * kAiToggleZonePad;
}
constexpr int kAiToggleZoneWStd = kAiToggleZoneWidth(kAiToggleClusterCountStd);      // 74
constexpr int kAiToggleZoneWAgent = kAiToggleZoneWidth(kAiToggleClusterCountAgent);  // 92

// ---- Selection style (focus decoration, not interaction timing) ----
//
// The enum only describes HOW a focused control is decorated visually.
// Whether an input fires immediately or persists is the interaction model's
// concern and MUST NOT be encoded here. The choice between styles is by
// 交互语义 + e-paper flicker cost (v2 设计语言, 见对话决议):
//   kInvert              — 将执行动作的瞬时焦点: 反白(黑底白字). 用于动作按钮、
//                          正在编辑的配置字段、对话框选项、自评选项、查词字母.
//   kRoundedInnerBorder  — 持久浏览选中的低闪烁焦点: 圆角双边框(外 r6 + 内缩2px r4).
//                          用于所有卡片式行 (todo/word/home/settings 行/卡片).
//   kRowFill             — 密度长列表的行选中: 圆角反白实块 + 纸色字. 醒目、移动
//                          浏览成本低. 用于 note/problem/settings 词库等 30px 级行.
// [retired] kInnerBorder(方角双边框) 语义二义(既表行选中又被对话框当装饰), 已退役.
enum class SelectionStyle {
    kNone,
    kInvert,
    kRoundedInnerBorder,
    kRowFill,
};

// ---- Geometry rect (owned here so decoration/UI layers share one type) ----
struct UiRect {
    int x;
    int y;
    int width;
    int height;
    const char* name;
};

}  // namespace device_ui_internal
