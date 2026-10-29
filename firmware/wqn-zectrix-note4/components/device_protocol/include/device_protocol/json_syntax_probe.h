#pragma once

#include <string_view>
#include "device_protocol/json_depth_guard.h"

namespace wqn::protocol {

// cJSON reports the same null result for malformed JSON and allocation failure.
// A complete document may have failed for lack of memory and must never be
// dropped during repair. Callers also use this to preserve opaque records.
// Fixed container states bound stack use; no recursion or global allocator hooks.
// Match cJSON's whitespace, BOM, raw string bytes and permissive number forms.
class JsonSyntaxProbe {
public:
    explicit JsonSyntaxProbe(std::string_view text) : text_(text) {}
    bool Complete()
    {
        enum State : unsigned char {
            kValue, kDone, kArrayFirst, kArrayNext, kArrayAfter,
            kObjectFirst, kObjectNext, kObjectColon, kObjectAfter,
        };
        State states[wqn::protocol::kMaxJsonNestingDepth + 1] = {kValue};
        size_t depth = 0;
        if (text_.compare(0, 3, "\xef\xbb\xbf") == 0) position_ = 3;
        while (true) {
            while (position_ < text_.size() &&
                   static_cast<unsigned char>(text_[position_]) <= 32) ++position_;
            const char ch = position_ < text_.size() ? text_[position_] : '\0';
            State& state = states[depth];
            if (state == kDone) return position_ == text_.size();
            if ((state == kArrayFirst && ch == ']') ||
                (state == kObjectFirst && ch == '}')) {
                ++position_; --depth; continue;
            }
            if (state == kArrayAfter || state == kObjectAfter) {
                if (ch == (state == kArrayAfter ? ']' : '}')) {
                    ++position_; --depth; continue;
                }
                if (ch != ',') return false;
                state = state == kArrayAfter ? kArrayNext : kObjectNext;
                ++position_; continue;
            }
            if (state == kObjectFirst || state == kObjectNext) {
                if (!String()) return false;
                state = kObjectColon; continue;
            }
            if (state == kObjectColon) {
                if (ch != ':') return false;
                ++position_; state = kValue; continue;
            }
            state = depth == 0 ? kDone :
                (state == kArrayFirst || state == kArrayNext ? kArrayAfter : kObjectAfter);
            if (ch == '[' || ch == '{') {
                if (depth == wqn::protocol::kMaxJsonNestingDepth) return false;
                states[++depth] = ch == '[' ? kArrayFirst : kObjectFirst;
                ++position_;
            } else if (ch == '"') {
                if (!String()) return false;
            } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                if (!Number()) return false;
            } else if (text_.compare(position_, 4, "null") == 0 ||
                       text_.compare(position_, 4, "true") == 0) {
                position_ += 4;
            } else if (text_.compare(position_, 5, "false") == 0) {
                position_ += 5;
            } else return false;
        }
    }
private:
    bool String()
    {
        if (position_ >= text_.size() || text_[position_++] != '"') return false;
        size_t end = position_;
        while (end < text_.size() && text_[end] != '"') {
            if (text_[end] == '\\') ++end;
            ++end;
        }
        if (end >= text_.size()) return false;
        while (position_ < end) {
            const char ch = text_[position_++];
            if (ch != '\\') continue;
            if (position_ == text_.size()) return false;
            const char escaped = text_[position_++];
            if (escaped == 'u') {
                unsigned first = 0;
                if (!Unicode(&first, end) || (first >= 0xdc00 && first <= 0xdfff)) return false;
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (text_.compare(position_, 2, "\\u") != 0) return false;
                    position_ += 2;
                    unsigned second = 0;
                    if (!Unicode(&second, end) || second < 0xdc00 || second > 0xdfff) return false;
                }
            } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                       escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                       escaped != 'r' && escaped != 't') return false;
        }
        position_ = end + 1;
        return true;
    }
    bool Unicode(unsigned* value, size_t end)
    {
        if (position_ > end || end - position_ < 4) return false;
        bool hex = true;
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned char ch = text_[position_++];
            *value <<= 4;
            if (ch >= '0' && ch <= '9') *value += ch - '0';
            else if (ch >= 'a' && ch <= 'f') *value += ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') *value += ch - 'A' + 10;
            else hex = false;
        }
        // cJSON's parse_hex4 maps non-hex sequences to zero too. Keeping that
        // compatibility prevents discarding an older readable record on OOM.
        if (!hex) *value = 0;
        return true;
    }
    bool Number()
    {
        if (text_[position_] == '-') ++position_;
        const size_t integer = position_;
        while (Digit()) ++position_;
        bool digits = position_ != integer;
        if (position_ < text_.size() && text_[position_] == '.') {
            const size_t fraction = ++position_;
            while (Digit()) ++position_;
            digits = digits || position_ != fraction;
        }
        if (!digits) return false;
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const size_t exponent = position_;
            while (Digit()) ++position_;
            if (position_ == exponent) return false;
        }
        return true;
    }
    bool Digit() const
    {
        return position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9';
    }
    std::string_view text_;
    size_t position_ = 0;
};


}  // namespace wqn::protocol
