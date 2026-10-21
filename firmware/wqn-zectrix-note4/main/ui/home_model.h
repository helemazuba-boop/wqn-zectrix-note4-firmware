#pragma once

#include <cstdint>
#include <string>
#include "ui_screen.h"

namespace wqn {

inline constexpr int64_t kHomeAgentActiveMs = 30000;
inline constexpr int64_t kHomeAgentPollMs = 15000;
inline constexpr int64_t kHomeAgentFreshMs = 45000;
bool ShouldPollHomeAgent(int64_t now_ms, int64_t activity_ms, int64_t next_ms,
                         bool on_home, bool menu_visible, bool paired,
                         bool online, bool running);

enum class HomeAiStatus : uint8_t {
    kIdle, kRunning, kPermission, kQuestion, kComplete, kFailed, kInterrupted, kUnknown,
};

struct HomeAiCard {
    HomeAiStatus status = HomeAiStatus::kIdle;
    std::string title = "继续上次对话";
    std::string badge = "随时可用";
    std::string action = "进入最近使用的 AI 会话";
    std::string session_id;
    std::string session_title;
    int64_t result_version = 0;
    UiScreen target = UiScreen::kAi;
};

struct HomeStudyCard {
    std::string title = "笔记与错题";
    std::string progress;
    UiScreen target = UiScreen::kNote;
    bool resumable = false;
    int progress_percent = -1;
};

HomeAiCard MakeHomeAiCard(HomeAiStatus status, const std::string& session_id,
                         const std::string& title, int64_t result_version,
                         bool paired, bool enabled, bool viewed);
std::string HomeContentSignature(const HomeAiCard& ai, const HomeStudyCard& study,
                                 const std::string& review, const std::string& todo,
                                 const std::string& notice);

}  // namespace wqn
