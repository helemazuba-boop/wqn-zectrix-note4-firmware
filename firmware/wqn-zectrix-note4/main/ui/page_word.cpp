// Word pages share Home's paper sections and native type. Learning input stays
// in word_app: there is no reader, text paging, or new button gesture here.
#include "ui_internal.h"
#include "display_service.h"
#include "ui_fonts.h"
#include "word_fonts.h"
#include "word_app.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace device_ui_internal {
namespace {

struct WordCharacter {
    std::string text;
    uint32_t codepoint;
};

WordCharacter WordNextCharacter(const std::string& text, size_t* position)
{
    const size_t first = *position;
    const auto c = static_cast<unsigned char>(text[first]);
    size_t count = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
        (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    if (first + count > text.size()) count = 1;
    uint32_t cp = c & (count == 1 ? 0x7f : count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
    for (size_t i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[first + i]);
        if ((next & 0xc0) != 0x80) { count = 1; cp = '?'; break; }
        cp = (cp << 6) | (next & 0x3f);
    }
    *position += count;
    return {count == 1 && c >= 0x80 ? "?" : text.substr(first, count), cp};
}

const lv_font_t* WordPrimaryFont(int size)
{
    switch (size) {
        case 12: return &wqn_ui_sans_12;
        case 16: return &wqn_word_regular_16;
        case 20: return &wqn_ui_sans_20;
        case 24: return &wqn_word_bold_24;
        case 32: return &wqn_word_bold_32;
        case 48: return &wqn_ui_latin_48;
        default: return wqn::PrimaryUiFont();
    }
}

const lv_font_t* WordFallbackFont(int size)
{
    switch (size) {
        case 12: return &wqn_ui_sans_12;
        case 20: return &wqn_word_regular_20;
        case 24: case 32: return &wqn_ui_sans_24;
        default: return wqn::PrimaryUiFont();
    }
}

std::pair<const lv_font_t*, std::string> WordGlyph(int size, const std::string& text)
{
    const auto* primary = WordPrimaryFont(size);
    if (wqn::MeasureTextWithFont(primary, text.c_str()) > 0) return {primary, text};
    const auto* fallback = WordFallbackFont(size);
    if (wqn::MeasureTextWithFont(fallback, text.c_str()) > 0) return {fallback, text};
    // Unsupported user text stays visible rather than silently losing letters.
    return wqn::MeasureTextWithFont(fallback, "□") > 0
        ? std::make_pair(fallback, std::string("□"))
        : std::make_pair(primary, std::string("?"));
}

int WordMeasure(int size, const std::string& text)
{
    int width = 0;
    for (size_t i = 0; i < text.size();) {
        const auto character = WordNextCharacter(text, &i);
        const auto glyph = WordGlyph(size, character.text);
        width += wqn::MeasureTextWithFont(glyph.first, glyph.second.c_str());
    }
    return width;
}

std::string WordOneLine(std::string text)
{
    std::replace_if(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }, ' ');
    return text;
}

void WordRemoveLast(std::string* text)
{
    if (text->empty()) return;
    size_t end = text->size() - 1;
    while (end && (static_cast<unsigned char>((*text)[end]) & 0xc0) == 0x80) --end;
    text->resize(end);
}

std::string WordClip(const std::string& value, int size, int width)
{
    const std::string text = WordOneLine(value);
    if (width <= 0) return {};
    if (WordMeasure(size, text) <= width) return text;
    const int ellipsis = WordMeasure(size, "...");
    if (ellipsis > width) return {};
    std::string output;
    int used = 0;
    for (size_t i = 0; i < text.size();) {
        const auto c = WordNextCharacter(text, &i);
        const int advance = WordMeasure(size, c.text);
        if (used + advance + ellipsis > width) break;
        output += c.text; used += advance;
    }
    return output + "...";
}

std::string WordEllipsis(std::string text, int size, int width)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    while (!text.empty() && WordMeasure(size, text + "...") > width) WordRemoveLast(&text);
    return WordMeasure(size, "...") <= width ? text + "..." : std::string();
}

void WordDrawLine(int x, int top, int size, const std::string& text, bool ink = true)
{
    for (size_t i = 0; i < text.size();) {
        const auto c = WordNextCharacter(text, &i);
        const auto glyph = WordGlyph(size, c.text);
        // Align the native glyph baselines when Latin/IPA and CJK share a run.
        wqn::DrawTextWithFont(x, top + size - glyph.first->line_height + glyph.first->base_line,
            glyph.first, glyph.second.c_str(), ink);
        x += wqn::MeasureTextWithFont(glyph.first, glyph.second.c_str());
    }
}

void WordText(int x, int top, const std::string& text, int size = 16,
              int width = kWordWidth, bool right = false, bool ink = true)
{
    const auto clipped = WordClip(text, size, width);
    WordDrawLine(right ? x - WordMeasure(size, clipped) : x, top, size, clipped, ink);
}

struct WordWrapped {
    std::vector<std::string> lines;
    bool truncated = false;
};

WordWrapped WordWrap(const std::string& text, int size, int width, size_t max_lines)
{
    WordWrapped result;
    if (width <= 0 || !max_lines) { result.truncated = !text.empty(); return result; }
    size_t position = 0, break_input = 0, break_output = std::string::npos;
    int used = 0;
    std::string line;
    auto flush = [&]() {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        result.lines.push_back(line); line.clear(); used = 0; break_output = std::string::npos;
    };
    while (position < text.size()) {
        const size_t start = position;
        auto c = WordNextCharacter(text, &position);
        if (c.codepoint == '\r') continue;
        if (c.codepoint == '\n') {
            flush();
        } else {
            if (c.codepoint == '\t') c = {" ", ' '};
            if (line.empty() && c.codepoint == ' ') continue;
            const int advance = WordMeasure(size, c.text);
            if (!line.empty() && used + advance > width) {
                // Prefer whitespace breaks in Latin prose; CJK keeps character
                // wrapping. A single overlong word/IPA can still span lines.
                if (c.codepoint < 0x2e80 && c.codepoint != ' ' && break_output != std::string::npos) {
                    line.resize(break_output); position = break_input;
                } else {
                    position = start;
                }
                flush();
            } else {
                if (advance > width) { result.truncated = true; continue; }
                if (c.codepoint == ' ') { break_output = line.size(); break_input = position; }
                line += c.text; used += advance;
            }
        }
        if (result.lines.size() == max_lines) {
            result.truncated = text.find_first_not_of(" \t\r\n", position) != std::string::npos;
            return result;
        }
    }
    if (!line.empty()) result.lines.push_back(line);
    return result;
}

int WordParagraph(int top, const std::string& text, int size, int pitch, size_t max_lines)
{
    auto wrapped = WordWrap(text, size, kWordWidth, max_lines);
    if (wrapped.truncated && !wrapped.lines.empty())
        wrapped.lines.back() = WordEllipsis(wrapped.lines.back(), size, kWordWidth);
    for (const auto& line : wrapped.lines) { WordDrawLine(kWordLeft, top, size, line); top += pitch; }
    return top;
}

void WordHeader(const wqn::UiFrame& frame)
{
    WordText(kStatusBarTitleX, kStatusBarTitleY, "WQN / 单词", 12, 260);
    DrawStatusBarIcons(kScreenWidth - kStatusBarRightInset, kStatusBarIconY, frame.home);
    DrawHorizontalLine(0, kStatusBarDividerY, kScreenWidth);
}

void WordFocus(int top, int height, bool selected)
{
    if (selected) FillRect(kWordLeft - 9, top + 6, 3, height - 12, true);
}

void WordHome(const wqn::WordAppSnapshot& word)
{
    WordText(kWordLeft, kWordHomeTitleY, "今日学习", 24);
    const std::string today = "已复习 " + std::to_string(word.reviewed_today) +
        "  答对 " + std::to_string(word.correct_today);
    const int today_width = std::min(210, WordMeasure(12, today));
    WordText(kWordLeft, kWordHomeSummaryY, word.scope_title, 12, kWordWidth - today_width - 10);
    WordText(kWordRight, kWordHomeSummaryY, today, 12, today_width, true);
    WordText(kWordLeft, kWordHomeNoticeY, word.hint.empty() ? word.status_line : word.hint, 12);
    DrawHorizontalLine(kWordLeft, kWordMenuTop - 2, kWordWidth);
    const char* titles[] = {"智能复习", "随机浏览", "遗忘的单词"};
    const char* subtitles[] = {"按到期顺序复习", "从当前词库随机抽取", "复习曾经答错的单词"};
    const bool resume[] = {word.review_session_resumable, word.shuffle_session_resumable, word.mistakes_session_resumable};
    const std::string counts[] = {
        word.review_due_known ? std::to_string(word.review_due_count) + " 到期" : "-- 到期",
        std::to_string(word.total_count) + " 词",
        word.mistake_count >= 0 ? std::to_string(word.mistake_count) + " 词" : "-- 词",
    };
    for (int i = 0; i < 3; ++i) {
        const int top = kWordMenuTop + i * kWordMenuPitch;
        WordFocus(top, kWordMenuPitch, static_cast<int>(word.home_selection) == i);
        WordText(kWordLeft, top + 4, titles[i], 20, 225);
        WordText(kWordRight, top + 10, word.pack_ready ? counts[i] : "未同步", 12, 130, true);
        WordText(kWordLeft, top + 31,
            !word.pack_ready ? "需同步词库" : resume[i] ? "可继续上次会话" : subtitles[i], 12);
        if (i != 2) DrawHorizontalLine(kWordLeft, top + kWordMenuPitch, kWordWidth);
    }
}

void WordComplete(const wqn::WordAppSnapshot& word)
{
    WordText(kWordLeft, 44, word.review_complete_empty ? "今天没有到期单词" : "本轮复习完成", 24);
    if (word.review_complete_empty) {
        WordText(kWordLeft, 94, "当前词库到期 0 词。");
        WordText(kWordLeft, 120, "可以继续顺序学习，或稍后再来。");
    } else {
        WordText(kWordLeft, 82, std::to_string(word.review_complete_reviewed), 48, 140);
        WordText(kWordLeft, 140, "本轮复习 / 张", 12, 150);
        WordText(kWordLeft + 154, 92, "没答对 " + std::to_string(word.review_complete_unknown) + " 张", 16, 214);
        WordText(kWordLeft + 154, 118, "重学 " + std::to_string(word.review_complete_replayed) + " 张", 16, 214);
    }
    DrawHorizontalLine(kWordLeft, kWordChoiceTop - 1, kWordWidth);
    const std::string cursor = word.sequential_total == 0 ? "" :
        word.sequential_cursor >= word.sequential_total ? "从头开始" :
        "#" + std::to_string(word.sequential_cursor) + " / " + std::to_string(word.sequential_total);
    for (int i = 0; i < 2; ++i) {
        const int top = kWordChoiceTop + i * kWordChoicePitch;
        WordFocus(top, kWordChoicePitch, static_cast<int>(word.complete_selection) == i);
        WordText(kWordLeft, top + 10, i == 0 ? "顺序过词库" : "返回单词首页", 20, 220);
        if (i == 0) {
            WordText(kWordRight, top + 17, cursor, 12, 140, true);
            DrawHorizontalLine(kWordLeft, top + kWordChoicePitch, kWordWidth);
        }
    }
}

struct WordBodyParagraph {
    int size;
    int pitch;
    WordWrapped wrapped;
    size_t shown = 0;
};

std::string WordMeaning(const wqn::WordAppSnapshot& word)
{
    const auto trim = [](const std::string& text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() :
            text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    const std::string meaning = trim(word.meaning);
    if (meaning.empty()) return "暂无释义";
    const std::string part_of_speech = trim(word.part_of_speech);
    if (part_of_speech.empty()) return meaning;

    size_t end = 0;
    while (end < meaning.size() && ((meaning[end] >= 'a' && meaning[end] <= 'z') ||
                                   (meaning[end] >= 'A' && meaning[end] <= 'Z'))) ++end;
    std::string label = meaning.substr(0, end);
    for (auto& c : label) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (end < meaning.size() && (meaning[end] == '.' || meaning[end] == ':' ||
        meaning[end] == ' ' || meaning[end] == '\t' || meaning[end] == '\r' ||
        meaning[end] == '\n' || static_cast<unsigned char>(meaning[end]) >= 0x80)) {
        // Imported meanings carry their own labels, including multiple senses.
        // Keep that text intact instead of prepending the summary POS again.
        for (const auto* prefix : {"n", "v", "vt", "vi", "adj", "adv", "pron", "prep",
             "conj", "num", "art", "int", "interj", "aux", "det", "modal", "abbr",
             "phr", "noun", "verb", "adjective", "adverb"})
            if (label == prefix) return meaning;
        if (label == "a" && meaning[end] == '.') return meaning;
    }
    return part_of_speech + " " + meaning;
}

void WordBack(int top, const wqn::WordAppSnapshot& word)
{
    if (!word.feedback.empty()) {
        FillRect(kWordLeft, top, 2, 17, true);
        WordText(kWordLeft + 8, top, word.feedback, 12, kWordWidth - 8);
        top += 25;
    }
    std::vector<WordBodyParagraph> paragraphs;
    paragraphs.push_back({20, 28, WordWrap(WordMeaning(word), 20, kWordWidth, 12), 0});
    if (!word.example.empty()) paragraphs.push_back({16, 22, WordWrap(word.example, 16, kWordWidth, 12), 0});
    if (!word.example_translation.empty()) paragraphs.push_back({16, 22, WordWrap(word.example_translation, 16, kWordWidth, 12), 0});
    int remaining = kWordBottom - top;
    bool omitted = false;
    // Reserve one line per field, then spend remaining room in reading order.
    // This keeps short meanings from wasting space and never invents a reader.
    for (auto& p : paragraphs) {
        if (!p.wrapped.lines.empty() && remaining >= p.pitch + 7) { p.shown = 1; remaining -= p.pitch + 7; }
        else omitted = omitted || !p.wrapped.lines.empty();
    }
    for (auto& p : paragraphs)
        while (p.shown && p.shown < p.wrapped.lines.size() && remaining >= p.pitch) { ++p.shown; remaining -= p.pitch; }
    size_t last_visible = 0;
    for (size_t i = 0; i < paragraphs.size(); ++i) if (paragraphs[i].shown) last_visible = i;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        auto& p = paragraphs[i];
        if (!p.shown) continue;
        const bool truncated = p.wrapped.truncated || p.shown < p.wrapped.lines.size() || (omitted && i == last_visible);
        for (size_t line = 0; line < p.shown; ++line) {
            const auto text = truncated && line + 1 == p.shown
                ? WordEllipsis(p.wrapped.lines[line], p.size, kWordWidth) : p.wrapped.lines[line];
            WordDrawLine(kWordLeft, top, p.size, text); top += p.pitch;
        }
        top += 7;
    }
}

void WordCard(const wqn::WordAppSnapshot& word)
{
    const std::string progress = word.mode_label + (word.progress_line.empty() ? "" : " " + word.progress_line);
    const std::string badge = word.save_failed ? "保存失败" : word.saving ? "保存中" : "";
    const int right_width = badge.empty() ? std::min(200, WordMeasure(12, progress)) : WordMeasure(12, badge) + 12;
    WordText(kWordLeft, kWordMetaY, word.scope_title, 12, kWordWidth - right_width - 12);
    if (badge.empty()) WordText(kWordRight, kWordMetaY, progress, 12, right_width, true);
    else {
        FillRect(kWordRight - right_width, kWordMetaY - 1, right_width, 20, true);
        WordText(kWordRight - 6, kWordMetaY, badge, 12, right_width - 12, true, false);
    }
    bool primary = true;
    for (size_t i = 0; i < word.word.size();) {
        const auto c = WordNextCharacter(word.word, &i);
        primary = primary && wqn::MeasureTextWithFont(WordPrimaryFont(32), c.text.c_str()) > 0;
    }
    const int size = primary && WordMeasure(32, WordOneLine(word.word)) <= kWordWidth ? 32 : 24;
    int top = WordParagraph(kWordHeadlineY, WordOneLine(word.word), size, size + 8, 2);
    if (!word.phonetic.empty()) top = WordParagraph(top + 4, word.phonetic, 16, 22, 2);
    const int divider = top + 10;
    DrawHorizontalLine(kWordLeft, divider, kWordWidth);
    if (word.card_phase == wqn::WordCardPhase::kFront) {
        if (!word.feedback.empty()) WordText(kWordLeft, divider + 13, word.feedback, 12);
        WordText(kWordLeft, std::max(178, divider + 37), "回忆这个词的意思");
    } else {
        WordBack(divider + 13, word);
    }
}

}  // namespace

esp_err_t RenderWordToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const auto& word = frame.word_app;
    wqn::ClearEpdFramebuffer(true);
    WordHeader(frame);
    if (word.mode == wqn::WordAppMode::kHome) WordHome(word);
    else if (word.mode == wqn::WordAppMode::kReviewComplete) WordComplete(word);
    else if (word.mode == wqn::WordAppMode::kSessionStarting || !word.has_card) {
        WordText(kWordLeft, 64,
            word.mode == wqn::WordAppMode::kSessionStarting ? "正在准备本轮单词" : "单词暂不可用", 24);
        WordText(kWordLeft, 118, word.scope_title);
        WordText(kWordLeft, 150, word.mode_label, 12);
        WordParagraph(180, word.hint.empty() ? word.status_line : word.hint, 16, 24, 3);
    } else WordCard(word);

    // Preserve the existing owner-task refresh policy. A region is a dirty
    // hint, not a hard clip; no panel timing or power behavior changes here.
    if (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig) {
        const UiRect content = {0, kStatusBarHeight, kScreenWidth, kScreenHeight - kStatusBarHeight, "word-paper"};
        return word.mode == wqn::WordAppMode::kHome
            ? RefreshStableRegion(content, schedule) : RefreshRegion(content, schedule);
    }
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
