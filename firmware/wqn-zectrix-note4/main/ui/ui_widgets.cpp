// UI widget decoration layer implementation. See ui_widgets.h for the design.
//
// Sits above the raw graphics primitives (graphics.cpp): FillRect / DrawRect /
// DrawRoundedRect / DrawWqnBitmapAsset / DrawCenteredText, and the ClearRect /
// typography / text helpers. This layer only draws decorations; page renderers
// own content layout.

#include "ui_widgets.h"

#include "ui_internal.h"  // UiRect, graphics primitives, DrawCenteredText, kTag

namespace device_ui_internal {

// Offset (inset) used for the inner of the concentric double border. Kept here
// rather than in ui_layout.h because it is an implementation detail of the
// decoration, not page geometry.
constexpr int kInnerBorderInset = 2;
constexpr int kRoundedInnerRadius = 4;
constexpr int kStatusChipCanonicalHeight = 26;  // the product's standard chip height (word home card chips)

void DrawSelectionDecoration(int x, int y, int width, int height, SelectionStyle style)
{
    switch (style) {
        case SelectionStyle::kNone:
            return;
        case SelectionStyle::kInvert:
            FillRect(x, y, width, height, true);
            return;
        case SelectionStyle::kInnerBorder:
            DrawRect(x, y, width, height);
            DrawRect(x + kInnerBorderInset, y + kInnerBorderInset,
                     width - 2 * kInnerBorderInset, height - 2 * kInnerBorderInset);
            return;
        case SelectionStyle::kRoundedInnerBorder:
            DrawRoundedRect(x, y, width, height, kRoundedOuterRadius);
            DrawRoundedRect(x + kInnerBorderInset, y + kInnerBorderInset,
                            width - 2 * kInnerBorderInset, height - 2 * kInnerBorderInset,
                            kRoundedInnerRadius);
            return;
    }
}

void DrawSelectionDecoration(const UiRect& rect, SelectionStyle style)
{
    DrawSelectionDecoration(rect.x, rect.y, rect.width, rect.height, style);
}

void DrawSelectableIcon(int x, int y, const WqnBitmapAsset& asset, bool focused, int padding)
{
    // In focused state the ink-filled square sits BEHIND the glyph; the glyph
    // is then drawn in paper color so it reads on the ink background. In the
    // unfocused state only the ink glyph is drawn (no background). The
    // background square dimensions are asset.width/height + 2*padding so a
    // nonzero padding grows the reverse-fill pad without changing the glyph
    // origin. The legacy 16x16 AI status-bar tiles pass padding=0 and draw the
    // glyph at (x, y) aligned to the square origin.
    if (focused) {
        const int bg_w = static_cast<int>(asset.width) + 2 * padding;
        const int bg_h = static_cast<int>(asset.height) + 2 * padding;
        FillRect(x, y, bg_w, bg_h, true);
        DrawWqnBitmapAsset(x + padding, y + padding, asset, false);  // paper glyph
    } else {
        DrawWqnBitmapAsset(x + padding, y + padding, asset, true);   // ink glyph
    }
}

void DrawStatusChip(int x, int y, int width, int height, int radius, const std::string& text)
{
    DrawRoundedRect(x, y, width, height, radius);
    // Single-line text visually centered in the short chip. The legacy word-card
    // chip used DrawCenteredText(x, y+5, w) for a 26px-tall chip and that read
    // correctly, so replicate that nudge for the canonical 26px chip height; for
    // other heights fall back to a half-height-minus-3 baseline estimate.
    const int text_y = (height == kStatusChipCanonicalHeight) ? y + 5 : y + (height - kCjkFontHeight) / 2 + 3;
    DrawCenteredText(x, text_y, width, text);
}

esp_err_t DrawPageFooterHint(const std::string& text)
{
    // Footer hint slot: y = kBottomHintY, content margins per ui_layout.h.
    return DrawClippedText(kMarginX, kBottomHintY, kContentWidth - 4, text);
}

}  // namespace device_ui_internal
