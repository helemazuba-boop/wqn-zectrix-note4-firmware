// Device UI layout tokens - single source of truth for screen geometry.
// Replaces scattered magic numbers across main/ui/page_*.cpp.
// See ESP32DOC/wqn-cloud-relay/docs/13-ui-design-language.md §13.4.1.
//
// Pure constants only (no externs), safe to include from any translation unit
// that already pulls in display_service.h.

#pragma once

#include "display_service.h"  // wqn::kEpdWidth / kEpdHeight

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

// ---- Content margins ----
constexpr int kMarginX = 10;                    // global horizontal content margin
constexpr int kEdgeFlushX = 6;                  // AI assistant role-bar: flush to bezel shadow (NOT general-purpose)
constexpr int kContentTopY = 35;                // first content row below status bar (divider 27 + 8px gap)
constexpr int kContentWidth = kScreenWidth - 2 * kMarginX;  // 380
constexpr int kBottomHintY = 278;               // y of bottom hint / footer line

// ---- Selection style (focus decoration, not interaction timing) ----
//
// The enum only describes HOW a focused control is decorated visually.
// Whether an input fires immediately or persists is the interaction model's
// concern and MUST NOT be encoded here. The choice between styles is by
// control shape + e-paper flicker cost:
//   kInvert              — high-salience focus for small, compact, operable
//                          targets (status-bar editable controls, time config
//                          fields, action buttons, dictionary lookup choices)
//   kInnerBorder         — low-flicker focus for square rows / grid cells /
//                          list options (2px-inset double border)
//   kRoundedInnerBorder  — low-flicker focus for rounded cards (outer r6 +
//                          selected r4 concentric). Card shape owns the round.
enum class SelectionStyle {
    kNone,
    kInvert,
    kInnerBorder,
    kRoundedInnerBorder,
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
