# 单词离线 Outbox

## 持久顺序

1. reducer 生成 observation 和预期的 next position/phase。
2. `StorageService` 把 200-byte observation record append + fsync 到 `/storage/wout.v1`。
3. 只有本地提交成功后 UI 才推进。
4. 后台上传原 record；云端成功或幂等重放成功后 append ACK + fsync。
5. PrepareSleep 阶段将会话 cursor 和未 ACK observation 原子压缩；超时则拒绝深睡。

## 不变式

- 容量固定为 1,000 条；满时不覆盖旧观察，不提前显示“已记录”。
- request id 重试保持不变；401 才可清除设备 token，超时、429 和 5xx 保留队列。
- ACK 记录和对应 observation 共同提供掉电恢复 cursor 的事实，互动期不重写完整 session。
- 不完整尾记录通过 CRC 被识别并丢弃；临时文件和备份只用于原子替换恢复。
- 顺序、随机、词典 session 分别使用 `wsq/wsr/wsd.v1`，不共用单一 pending slot。

## 诊断日志

关注 `word observation durable`、`word-outbox-peek`、`word-outbox-ack`、`word outbox drained`
和 `word outbox prepared for sleep`。前台 commit 应优先于后台事务；单个已开始的 SPIFFS
事务不可被抢占，因此大型压缩只能在 PrepareSleep 中执行。

