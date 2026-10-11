// Shared Markdown row renderer: draws one laid-out MdLine at (x, y) inside a
// content_w-wide, line_h-tall row slot. All style is adornment (bars, bullets,
// boxes, rules) over the single fixed-width face -- no font variation.
//
// Consumers: note body (x=10, w=370, line_h=20), problem faces (same), AI
// assistant blocks (x=16, w=378, line_h=18). The MdLine array must come from
// LayoutMarkdown at the SAME content width, or decorations and table geometry
// land on the wrong pixels. Decoration vertical offsets are relative to the
// ~16px glyph box, so they hold for both 18px and 20px row heights.

#include "ui_internal.h"

#include "markdown_layout.h"

namespace device_ui_internal {

esp_err_t DrawMarkdownLine(const MdLine& line, int x, int y, int content_w, int line_h)
{
    if (line.kind == MdLineKind::kRule) {
        DrawHorizontalLine(x, y + line_h / 2, content_w);
        return ESP_OK;
    }

    if (line.kind == MdLineKind::kTableRow) {
        if (line.border_top) {
            DrawHorizontalLine(x, y, line.table_width);
        }
        for (const int16_t sx : line.col_seps) {
            DrawVerticalLine(x + sx, y, line_h);
        }
        esp_err_t rc = ESP_OK;
        for (const MdTableCell& cell : line.cells) {
            if (cell.text.empty()) continue;
            const esp_err_t r =
                DrawClippedText(x + cell.x, y + 2, line.table_width - cell.x, cell.text);
            if (r != ESP_OK) rc = r;
        }
        if (line.border_bottom) {
            DrawHorizontalLine(x, y + line_h - 1, line.table_width);
        }
        return rc;
    }

    // kText: left rails (quote bars / code rule), then bullet, text, inline
    // decorations, and an optional heading underline.
    for (int d = 0; d < line.quote_depth; ++d) {
        FillRect(x + d * 6, y, 2, line_h, true);
    }
    if (line.code) {
        FillRect(x + 2, y, 1, line_h, true);
    }
    if (line.bullet == MdBullet::kFilledSquare) {
        FillRect(x + line.bullet_x, y + 6, 5, 5, true);
    } else if (line.bullet == MdBullet::kHollowSquare) {
        DrawRect(x + line.bullet_x, y + 6, 5, 5);
    }

    const int text_x = x + line.indent_px;
    esp_err_t rc = ESP_OK;
    if (!line.text.empty()) {
        rc = DrawClippedText(text_x, y, content_w - line.indent_px, line.text);
    }
    for (const MdDecoration& deco : line.decorations) {
        const int dx = text_x + deco.x;
        switch (deco.kind) {
            case MdDecoKind::kUnderline:
                DrawHorizontalLine(dx, y + 17, deco.w);
                break;
            case MdDecoKind::kStrike:
                DrawHorizontalLine(dx, y + 8, deco.w);
                break;
            case MdDecoKind::kCodeBox:
                DrawRect(dx - 1, y - 1, deco.w + 2, 18);
                break;
        }
    }
    if (line.rule_below) {
        DrawHorizontalLine(text_x, y + 17, content_w - line.indent_px);
    }
    return rc;
}

}  // namespace device_ui_internal
