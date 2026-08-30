#pragma once

#include <cstddef>

namespace wqn::protocol {

// cJSON's recursive parser permits much deeper documents than the firmware's
// task stacks can safely parse. Count containers without treating brackets in
// JSON strings as structure so callers can reject excessive depth first.
inline bool JsonNestingWithinLimit(const char* data, std::size_t length,
                                   std::size_t max_depth)
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

}  // namespace wqn::protocol
