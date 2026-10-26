#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wqn {
inline constexpr char kAiInteractionSchemaSha256[] =
    "aeb4633540d380c3cc8e3eb6d1cf4d415137e9b33f04eaddb462f76e0b138efc";
struct AiInteractionOption {
    std::string value;
    std::string label;
};
// Immutable wire payload plus the session owner's reply status. UI focus is
// separate; only the worker sends HTTP and publishes the resulting snapshot.
struct AiInteraction {
    std::string id;
    std::string kind;
    std::string prompt;
    std::vector<AiInteractionOption> options;
    bool allow_custom = false;
    bool sending = false;
    int64_t armed_at_ms = 0;
    std::string reply_value;
    std::string error;
};
} // namespace wqn
