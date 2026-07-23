// UI widget decoration layer: focus decoration primitives that sit above the
// raw graphics primitives (graphics.cpp) and below the page renderers.
//
// This layer ONLY draws decorations (focus borders / reverse-fill / chips /
// the footer-hint slot). Page renderers remain responsible for content
// layout inside a card/row; they hand the geometry to these helpers to draw
// the focus decoration so the visual language is consistent across pages.
//
// See ui_layout.h SelectionStyle for the style taxonomy. Style owns the
// corner shape (kRoundedInnerBorder => rounded), per the agreed design:
// round-corner capability folds into the style enum, not a separate shape arg.

#pragma once

#include <string>

#include "ui/assets/wqn_bitmap_asset.h"
#include "ui_layout.h"

namespace device_ui_internal {

// Rounded-card outline radius used by both DrawSelectionDecoration(kRoundedInnerBorder)
// and page callers that need to draw a plain (unselected) rounded outline matching
// the card language. Exposed here (not ui_layout.h) because it is a decoration
// detail, but pages draw unselected card outlines against it for visual parity
// with the selected focus decoration.
constexpr int kRoundedOuterRadius = 6;

// Rounded outline radius for small badge/chip containers (task tags, settings
// tags). Smaller than the card radius because chips are short (18-26px tall)
// and r6 would eat too much of the corners.
constexpr int kChipRadius = 4;

// Draw the focus decoration for a control occupying `rect`, in the given
// style. kNone draws nothing. For kInnerBorder the decoration is the control
// outline + a 2px-inset inner outline (the concentric double border). For
// kRoundedInnerBorder the same but rounded (outer r6, inner r4). For kInvert
// the rect is reverse-filled (ink background); the caller then draws its
// content in paper color on top.
//
// The rect is the FULL control bounds (including the outer outline). Content
// passed to DrawClippedText/DrawCenteredText by the page must be inset to
// match the chosen style.
void DrawSelectionDecoration(const UiRect& rect, SelectionStyle style);

// Inline overload taking separate coords for call-site convenience.
void DrawSelectionDecoration(int x, int y, int width, int height, SelectionStyle style);

// Draw a compact operable status-bar icon. When `focused` the icon glyph is
// drawn in paper color on an ink-filled square background (reverse-fill
// focus, the AI status-bar editable-control language); otherwise the glyph is
// drawn in ink with no background. The background square is the asset's own
// width/height plus `padding` on every side, so callers are not pinned to a
// 16x16 cell -- pass padding=0 to match the legacy 16x16 AI status-bar tiles.
void DrawSelectableIcon(int x, int y, const WqnBitmapAsset& asset, bool focused, int padding = 0);

// Draw a non-selectable status chip (rounded badge). No `selected` parameter
// by design -- a chip is a read-only status badge, not a focusable control.
// `text` is centered and truncated to fit the chip width.
void DrawStatusChip(int x, int y, int width, int height, int radius, const std::string& text);

// Draw the page footer hint line at the shared footer y (kBottomHintY) across
// the standard content width. Replaces per-page DrawClippedText(..., 12, 278, 376, ...)
// footers so the hint slot geometry is owned in one place.
esp_err_t DrawPageFooterHint(const std::string& text);

}  // namespace device_ui_internal
