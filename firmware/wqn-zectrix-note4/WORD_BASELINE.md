# WQN 单词参考基线

本文档冻结 Note4 单词功能的产品与运行时边界，供错题和笔记后续复用。

## 产品语义

- 入口只有“顺序 / 随机 / 词典”，三者使用同一个 `WordCard`。
- 系统会记录、给出候选，但不设置强制“今日目标”，暂停或退出不算失败。
- 随机是 `guided_random_v1`：候选先按学习状态分桶，桶内按 session seed 稳定随机；界面不显示推荐理由。
- 查词本身不改变进度。只有用户明确产生 `known` 或 `unknown` 才修改进度；`skipped` 只是事实记录。

## 事实与投影

`StudyObservation` 是只追加的事实。云端以 `user_id + request_id` 幂等，以
`session_id + sequence` 保证会话内顺序。`WordProgress` 和错词是该事实的投影，
不能成为第二套可独立写入的事实源。

## 设备端保证

- 按键事件只驱动 `WordAppState` reducer，网络和存储以 typed effect/result 返回。
- 观察先进入有界 durable outbox，本地提交成功后才推进卡片。
- 顺序、随机、词典各有独立持久会话槽；模式切换不销毁其他模式的暂停会话。
- 活动会话固定 pack snapshot；后台下载的新 pack 只能下轮生效。
- 旧 `/words/sync` 和 `/words/review` 客户端已从固件删除，架构门禁防止其回归。

## 当前切换边界

W7 的 Web、AI 和错词云端投影仍在评估，因此 W8 可以冻结固件参考实现，
但在 AI/Web 全部改用统一 observation RPC 前，不得宣布云端旧写路径已完成切换。

