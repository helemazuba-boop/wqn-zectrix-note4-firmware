// Markdown -> flat "layout line" model for the note body viewport.
//
// The note body renderer (ui/page_note.cpp) and the scroll clamp
// (note_app.cpp NoteBodyMaxScroll) both treat the body as a flat array of
// fixed-height (kBodyLineH) rows. This module turns a Markdown subset into
// exactly that array so both sides stay in lockstep: LayoutMarkdown() is the
// single source of truth for "how many rows does this note occupy" and "what
// does row N contain".
//
// Deliberate constraints (UI-beautify stage 1):
//   * No new fonts. Every glyph is the existing 16px CJK / 6px ASCII face, so
//     bold/italic/heading levels are conveyed with adornments (rules, bullets,
//     boxes, bars, indentation), never font weight or size.
//   * Pure CPU, no I/O, no globals. Safe to run on the UI task at note-open.
//
// Widths are measured with wqn::MeasureUtf8TextWidth (the fixed-width metric),
// so a decoration's pixel extent is resolved here once and the renderer just
// draws it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace device_ui_internal {

// Inline run drawn on top of a text line (the text itself is already the
// marker-stripped content; these only add ink around/under a byte span that
// has been resolved to pixels relative to the line's text origin).
enum class MdDecoKind : uint8_t {
    kUnderline,  // [link](url) -> link text underlined, url dropped
    kStrike,     // ~~text~~    -> line through the middle
    kCodeBox,    // `code`      -> 1px rectangle around the run
};

struct MdDecoration {
    int16_t x = 0;  // px offset from the line's text origin
    int16_t w = 0;  // px width of the run
    MdDecoKind kind = MdDecoKind::kUnderline;
};

// Marker drawn at the head of a list item's first visual line. Drawn as a
// small square (font-independent) because the slim CJK font maps U+2022/U+00B7
// to '.' (see NormalizeCodepointForDisplay), so a glyph bullet is unreliable.
enum class MdBullet : uint8_t {
    kNone = 0,
    kFilledSquare,  // top-level unordered item
    kHollowSquare,  // nested unordered item
};

enum class MdLineKind : uint8_t {
    kText,       // normal text row (paragraph / heading / list / quote / code)
    kRule,       // horizontal rule: a single full-width line, no text
    kTableRow,   // one visual row of a pipe table (cells + separators)
};

// One rendered cell fragment inside a table row (already clipped to its column).
struct MdTableCell {
    std::string text;
    int16_t x = 0;  // px offset from the content origin (kNoteMarginX + 2)
};

// A single fixed-height row in the note body viewport.
struct MdLine {
    MdLineKind kind = MdLineKind::kText;

    // kText payload.
    std::string text;                     // marker-stripped, ready to draw
    int16_t indent_px = 0;                // text x offset from content origin
    uint8_t quote_depth = 0;              // number of left quote bars
    bool code = false;                    // code-block row (left rule + shade)
    bool rule_below = false;              // heading underline under this row
    MdBullet bullet = MdBullet::kNone;    // list marker on this row
    int16_t bullet_x = 0;                 // px offset of the bullet square
    std::vector<MdDecoration> decorations;

    // kTableRow payload.
    std::vector<MdTableCell> cells;
    std::vector<int16_t> col_seps;        // px offsets of vertical separators
    int16_t table_width = 0;              // px width of the table box
    bool border_top = false;              // horizontal rule at the row's top
    bool border_bottom = false;           // horizontal rule at the row's bottom
};

// Layout option flags (bitmask).
//
// kMdNoSingleEmphasis: single '*' / '_' runs stay literal instead of pairing
// as italic. Math-heavy plain text (problem bodies come from HtmlToPlainText)
// writes "x*y" and "x_1"; stripping those markers corrupts the formula.
// Double markers (**, __), strike, code, links and all block elements are
// unaffected -- they do not occur in math plain text.
constexpr uint8_t kMdNoSingleEmphasis = 0x01;

// Lay a Markdown body out into fixed-height rows for a content_width_px-wide
// viewport. Deterministic for a given (body, width, opts): the render path and
// the scroll-clamp count path MUST pass the same width and opts so their row
// counts agree.
std::vector<MdLine> LayoutMarkdown(
    const std::string& body, int content_width_px, uint8_t opts = 0);

// Row count only (scroll clamp). Equivalent to LayoutMarkdown(...).size().
std::size_t CountMarkdownLines(
    const std::string& body, int content_width_px, uint8_t opts = 0);

}  // namespace device_ui_internal
