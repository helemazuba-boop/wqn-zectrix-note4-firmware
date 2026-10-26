#pragma once

#include <cstddef>

namespace wqn::voice_diagnostics {

// IDs/stage keys are metadata, never free-form provider text or credentials.
// Return a static marker for keys that could corrupt a key=value log record.
inline const char* SafeKey(const char* value, size_t limit = 63)
{
    if (value == nullptr || value[0] == '\0') return "none";
    for (size_t i = 0; i <= limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == 0) return value;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == ':')) return "redacted";
    }
    return "redacted";
}

}  // namespace wqn::voice_diagnostics
