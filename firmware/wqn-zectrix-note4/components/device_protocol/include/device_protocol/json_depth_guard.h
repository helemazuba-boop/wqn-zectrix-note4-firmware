#pragma once

#include <cstddef>

namespace wqn::protocol {

inline constexpr std::size_t kMaxJsonNestingDepth = 32;

// cJSON's recursive parser permits much deeper documents than the firmware's
// task stacks can safely parse. Count containers without treating brackets in
// JSON strings as structure so callers can reject excessive depth first.
inline bool JsonNestingWithinLimit(const char* data, std::size_t length,
                                   std::size_t max_depth = kMaxJsonNestingDepth)
{
    if (data == nullptr) {
        return length == 0;
    }

    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = data[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max_depth) {
                return false;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return true;
}

// cJSON exposes decoded names/values as NUL-terminated pointers, without a
// length. A JSON U+0000 therefore aliases its prefix at subsequent key/ID/enum
// comparisons. Malformed hex escapes also decode to zero in cJSON. Inspect
// the bounded bytes, including raw NULs after an otherwise complete document,
// before cJSON can erase that distinction.
inline bool JsonStringsRepresentable(const char* data, std::size_t length)
{
    if (data == nullptr) return length == 0;
    bool escaped = false;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = data[i];
        if (c == '\0') return false;
        if (escaped) {
            if (c == 'u') {
                if (length - i <= 4) return false;
                unsigned value = 0;
                for (std::size_t digit = 1; digit <= 4; ++digit) {
                    const char hex = data[i + digit];
                    value <<= 4;
                    if (hex >= '0' && hex <= '9') value += hex - '0';
                    else if (hex >= 'a' && hex <= 'f') value += hex - 'a' + 10;
                    else if (hex >= 'A' && hex <= 'F') value += hex - 'A' + 10;
                    else return false; // cJSON maps malformed hex escapes to zero.
                }
                if (value == 0) return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        }
    }
    return true;
}

// Depth and cJSON string representability; the parser owns other syntax.
// Durable decoders must separately define preservation/recovery for rejected
// strings rather than treating every guard failure as disposable corruption.
inline bool JsonInputWithinLimits(const char* data, std::size_t length,
                                  std::size_t max_depth = kMaxJsonNestingDepth)
{
    return JsonNestingWithinLimit(data, length, max_depth) &&
        JsonStringsRepresentable(data, length);
}

}  // namespace wqn::protocol
