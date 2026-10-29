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
// comparisons. Inspect the original bounded bytes, including raw NULs after
// an otherwise complete document, before cJSON can erase that distinction.
inline bool JsonStringsRepresentable(const char* data, std::size_t length)
{
    if (data == nullptr) return length == 0;
    bool escaped = false;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = data[i];
        if (c == '\0') return false;
        if (escaped) {
            if (c == 'u' && length - i > 4 && data[i + 1] == '0' &&
                data[i + 2] == '0' && data[i + 3] == '0' && data[i + 4] == '0') {
                return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        }
    }
    return true;
}

// External protocol boundary: depth and cJSON string representability only.
// The JSON parser still owns syntax validation. Local persistence decoders
// retain their independent repair/preservation policy and depth-only helper.
inline bool JsonInputWithinLimits(const char* data, std::size_t length,
                                  std::size_t max_depth = kMaxJsonNestingDepth)
{
    return JsonNestingWithinLimit(data, length, max_depth) &&
        JsonStringsRepresentable(data, length);
}

}  // namespace wqn::protocol
