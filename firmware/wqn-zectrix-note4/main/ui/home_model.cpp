#include "home_model.h"

namespace wqn {

bool ShouldPollHomeAgent(int64_t now_ms, int64_t activity_ms, int64_t next_ms,
                         bool on_home, bool menu_visible, bool paired,
                         bool online, bool running)
{
    return on_home && !menu_visible && paired && online && now_ms >= activity_ms &&
        now_ms - activity_ms < kHomeAgentActiveMs && now_ms >= next_ms &&
        (next_ms == 0 || running);
}

HomeAiCard MakeHomeAiCard(HomeAiStatus status, const std::string& session_id,
                         const std::string& title, int64_t result_version,
                         bool paired, bool enabled, bool viewed)
{
    HomeAiCard card;
    card.status = status;
    card.session_id = session_id;
    card.session_title = title;
    card.result_version = result_version;
    if (!enabled || !paired) {
        card.title = enabled ? "先配对设备" : "AI 暂不可用";
        card.badge = enabled ? "未配对" : "未启用";
        card.action = "进入设置查看连接状态";
        card.session_id.clear();
        card.target = UiScreen::kSettings;
        return card;
    }
    switch (status) {
        case HomeAiStatus::kRunning:
            card.badge = "正在工作";
            card.action = "返回 Agent 会话查看任务";
            break;
        case HomeAiStatus::kPermission:
            card.badge = "需要授权";
            card.action = "进入会话，查看待处理请求";
            break;
        case HomeAiStatus::kQuestion:
            card.badge = "等待回答";
            card.action = "进入会话，回答待处理问题";
            break;
        case HomeAiStatus::kComplete:
            card.badge = viewed ? "已查看" : "已完成";
            card.action = viewed ? "返回会话，继续对话" : "任务完成，进入会话查看结果";
            break;
        case HomeAiStatus::kFailed:
            card.badge = "执行失败";
            card.action = "进入会话，查看失败原因";
            break;
        case HomeAiStatus::kInterrupted:
            card.badge = "已中止";
            card.action = "进入会话，查看任务记录";
            break;
        case HomeAiStatus::kUnknown:
            card.title = "上次任务待更新";
            card.badge = "状态未同步";
            card.action = "联网同步后显示最新状态";
            return card;
        case HomeAiStatus::kIdle:
            return card;
    }
    card.title = title.empty() ? "Agent 任务" : title;
    return card;
}

std::string HomeContentSignature(const HomeAiCard& ai, const HomeStudyCard& study,
                                 const std::string& review, const std::string& todo,
                                 const std::string& notice)
{
    // Length prefixes prevent task names containing separators from colliding.
    std::string signature;
    const auto append = [&signature](const std::string& value) {
        signature += std::to_string(value.size()) + ':' + value;
    };
    append(ai.title); append(ai.badge); append(ai.action);
    append(study.title); append(study.progress);
    append(std::to_string(study.progress_percent));
    append(study.resumable ? "1" : "0");
    append(review); append(todo); append(notice);
    return signature;
}

}  // namespace wqn
