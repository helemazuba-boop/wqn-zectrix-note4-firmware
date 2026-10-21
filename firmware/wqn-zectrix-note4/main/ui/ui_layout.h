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
// Home and Settings use the same full-width divider as Todo and AI.
constexpr int kStatusBarHeight = 28;            // visual height of the top status band
constexpr int kStatusBarDividerY = 27;          // y of the separator line under the status bar
constexpr int kStatusBarTitleX = 10;            // left x of the status bar title (= kMarginX)
constexpr int kStatusBarTitleY = 6;             // font line-box top, not alphabetic baseline
constexpr int kStatusBarIconY = 6;
constexpr int kStatusBarRightInset = 10;        // right inset for status text (x = kScreenWidth - inset)

// ---- Content margins (two tiers, by page density) ----
// 标准页(卡片/对话框/配置): 视觉宽松.
// 密度页(AI/note/problem/word 正文): 信息密度优先, 边距收窄到 6.
constexpr int kMarginX = 10;                    // 全局标准水平边距(卡片/对话框页)
constexpr int kMarginDense = 6;                 // 密度页边距 (note/problem/word/AI 正文)
constexpr int kContentWidth = kScreenWidth - 2 * kMarginX;  // 380 (标准页内容宽)
constexpr int kContentWidthDense = kScreenWidth - 2 * kMarginDense;  // 388 (密度页内容宽)
constexpr int kMarkdownWidthDense = kContentWidthDense - 14;  // 374 (密度页 Markdown 绘制宽度)
constexpr int kBottomHintY = 278;               // 底部提示/脚注行 y

// ---- Home, reference A: ink on paper ----
constexpr int kHomeLeft = 16;
constexpr int kHomeWidth = 368;
constexpr int kHomeClockY = 40;
constexpr int kHomeDateX = 280;
constexpr int kHomeAiCaptionY = 112;
constexpr int kHomeAiTitleY = 141;
constexpr int kHomeAiActionY = 184;
constexpr int kHomeStudyCaptionY = 220;
constexpr int kHomeStudyTitleY = 240;
constexpr int kHomeFooterY = 276;
constexpr int kHomeAiDividerY = 102;
constexpr int kHomeStudyDividerY = 210;

// ---- Word: Home's paper sections, native text, no persistent help footer ----
constexpr int kWordLeft = kHomeLeft;
constexpr int kWordWidth = kHomeWidth;
constexpr int kWordRight = kWordLeft + kWordWidth;
constexpr int kWordBottom = kScreenHeight - 8;
constexpr int kWordMetaY = 35;
constexpr int kWordHeadlineY = 62;
constexpr int kWordHomeTitleY = 36;
constexpr int kWordHomeSummaryY = 70;
constexpr int kWordHomeNoticeY = 91;
constexpr int kWordMenuTop = 116;
constexpr int kWordMenuPitch = 56;
constexpr int kWordChoiceTop = 178;
constexpr int kWordChoicePitch = 50;

// ---- Settings: approved B layout, native type, no persistent help footer ----
constexpr int kSettingsListX = kMarginX;
constexpr int kSettingsDetailX = kMarginX;
constexpr int kSettingsRight = kScreenWidth - kMarginX;
constexpr int kSettingsTextX = kSettingsListX + 30;
constexpr int kSettingsOptionTextX = kSettingsDetailX + 30;
constexpr int kSettingsHeaderDividerY = kStatusBarDividerY;
constexpr int kSettingsHeaderTextBaseline = kStatusBarTitleY + 14;
constexpr int kSettingsTitleBaseline = 61;
constexpr int kSettingsSubtitleBaseline = 84;
constexpr int kSettingsContentDividerY = 95;
constexpr int kSettingsRowTop = 96;
constexpr int kSettingsRowPitch = 36;
constexpr int kSettingsOptionTop = 111;
constexpr int kSettingsOptionPitch = 31;
constexpr int kSettingsDetailWidth = kContentWidth;

// ---- Provisioning: approved About identity, no status bar or help footer ----
constexpr int kProvisionLeft = 24;
constexpr int kProvisionRight = 376;
constexpr int kProvisionWidth = kProvisionRight - kProvisionLeft;
constexpr int kProvisionBrandY = 17;
constexpr int kProvisionBrandBaseline = 45;
constexpr int kProvisionModelBaseline = 89;
constexpr int kProvisionStatusBaseline = 86;
constexpr int kProvisionDividerY = 101;
constexpr int kProvisionQrColumnWidth = 166;
constexpr int kProvisionQrSecondX = 210;
constexpr int kProvisionQrLabelBaseline = 122;
constexpr int kProvisionQrSlotY = 127;
constexpr int kProvisionQrSlotSize = 123;
constexpr int kProvisionQrCaptionBaseline = 267;
constexpr int kProvisionQrDividerX = 199;
constexpr int kProvisionQrDividerY = 114;
constexpr int kProvisionQrDividerHeight = 150;
constexpr int kProvisionStateTitleBaseline = 150;
constexpr int kProvisionStateRowBaseline = 190;
constexpr int kProvisionStateRowPitch = 32;
constexpr int kProvisionStateNoteBaseline = 258;

// Agent picker follows the approved Editorial reference, with four real rows.
constexpr int kAgentPickerLeft = 13;
constexpr int kAgentPickerWidth = kScreenWidth - 2 * kAgentPickerLeft;
constexpr int kAgentPickerKickerY = 38;
constexpr int kAgentPickerTitleY = 53;
constexpr int kAgentPickerDividerY = 90;
constexpr int kAgentPickerListY = 96;
constexpr int kAgentPickerRowStep = 46;

// ---- Todo: approved Editorial sheet, four double-line rows ----
constexpr int kTodoLeft = 14;
constexpr int kTodoWidth = kScreenWidth - 2 * kTodoLeft;
constexpr int kTodoMetaY = 35;
constexpr int kTodoTitleY = 55;
constexpr int kTodoDividerY = 96;
constexpr int kTodoListTop = 97;
constexpr int kTodoRowPitch = 46;
constexpr int kTodoScrollX = kScreenWidth - 6;

// ---- Notes: approved four-row list, shared with the Agent picker ----
constexpr int kNoteListX = kAgentPickerLeft;
constexpr int kNoteListWidth = kAgentPickerWidth;
constexpr int kNoteListKickerY = kAgentPickerKickerY;
constexpr int kNoteListTitleY = kAgentPickerTitleY;
constexpr int kNoteListDividerY = kAgentPickerDividerY;
constexpr int kNoteListTop = kAgentPickerListY;
constexpr int kNoteListPitch = kAgentPickerRowStep;
constexpr int kNoteBodyX = 8;
constexpr int kNoteBodyWidth = 378;
constexpr int kNoteBodyTop = 32;
constexpr int kNoteBodyBottom = kScreenHeight - 4;
constexpr int kNoteBodyLineHeight = 20;

// ---- Problem review: native Note geometry, dedicated four-action menu ----
constexpr int kProblemVerdictSubtitleY = 86;
constexpr int kProblemVerdictDividerY = 112;
constexpr int kProblemVerdictTop = 122;
constexpr int kProblemVerdictPitch = 41;

// ---- Global page navigation ----
constexpr int kNavigationX = 8;
constexpr int kNavigationY = 214;
constexpr int kNavigationWidth = 384;
constexpr int kNavigationHeight = 78;
constexpr int kNavigationRadius = 6;
constexpr int kNavigationItemX = 20;
constexpr int kNavigationItemY = 222;
constexpr int kNavigationItemWidth = 48;
constexpr int kNavigationItemHeight = 60;
constexpr int kNavigationItemStep = 52;
constexpr int kNavigationIconY = 228;
constexpr int kNavigationLabelY = 260;
constexpr int kNavigationCurrentY = 285;

// ---- AI page ---------------------------------------------------------------
// [agent] Shared by page_ai.cpp (STD/Pro + Flash tiers) and page_ai_agent.cpp
// (Agent tier): the Agent tier renders through the same viewport, so the two
// files MUST agree on where the status bar ends and where the bottom band
// begins. Duplicating them locally would let one file drift from the other.

constexpr int kAiStatusBarY = 0;
constexpr int kAiStatusBarH = kStatusBarHeight;  // 28, divider on kStatusBarDividerY
constexpr int kAiViewportY = kAiStatusBarY + kAiStatusBarH;  // 27
constexpr int kAiLineH = kCjkLineHeight;                     // 18
constexpr int kAiLineGap = 4;
constexpr int kAiAssistantLeftBorder = 4;
constexpr int kAiHistoryLeftPad = kMarginDense;                        // 6
constexpr int kAiHistoryRightPad = kMarginDense;                       // 6
constexpr int kAiHistoryRightEdge = kScreenWidth - kAiHistoryRightPad;  // 394
constexpr int kAiHistoryUsableW = kAiHistoryRightEdge - kAiHistoryLeftPad;  // 388
constexpr int kAiUserPillMaxW = kAiHistoryUsableW * 78 / 100;              // ~302
constexpr int kAiAssistantW = kAiHistoryUsableW;
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
