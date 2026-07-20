# StudySession 协议说明

权威机读契约位于 `contracts/word-study-v1/`，本文只说明生命周期。

## 会话创建

1. 设备向 `/api/esp32/v3/words/sessions` 提交 mode、scope、optional count 和幂等 request id。
2. 云端固定 deck/pack snapshot、progress revision、seed 和有序候选集。
3. 首页候选与 cursor 返回设备；后续页只能从同一 session 的候选表读取。
4. 设备保存 session 与精确 position/phase，重启时继续而不重排。

## 顺序规则

- `sequential` 按 deck order、`sort_index`、normalized word、item id 稳定排序。
- `guided_random_v1` 按“到期 learning、到期 review、new、未到期、mastered”分桶，
  桶内以 FNV-1a-64(`seed + NUL + item_id`) 排序。
- `lexicographic` 按 normalized word 和 item id 排序。
- 同一 snapshot 和 seed 必须在 TypeScript 与 C++ 得到同一结果。

## 观察语义

`shown / revealed / known / unknown / skipped / looked_up` 都是可追加事实。
`known / unknown` 可改变学习进度，其余不得被偷换成“完成学习”。

设备上传必须携带原 session id、sequence、item id、mode、occurred time 和稳定 request id。
网络超时只重放原请求，不生成新 request id。

## 终止与恢复

- pause 保留 session、cursor、position、phase 和固定 pack snapshot。
- completed 表示候选耗尽，closed 表示明确关闭；二者都不代表用户失败。
- 异步 session/page/lookup 结果必须验证当前 request 与页面上下文，迟到结果直接丢弃。

