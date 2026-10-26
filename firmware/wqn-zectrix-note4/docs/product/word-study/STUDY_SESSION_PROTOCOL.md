# StudySession 协议说明

权威机读契约位于 `contracts/word-study-v1/`，本文只说明生命周期。

## 会话创建

1. 设备向 `/api/esp32/v3/words/sessions` 提交 mode、scope、optional count 和幂等 request id；
   顺序过词库额外带 `start_index`（续点），新词头部额外带 `new_word_limit`（当日新词配额）。未设的字段必须整体省略，让云端使用自己的默认值。
2. 云端固定 deck/pack snapshot、progress revision、seed 和有序候选集。
3. 首页候选与 cursor 返回设备；后续页只能从同一 session 的候选表读取。
4. 设备保存 session 与精确 position/phase，重启时继续而不重排。

## 顺序规则

- `sequential` 按 deck order、`sort_index`、normalized word、item id 稳定排序。
- `guided_random_v1` 按“到期 learning、到期 review、new、未到期、mastered”分桶，
  桶内以 FNV-1a-64(`seed + NUL + item_id`) 排序。
- `lexicographic` 按 normalized word 和 item id 排序。
- `due_queue_v1`（智能复习）取当天 24:00 前到期的词，先 learning 后 due_at 升序。
- `new_intake_v1`（新词头部）按词库顺序取当日配额内的新词。
- `pure_random_v1`（随机）每次重新抽样打乱，排除 mastered；`mistake_words_v1`（遗忘的单词）取错词链，随机顺序。
- mode、ordering、candidate policy 三者必须成对出现；配对不符按契约违规拒绝，不做回退。
- 同一 snapshot 和 seed 必须在 TypeScript 与 C++ 得到同一结果。

## 观察语义

`shown / revealed / known / unknown / skipped / looked_up` 都是可追加事实。
`known / unknown` 可改变学习进度，其余不得被偷换成“完成学习”。
`looked_up` 是历史契约值：设备端词典已移除，固件不再发出该动作。

设备上传必须携带原 session id、sequence、item id、mode、occurred time 和稳定 request id。
网络超时只重放原请求，不生成新 request id。

## 终止与恢复

- pause 保留 session、cursor、position、phase 和固定 pack snapshot。
- completed 表示候选耗尽，closed 表示明确关闭；二者都不代表用户失败。
- 异步 session/page 结果必须验证当前 request 与页面上下文，迟到结果直接丢弃。

## 本地重学池（仅智能复习）

- 答 `unknown` 的词写入 FSRS（due=now），同时进入设备内存里的重学池；池不进 outbox，
  断电即丢（FSRS 已经记住它，重启后仍会到期）。
- 池内条目至少隔 5 张已作答的卡才可被随机抽回，同一个词最多重学 2 次；
  答 `known` 或第二次答 `unknown` 后出池。重学卡答完后回到它打断的那个队列位置。
- 队列与池都空才进入完成页（`今天的复习完成了 · 复习 N 张 · 重学 M 张`，并提示 X 张没答对）。
  完成页是纯本地判断，离线可用；空队列变体是“今天没有到期的单词”。
- 完成页只有两个动作：`顺序过词库（续 #N）` 和 `返回`。前者把当日新词（intake 会话）接在
  词库顺序游标之前，两段会话由设备去重，游标走完一圈归零。

