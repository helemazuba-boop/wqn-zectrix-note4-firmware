# StudyRuntime 复用指南

单词是错题和笔记的参考实现，不是可整块复制的页面。只有 W8 全部门禁通过后，
才从已验收代码中提取通用 `StudyRuntime`。

## 可复用的生命周期

- 内容与用户进度分离：内容可版本化/缓存，进度由 observation 投影产生。
- session 固定 scope、ordering、snapshot、seed 和 cursor，支持暂停与精确恢复。
- observation 先本地 durable commit，后幂等上传，不用可覆盖的单 pending slot。
- UI reducer 仅产生 typed effects，迟到的存储/网络结果用 request 和上下文身份隔离。
- 后台内容更新只下轮生效，不改变活动会话中的 item 归因。

## 必须保持领域化的部分

- 单词的 known/unknown 算法、词包 JSONL 和 `WordCard` 不进入通用层。
- 错题的作答、判定、解析和 mastered 投影由 Problem domain 定义。
- 笔记的阅读、编辑、关联和提醒是 Note domain 事实，不能被伪装成单词的 known/unknown。
- “推荐不强迫”是产品原则，但每个 domain 可有自己的候选分桶和不影响进度的 browse 动作。

## 提取门禁

1. 先为新 domain 冻结 item id、session scope、observation 集合和投影权威。
2. 使用同一套幂等、有界 outbox、pause/resume 和异步结果隔离测试。
3. 用 fake content/projection adapter 证明 runtime 无 word include 与 word enum 后再抽组件。
4. 至少通过 500 次混合动作、掉电恢复、重放和活动内容替换测试。

