// Markdown subset -> flat MdLine rows. See markdown_layout.h for the contract.
//
// Scope (UI-beautify stage 1, no font variation): headings, unordered/ordered
// lists (nested), blockquotes, fenced code, horizontal rules, GFM pipe tables
// (<=4 cols native, wider downgraded to text), and the inline set
// bold/italic/strike/inline-code/link/image. Emphasis with no ink cost (bold,
// italic) is stripped; the rest is drawn with adornments resolved to pixels
// here so the renderer stays trivial.

#include "markdown_layout.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "display_service.h"  // wqn::MeasureUtf8TextWidth

namespace device_ui_internal {

namespace {

// Layout budget. Kept intentionally small and integer so both the render pass
// and the scroll-clamp count pass produce identical geometry.
constexpr int kMaxLines = 4096;      // matches the old WrapUtf8 cap
constexpr int kListStepPx = 14;      // indent per nesting level
constexpr int kListBasePx = 4;       // first-level text indent (before marker)
constexpr int kBulletAreaPx = 12;    // space reserved for an unordered marker
constexpr int kQuoteStepPx = 6;      // width of one quote bar + its gap
constexpr int kQuoteTextPad = 4;     // gap between the last bar and the text
constexpr int kCodeIndentPx = 8;     // text indent inside a code block
constexpr int kTableCellPad = 3;     // px padding on each side of a cell
constexpr int kTableMinColPx = 28;   // floor so scaled columns stay legible
constexpr int kHeadingMarkPx = 12;   // marker area for H3+ headings

int Utf8Len(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// Width of a single codepoint via the shared fixed-width metric, no heap.
int CpWidth(const std::string& s, size_t pos, int nbytes)
{
    char buf[8];
    if (nbytes > 7) nbytes = 7;
    std::memcpy(buf, s.data() + pos, static_cast<size_t>(nbytes));
    buf[nbytes] = '\0';
    return wqn::MeasureUtf8TextWidth(buf);
}

int MeasureRange(const std::string& s, size_t a, size_t b)
{
    if (b <= a) return 0;
    return wqn::MeasureUtf8TextWidth(s.substr(a, b - a).c_str());
}

std::string RStrip(const std::string& s)
{
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(0, e);
}

std::string LStrip(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

size_t LeadingSpaces(const std::string& s)
{
    size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) ++n;
    return n;
}

// ---- Inline parsing: raw -> plain text + decoration spans -------------------

struct InlineSpan {
    size_t start = 0;
    size_t end = 0;
    MdDecoKind kind = MdDecoKind::kUnderline;
};

struct StyledText {
    std::string plain;
    std::vector<InlineSpan> spans;
};

size_t FindSub(const std::string& s, size_t from, const char* sub, size_t sublen)
{
    if (from >= s.size()) return std::string::npos;
    return s.find(sub, from, sublen);
}

void ParseInlineInto(const std::string& raw, uint8_t opts, StyledText* out);

// Handle a paired emphasis/strike marker of `mlen` chars starting at i. On a
// matched close, recurses over the inner text and (for strike) records a span.
// Returns the index just past the close, or npos if unmatched.
size_t TryPaired(const std::string& raw, size_t i, const char* mark, size_t mlen,
                 bool strike, uint8_t opts, StyledText* out)
{
    const size_t close = FindSub(raw, i + mlen, mark, mlen);
    if (close == std::string::npos || close == i + mlen) {
        return std::string::npos;  // unmatched or empty span -> treat literally
    }
    const std::string inner = raw.substr(i + mlen, close - (i + mlen));
    const size_t span_start = out->plain.size();
    ParseInlineInto(inner, opts, out);
    if (strike) {
        out->spans.push_back({span_start, out->plain.size(), MdDecoKind::kStrike});
    }
    return close + mlen;
}

void ParseInlineInto(const std::string& raw, uint8_t opts, StyledText* out)
{
    size_t i = 0;
    while (i < raw.size()) {
        const char c = raw[i];

        if (c == '\\' && i + 1 < raw.size()) {  // escape: next char is literal
            out->plain.push_back(raw[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`') {  // inline code (literal inner, boxed)
            const size_t close = raw.find('`', i + 1);
            if (close != std::string::npos && close > i + 1) {
                const size_t s = out->plain.size();
                out->plain.append(raw, i + 1, close - (i + 1));
                out->spans.push_back({s, out->plain.size(), MdDecoKind::kCodeBox});
                i = close + 1;
                continue;
            }
        }
        if (c == '!' && i + 1 < raw.size() && raw[i + 1] == '[') {  // image
            const size_t rb = raw.find(']', i + 2);
            if (rb != std::string::npos && rb + 1 < raw.size() && raw[rb + 1] == '(') {
                const size_t rp = raw.find(')', rb + 2);
                if (rp != std::string::npos) {
                    const std::string alt = raw.substr(i + 2, rb - (i + 2));
                    out->plain.append(alt.empty() ? "[图片]" : "[图:" + alt + "]");
                    i = rp + 1;
                    continue;
                }
            }
        }
        if (c == '[') {  // link: keep text (underlined), drop url
            const size_t rb = raw.find(']', i + 1);
            if (rb != std::string::npos && rb + 1 < raw.size() && raw[rb + 1] == '(') {
                const size_t rp = raw.find(')', rb + 2);
                if (rp != std::string::npos) {
                    const std::string text = raw.substr(i + 1, rb - (i + 1));
                    const size_t s = out->plain.size();
                    ParseInlineInto(text, opts, out);
                    out->spans.push_back({s, out->plain.size(), MdDecoKind::kUnderline});
                    i = rp + 1;
                    continue;
                }
            }
        }
        if (c == '~' && i + 1 < raw.size() && raw[i + 1] == '~') {  // strike
            const size_t next = TryPaired(raw, i, "~~", 2, true, opts, out);
            if (next != std::string::npos) { i = next; continue; }
        }
        if (c == '*' || c == '_') {  // emphasis: try ***/___, then **/__, then */_
            // A run must be matched longest-first: probing "**" inside
            // "***bold***" leaves the inner lone marker unpaired and it leaks
            // into the output as a literal asterisk.
            size_t run = 1;
            while (i + run < raw.size() && raw[i + run] == c && run < 3) ++run;
            // Math-protection mode: a lone * / _ is multiplication or a
            // subscript, never emphasis.
            const size_t min_run = (opts & kMdNoSingleEmphasis) ? 2 : 1;
            size_t next = std::string::npos;
            for (size_t m = run; m >= min_run && next == std::string::npos; --m) {
                char mk[4] = {c, c, c, '\0'};
                mk[m] = '\0';
                next = TryPaired(raw, i, mk, m, false, opts, out);
            }
            if (next != std::string::npos) {
                i = next;
                continue;
            }
            out->plain.append(run, c);  // no closing run anywhere: literal
            i += run;
            continue;
        }

        out->plain.push_back(c);
        ++i;
    }
}

StyledText ParseInline(const std::string& raw, uint8_t opts)
{
    StyledText st;
    st.plain.reserve(raw.size());
    ParseInlineInto(raw, opts, &st);
    return st;
}

// ---- Styled wrapping: StyledText -> visual lines with pixel decorations -----

struct WrappedLine {
    std::string text;
    std::vector<MdDecoration> decorations;
};

std::vector<WrappedLine> WrapStyled(const StyledText& st, int avail)
{
    std::vector<WrappedLine> out;
    if (avail < 8) avail = 8;
    const std::string& p = st.plain;
    size_t line_start = 0;
    int line_w = 0;

    auto flush = [&](size_t end) {
        WrappedLine wl;
        size_t e = end;
        while (e > line_start && p[e - 1] == ' ') --e;  // rtrim soft wrap gap
        wl.text = p.substr(line_start, e - line_start);
        for (const InlineSpan& sp : st.spans) {
            const size_t s2 = std::max(sp.start, line_start);
            const size_t e2 = std::min(sp.end, e);
            if (s2 < e2) {
                MdDecoration d;
                d.x = static_cast<int16_t>(MeasureRange(p, line_start, s2));
                d.w = static_cast<int16_t>(MeasureRange(p, s2, e2));
                d.kind = sp.kind;
                wl.decorations.push_back(d);
            }
        }
        out.push_back(std::move(wl));
    };

    size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '\n') {
            flush(i);
            line_start = i + 1;
            line_w = 0;
            ++i;
            continue;
        }
        int n = Utf8Len(static_cast<unsigned char>(p[i]));
        if (i + static_cast<size_t>(n) > p.size()) n = 1;
        const int w = CpWidth(p, i, n);
        if (line_w > 0 && line_w + w > avail) {
            flush(i);
            line_start = i;
            line_w = 0;
        }
        line_w += w;
        i += static_cast<size_t>(n);
    }
    if (line_start < p.size()) {
        flush(p.size());
    }
    return out;
}

// ---- Block classifiers ------------------------------------------------------

bool IsFenceMarker(const std::string& trimmed, char* fence_ch)
{
    if (trimmed.size() < 3) return false;
    const char c = trimmed[0];
    if (c != '`' && c != '~') return false;
    if (trimmed[1] != c || trimmed[2] != c) return false;
    *fence_ch = c;
    return true;
}

bool IsFenceClose(const std::string& trimmed, char fence_ch)
{
    size_t n = 0;
    while (n < trimmed.size() && trimmed[n] == fence_ch) ++n;
    if (n < 3) return false;
    return RStrip(trimmed.substr(n)).empty();
}

bool IsHorizontalRule(const std::string& trimmed)
{
    if (trimmed.empty()) return false;
    const char c = trimmed[0];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (const char ch : trimmed) {
        if (ch == c) ++count;
        else if (ch != ' ' && ch != '\t') return false;
    }
    return count >= 3;
}

bool IsHeading(const std::string& trimmed, int* level, std::string* text)
{
    size_t n = 0;
    while (n < trimmed.size() && trimmed[n] == '#') ++n;
    if (n == 0 || n > 6) return false;
    if (n >= trimmed.size() || (trimmed[n] != ' ' && trimmed[n] != '\t')) return false;
    *level = static_cast<int>(n);
    *text = LStrip(trimmed.substr(n));
    return true;
}

// Leading "> " (optionally nested) -> depth + remainder.
bool ParseQuote(const std::string& raw, int* depth, std::string* inner)
{
    size_t i = LeadingSpaces(raw);
    if (i >= raw.size() || raw[i] != '>') return false;
    int d = 0;
    while (i < raw.size() && raw[i] == '>') {
        ++d;
        ++i;
        if (i < raw.size() && raw[i] == ' ') ++i;
    }
    *depth = d;
    *inner = raw.substr(i);
    return true;
}

bool ParseUnordered(const std::string& raw, int* level, std::string* content)
{
    const size_t sp = LeadingSpaces(raw);
    if (sp >= raw.size()) return false;
    const char m = raw[sp];
    if (m != '-' && m != '*' && m != '+') return false;
    if (sp + 1 >= raw.size() || (raw[sp + 1] != ' ' && raw[sp + 1] != '\t')) return false;
    *level = std::min<int>(4, static_cast<int>(sp) / 2);
    *content = LStrip(raw.substr(sp + 1));
    return true;
}

bool ParseOrdered(const std::string& raw, int* level, std::string* marker, std::string* content)
{
    const size_t sp = LeadingSpaces(raw);
    size_t i = sp;
    while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) ++i;
    if (i == sp || i > sp + 9) return false;
    if (i >= raw.size() || (raw[i] != '.' && raw[i] != ')')) return false;
    if (i + 1 >= raw.size() || (raw[i + 1] != ' ' && raw[i + 1] != '\t')) return false;
    *level = std::min<int>(4, static_cast<int>(sp) / 2);
    *marker = raw.substr(sp, i - sp) + ". ";
    *content = LStrip(raw.substr(i + 1));
    return true;
}

// ---- Table parsing ----------------------------------------------------------

std::vector<std::string> SplitTableCells(const std::string& raw)
{
    std::string s = RStrip(LStrip(raw));
    if (!s.empty() && s.front() == '|') s.erase(s.begin());
    if (!s.empty() && s.back() == '|') s.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    bool in_code = false;  // backtick span: pipes inside are literal, not columns
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|' && !in_code) {
            cur.push_back('|');
            ++i;
            continue;
        }
        if (s[i] == '`') {
            in_code = !in_code;
            cur.push_back('`');
            continue;
        }
        if (s[i] == '|' && !in_code) {
            cells.push_back(RStrip(LStrip(cur)));
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    cells.push_back(RStrip(LStrip(cur)));
    return cells;
}

bool IsDelimiterRow(const std::string& raw)
{
    if (raw.find('|') == std::string::npos) return false;
    const std::vector<std::string> cells = SplitTableCells(raw);
    bool any = false;
    for (const std::string& cell : cells) {
        if (cell.empty()) continue;
        any = true;
        for (const char c : cell) {
            if (c != '-' && c != ':' && c != ' ') return false;
        }
        if (cell.find('-') == std::string::npos) return false;
    }
    return any;
}

}  // namespace

std::vector<MdLine> LayoutMarkdown(const std::string& body, int content_width_px, uint8_t opts)
{
    std::vector<MdLine> out;
    const int width = std::max(40, content_width_px);
    std::vector<std::string> src;
    {
        std::string cur;
        for (const char c : body) {
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                src.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        src.push_back(cur);
    }

    bool in_fence = false;
    char fence_ch = '`';
    bool last_blank = true;  // suppress a leading blank row

    auto push = [&](MdLine&& line) {
        if (static_cast<int>(out.size()) < kMaxLines) out.push_back(std::move(line));
    };
    auto push_blank = [&]() {
        if (last_blank) return;
        MdLine ml;
        push(std::move(ml));
        last_blank = true;
    };
    auto push_text = [&](const WrappedLine& wl, int indent) {
        MdLine ml;
        ml.text = wl.text;
        ml.indent_px = static_cast<int16_t>(indent);
        ml.decorations = wl.decorations;
        push(std::move(ml));
        last_blank = false;
    };

    size_t i = 0;
    while (i < src.size() && static_cast<int>(out.size()) < kMaxLines) {
        const std::string& raw = src[i];
        const std::string trimmed = LStrip(raw);

        if (in_fence) {
            if (IsFenceClose(trimmed, fence_ch)) {
                in_fence = false;
                ++i;
                continue;
            }
            StyledText st;
            st.plain = raw;  // literal, no inline parsing inside code
            for (const WrappedLine& wl : WrapStyled(st, width - kCodeIndentPx)) {
                MdLine ml;
                ml.text = wl.text;
                ml.indent_px = static_cast<int16_t>(kCodeIndentPx);
                ml.code = true;
                push(std::move(ml));
            }
            if (raw.empty()) {  // preserve blank lines inside the block
                MdLine ml;
                ml.indent_px = static_cast<int16_t>(kCodeIndentPx);
                ml.code = true;
                push(std::move(ml));
            }
            last_blank = false;
            ++i;
            continue;
        }

        if (IsFenceMarker(trimmed, &fence_ch)) {
            in_fence = true;
            ++i;
            continue;
        }

        if (trimmed.empty()) {
            push_blank();
            ++i;
            continue;
        }

        if (IsHorizontalRule(trimmed)) {
            MdLine ml;
            ml.kind = MdLineKind::kRule;
            push(std::move(ml));
            last_blank = false;
            ++i;
            continue;
        }

        // Table: a header row followed by a delimiter row.
        if (raw.find('|') != std::string::npos && i + 1 < src.size() &&
            IsDelimiterRow(src[i + 1])) {
            std::vector<std::vector<std::string>> rows;
            rows.push_back(SplitTableCells(raw));
            size_t j = i + 2;
            while (j < src.size() && !LStrip(src[j]).empty() &&
                   src[j].find('|') != std::string::npos) {
                rows.push_back(SplitTableCells(src[j]));
                ++j;
            }
            size_t ncols = 0;
            for (const auto& r : rows) ncols = std::max(ncols, r.size());

            // Only >4 columns are un-renderable as a grid on the 400px panel;
            // those degrade to plain "a | b | c" text rows. Every narrower
            // table is drawn as a real grid -- cells wrap and columns scale
            // down proportionally to fit -- never demoted to source-like text.
            if (ncols == 0 || ncols > 4) {
                for (size_t r = 0; r < rows.size(); ++r) {
                    std::string joined;
                    for (size_t c = 0; c < rows[r].size(); ++c) {
                        if (c) joined += " | ";
                        joined += rows[r][c];
                    }
                    for (const WrappedLine& wl : WrapStyled(ParseInline(joined, opts), width)) {
                        push_text(wl, 0);
                    }
                }
                i = j;
                continue;
            }

            // Column widths: natural single-line widths, scaled down to fit the
            // available width when the widest row overflows the panel.
            std::vector<int> colw(ncols, 6);
            for (const auto& r : rows) {
                for (size_t c = 0; c < r.size(); ++c) {
                    colw[c] = std::max(colw[c], wqn::MeasureUtf8TextWidth(r[c].c_str()));
                }
            }
            const int overhead =
                static_cast<int>(ncols + 1) + static_cast<int>(ncols) * 2 * kTableCellPad;
            const int avail_text =
                std::max(static_cast<int>(ncols) * kTableMinColPx, width - overhead);
            int sum_natural = 0;
            for (const int w : colw) sum_natural += w;
            if (sum_natural > avail_text) {
                for (size_t c = 0; c < ncols; ++c) {
                    colw[c] = std::max(kTableMinColPx, colw[c] * avail_text / sum_natural);
                }
            }
            std::vector<int> sep(ncols + 1, 0);
            std::vector<int> cell_x(ncols, 0);
            for (size_t c = 0; c < ncols; ++c) {
                cell_x[c] = sep[c] + 1 + kTableCellPad;
                sep[c + 1] = cell_x[c] + colw[c] + kTableCellPad;
            }
            const int table_w = sep[ncols] + 1;

            for (size_t r = 0; r < rows.size(); ++r) {
                std::vector<std::vector<std::string>> cell_lines(ncols);
                size_t row_h = 1;
                for (size_t c = 0; c < ncols; ++c) {
                    const std::string cell = c < rows[r].size() ? rows[r][c] : std::string();
                    for (const WrappedLine& wl : WrapStyled(ParseInline(cell, opts), colw[c])) {
                        cell_lines[c].push_back(wl.text);
                    }
                    row_h = std::max(row_h, cell_lines[c].size());
                }
                for (size_t vl = 0; vl < row_h; ++vl) {
                    MdLine ml;
                    ml.kind = MdLineKind::kTableRow;
                    ml.table_width = static_cast<int16_t>(table_w);
                    for (size_t c = 0; c <= ncols; ++c) {
                        ml.col_seps.push_back(static_cast<int16_t>(sep[c]));
                    }
                    for (size_t c = 0; c < ncols; ++c) {
                        MdTableCell tc;
                        tc.text = vl < cell_lines[c].size() ? cell_lines[c][vl] : std::string();
                        tc.x = static_cast<int16_t>(cell_x[c]);
                        ml.cells.push_back(std::move(tc));
                    }
                    // Full grid: the table's top edge once, then a horizontal
                    // rule under every row's last visual line (header divider,
                    // every inter-row boundary, and the table's bottom edge).
                    ml.border_top = (r == 0 && vl == 0);
                    ml.border_bottom = (vl + 1 == row_h);
                    push(std::move(ml));
                }
            }
            last_blank = false;
            i = j;
            continue;
        }

        int level = 0;
        std::string htext;
        if (IsHeading(trimmed, &level, &htext)) {
            push_blank();  // headings breathe
            const int indent = level >= 3 ? kHeadingMarkPx : 0;
            const std::vector<WrappedLine> wl = WrapStyled(ParseInline(htext, opts), width - indent);
            for (size_t k = 0; k < wl.size(); ++k) {
                MdLine ml;
                ml.text = wl[k].text;
                ml.indent_px = static_cast<int16_t>(indent);
                ml.decorations = wl[k].decorations;
                if (level <= 2 && k + 1 == wl.size()) {
                    ml.rule_below = true;  // section underline
                } else if (level >= 3 && k == 0) {
                    ml.bullet = MdBullet::kFilledSquare;
                    ml.bullet_x = static_cast<int16_t>(indent - kHeadingMarkPx + 2);
                }
                push(std::move(ml));
            }
            last_blank = false;
            ++i;
            continue;
        }

        int depth = 0;
        std::string inner;
        if (ParseQuote(raw, &depth, &inner)) {
            const int indent = depth * kQuoteStepPx + kQuoteTextPad;
            const std::vector<WrappedLine> wl = WrapStyled(ParseInline(LStrip(inner), opts), width - indent);
            if (wl.empty()) {  // "> " with no text still shows the bar
                MdLine ml;
                ml.indent_px = static_cast<int16_t>(indent);
                ml.quote_depth = static_cast<uint8_t>(depth);
                push(std::move(ml));
            }
            for (const WrappedLine& line : wl) {
                MdLine ml;
                ml.text = line.text;
                ml.indent_px = static_cast<int16_t>(indent);
                ml.quote_depth = static_cast<uint8_t>(depth);
                ml.decorations = line.decorations;
                push(std::move(ml));
            }
            last_blank = false;
            ++i;
            continue;
        }

        int list_level = 0;
        std::string content;
        std::string marker;
        if (ParseUnordered(raw, &list_level, &content)) {
            const int indent = kListBasePx + list_level * kListStepPx + kBulletAreaPx;
            const std::vector<WrappedLine> wl = WrapStyled(ParseInline(content, opts), width - indent);
            for (size_t k = 0; k < wl.size(); ++k) {
                MdLine ml;
                ml.text = wl[k].text;
                ml.indent_px = static_cast<int16_t>(indent);
                ml.decorations = wl[k].decorations;
                if (k == 0) {
                    ml.bullet = (list_level % 2 == 0) ? MdBullet::kFilledSquare
                                                      : MdBullet::kHollowSquare;
                    ml.bullet_x = static_cast<int16_t>(indent - kBulletAreaPx + 2);
                }
                push(std::move(ml));
            }
            last_blank = false;
            ++i;
            continue;
        }
        if (ParseOrdered(raw, &list_level, &marker, &content)) {
            const int indent = kListBasePx + list_level * kListStepPx + kBulletAreaPx;
            StyledText st = ParseInline(content, opts);
            st.plain.insert(0, marker);  // "N. " rides in the text; spans shift
            for (InlineSpan& sp : st.spans) {
                sp.start += marker.size();
                sp.end += marker.size();
            }
            const int first_indent = kListBasePx + list_level * kListStepPx;
            const std::vector<WrappedLine> wl = WrapStyled(st, width - first_indent);
            for (size_t k = 0; k < wl.size(); ++k) {
                push_text(wl[k], k == 0 ? first_indent : indent);
            }
            ++i;
            continue;
        }

        for (const WrappedLine& wl : WrapStyled(ParseInline(LStrip(raw), opts), width)) {
            push_text(wl, 0);
        }
        ++i;
    }

    while (!out.empty() && out.back().kind == MdLineKind::kText && out.back().text.empty() &&
           out.back().decorations.empty()) {
        out.pop_back();  // drop trailing blank rows
    }
    return out;
}

std::size_t CountMarkdownLines(const std::string& body, int content_width_px, uint8_t opts)
{
    return LayoutMarkdown(body, content_width_px, opts).size();
}

}  // namespace device_ui_internal
