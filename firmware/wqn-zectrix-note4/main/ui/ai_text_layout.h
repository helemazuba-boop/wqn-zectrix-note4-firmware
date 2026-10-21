#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "font_fmt.h"
#include "markdown_layout.h"

namespace device_ui_internal {

enum class AiTextKind : uint8_t { kPlain, kMarkdown };
inline constexpr size_t kAiTextMaxRows = 4096;
inline constexpr size_t kAiTextCacheMaxEntries = 32;
inline constexpr size_t kAiTextCacheMaxBytes = 1024 * 1024;

struct AiTextLayout {
    std::vector<std::string> plain;
    std::vector<MdLine> markdown;
    bool truncated = false;
    int measured_width = 0;
};

std::vector<std::string> WrapAiNativeText(const std::string& text, int width,
                                         const lv_font_t* font, size_t max_rows,
                                         bool* truncated = nullptr);
std::shared_ptr<const AiTextLayout> GetAiTextLayout(
    uint64_t message_id, const std::string& text, AiTextKind kind, int width,
    const lv_font_t* font, size_t max_rows = kAiTextMaxRows,
    uint8_t markdown_options = 0);

#ifdef WQN_HOST_TEST
struct AiTextCacheStats {
    size_t entries = 0;
    size_t bytes = 0;
    uint64_t builds = 0;
    uint64_t hits = 0;
};
AiTextCacheStats GetAiTextCacheStats();
#endif

}  // namespace device_ui_internal
