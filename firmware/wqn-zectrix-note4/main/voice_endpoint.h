#pragma once

#include <string>
#include "config.h"

namespace wqn {

// REST and voice share an origin by default. Their route paths are separate;
// a reverse proxy with a different WS route can set WQN_VOICE_WS_URL explicitly.
inline std::string BuildVoiceWebSocketUrl(const std::string& api_base,
                                          const std::string& ws_path,
                                          const std::string& override_url)
{
    const bool explicit_url = !override_url.empty();
    const std::string& base = explicit_url ? override_url : api_base;
    size_t start = 0;
    std::string scheme;
    if (base.compare(0, explicit_url ? 6 : 8, explicit_url ? "wss://" : "https://") == 0) {
        start = explicit_url ? 6 : 8;
        scheme = "wss://";
    } else if (base.compare(0, explicit_url ? 5 : 7, explicit_url ? "ws://" : "http://") == 0) {
        start = explicit_url ? 5 : 7;
        scheme = "ws://";
    } else {
        return {};
    }
    for (unsigned char c : base) {
        if (c <= 0x20 || c == 0x7f || c == '\\' || c == '#') return {};
    }
    const size_t end = base.find_first_of("/?", start);
    const std::string authority = base.substr(start, end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return {};
    // Preserve ports and bracketed IPv6 literals without splitting on ':' here.
    if (explicit_url) return base;
    if (ws_path.empty() || ws_path[0] != '/' || ws_path.compare(0, 2, "//") == 0) return {};
    for (unsigned char c : ws_path) {
        if (c <= 0x20 || c == 0x7f || c == '\\' || c == '#') return {};
    }
    return scheme + authority + ws_path;
}

inline std::string BuildVoiceWebSocketUrl()
{
    return BuildVoiceWebSocketUrl(WQN_API_BASE, WQN_FLASH_WS_PATH, WQN_VOICE_WS_URL);
}

}  // namespace wqn
