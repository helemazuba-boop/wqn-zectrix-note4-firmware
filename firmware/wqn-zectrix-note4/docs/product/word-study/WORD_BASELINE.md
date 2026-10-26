# WQN 单词参考基线

本文档冻结 Note4 单词功能的产品与运行时边界，供错题和笔记后续复用。

## 产品语义

- 入口只有“智能复习 / 随机 / 遗忘的单词”，三者使用同一个 `WordCard`；词典入口已移除（查词交给轻量 AI 模型，用户不必一直按键）。
- “顺序过词库”不再是入口：它只出现在智能复习完成页，按词库顺序无限制地过词；当日新词（默认 20，`new_word_limit`）排在这条队列的最前面。
- 系统会记录、给出候选，但不设置强制“今日目标”，暂停或退出不算失败。
- 智能复习取当天到期的队列（`due_queue_v1`，上海时区日界），答“不认识”的词除照常写 FSRS 外，还进入本地重学池：至少隔 5 张卡再随机穿插回来，同一个词最多重学 2 次；队列与重学池都空才算完成。
- 随机是 `pure_random_v1`：每次会话重新抽样、真正打乱（排除 mastered），没有完成态。
- 遗忘的单词取 `word_mistake_links`（mastered 自动剔除），随机顺序，没有完成态。
- `guided_random_v1` 只作为历史契约值保留，当前没有入口使用。
- 只有用户明确产生 `known` 或 `unknown` 才修改进度（都走 FSRS）；`skipped` 只是事实记录。

## 事实与投影

`StudyObservation` 是只追加的事实。云端以 `user_id + request_id` 幂等，以
`session_id + sequence` 保证会话内顺序。`WordProgress` 和错词是该事实的投影，
不能成为第二套可独立写入的事实源。

## 设备端保证

- 按键事件只驱动 `WordAppState` reducer，网络和存储以 typed effect/result 返回。
- 观察先进入有界 durable outbox，本地提交成功后才推进卡片。
- 顺序、智能复习、随机、遗忘的单词各有独立持久会话槽；模式切换不销毁其他模式的暂停会话。
- 活动会话固定 pack snapshot；后台下载的新 pack 只能下轮生效。
- 旧 `/words/sync`、`/words/review`、`/words/search` 和 `/words/ai-lookup` 客户端已从固件删除，架构门禁防止其回归。
- 顺序游标（`start_index`）存在 NVS，并随会话记录一起持久化，恢复后仍显示 `#N / 总数`。

## 当前切换边界

W7 的 Web、AI 和错词云端投影仍在评估，因此 W8 可以冻结固件参考实现，
但在 AI/Web 全部改用统一 observation RPC 前，不得宣布云端旧写路径已完成切换。

