#include "ai_text_layout.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "ai_native_text.h"

namespace device_ui_internal {
namespace {

size_t CodepointBytes(const std::string& text, size_t pos)
{
    const unsigned char lead = text[pos];
    size_t bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (pos + bytes > text.size()) return 1;
    for (size_t i = 1; i < bytes; ++i)
        if ((static_cast<unsigned char>(text[pos + i]) & 0xc0) != 0x80) return 1;
    return bytes;
}

struct Entry {
    uint64_t id;
    std::string source;
    AiTextKind kind;
    int width;
    const lv_font_t* font;
    size_t max_rows;
    size_t bytes;
    std::shared_ptr<const AiTextLayout> layout;
};

// Only metadata is mutable, always under this mutex. Returned layouts are
// immutable and retain their lifetime independently of LRU eviction.
std::mutex cache_mutex;
std::vector<Entry> cache;
AiTextCacheStats stats;

size_t LayoutBytes(const AiTextLayout& layout, size_t source_bytes)
{
    size_t bytes = sizeof(Entry) + sizeof(AiTextLayout) + source_bytes;
    bytes += layout.plain.capacity() * sizeof(std::string);
    for (const auto& row : layout.plain) bytes += row.capacity();
    bytes += layout.markdown.capacity() * sizeof(MdLine);
    for (const auto& row : layout.markdown) {
        bytes += row.text.capacity() + row.decorations.capacity() * sizeof(MdDecoration);
        bytes += row.col_seps.capacity() * sizeof(int16_t) + row.cells.capacity() * sizeof(MdTableCell);
        for (const auto& cell : row.cells) bytes += cell.text.capacity();
    }
    return bytes;
}

}  // namespace

std::vector<std::string> WrapAiNativeText(const std::string& text, int width,
                                         const lv_font_t* font, size_t max_rows,
                                         bool* truncated)
{
    if (truncated) *truncated = false;
    std::vector<std::string> rows;
    max_rows = std::min(max_rows, kAiTextMaxRows);
    if (!max_rows || width <= 0) { if (truncated) *truncated = !text.empty(); return rows; }
    size_t start = 0, pos = 0, last_space = 0;
    int used = 0;
    const auto append = [&](size_t first, size_t end) {
        std::string row = text.substr(first, end - first);
        row.erase(std::remove(row.begin(), row.end(), '\r'), row.end());
        rows.push_back(std::move(row));
    };
    while (pos < text.size() && rows.size() < max_rows) {
        if (text[pos] == '\r') { ++pos; continue; }
        if (text[pos] == '\n') {
            append(start, pos);
            start = ++pos; used = 0; last_space = start; continue;
        }
        const size_t count = CodepointBytes(text, pos);
        const std::string cp = text.substr(pos, count);
        const int advance = wqn::MeasureTextWithFont(font, cp.c_str());
        if (used > 0 && used + advance > width) {
            const size_t end = text[pos] == ' ' ? pos : AiWrapBoundary(text, start, pos, last_space);
            append(start, end);
            start = end;
            while (start < text.size() && text[start] == ' ') ++start;
            pos = start; used = 0; last_space = start; continue;
        }
        used += advance;
        if (text[pos] == ' ') last_space = pos;
        pos += count;
    }
    if (start < text.size() && rows.size() < max_rows) append(start, text.size());
    else if (rows.empty() && text.empty()) rows.emplace_back();
    const bool cut = pos < text.size();
    if (truncated) *truncated = cut;
    if (cut && !rows.empty()) rows.back() = AiNativeClip(rows.back() + "...", width, font);
    return rows;
}

std::shared_ptr<const AiTextLayout> GetAiTextLayout(
    uint64_t message_id, const std::string& text, AiTextKind kind, int width,
    const lv_font_t* font, size_t max_rows)
{
    max_rows = std::min(max_rows, kAiTextMaxRows);
    const auto match = [&](const Entry& entry) {
        return entry.id == message_id && entry.kind == kind && entry.width == width &&
            entry.font == font && entry.max_rows == max_rows && entry.source == text;
    };
    {
        const std::lock_guard<std::mutex> guard(cache_mutex);
        const auto found = std::find_if(cache.begin(), cache.end(), match);
        if (found != cache.end()) {
            auto result = found->layout;
            std::rotate(found, found + 1, cache.end());
            ++stats.hits;
            return result;
        }
    }
    // Expensive work is outside the mutex. Two cold consumers may both build;
    // the second publisher rechecks and adopts the first immutable result.
    auto next = std::make_shared<AiTextLayout>();
    if (kind == AiTextKind::kMarkdown) {
        next->markdown = LayoutMarkdown(text, width, 0, font, &next->truncated);
        if (next->truncated && !next->markdown.empty()) {
            MdLine notice;
            notice.text = AiNativeClip("内容过长，后续未显示", width, font);
            next->markdown.back() = std::move(notice);
        }
    } else {
        next->plain = WrapAiNativeText(text, width, font, max_rows, &next->truncated);
        if (next->truncated && max_rows == kAiTextMaxRows && !next->plain.empty())
            next->plain.back() = AiNativeClip("内容过长，后续未显示", width, font);
        for (const auto& row : next->plain)
            next->measured_width = std::max(next->measured_width,
                wqn::MeasureTextWithFont(font, row.c_str()));
    }
    const size_t bytes = LayoutBytes(*next, text.size());
    const std::lock_guard<std::mutex> guard(cache_mutex);
    ++stats.builds;
    const auto existing = std::find_if(cache.begin(), cache.end(), match);
    if (existing != cache.end()) return existing->layout;
    if (bytes > kAiTextCacheMaxBytes) return next;
    while (!cache.empty() && (cache.size() >= kAiTextCacheMaxEntries || stats.bytes + bytes > kAiTextCacheMaxBytes)) {
        stats.bytes -= cache.front().bytes;
        cache.erase(cache.begin());
    }
    cache.push_back({message_id, text, kind, width, font, max_rows, bytes, next});
    stats.bytes += bytes;
    return next;
}

AiTextCacheStats GetAiTextCacheStats()
{
    const std::lock_guard<std::mutex> guard(cache_mutex);
    auto result = stats;
    result.entries = cache.size();
    return result;
}

}  // namespace device_ui_internal
