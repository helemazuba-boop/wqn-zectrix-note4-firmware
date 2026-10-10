# v3 同步修复与实机交接（2026-10-10）

状态：审计确认的设备侧问题已落地；主机回归与构建通过后分批本地提交。
**HIL NOT VERIFIED。没有刷机、push、合并或发布。** 用户明确允许本轮先提交、后补 HIL。
这不重新打开已收口的存储任务，也不取消其原有未验项目。

## 修复边界

- `f1c54e8`：自动全量重试保留 attempt，标称阶梯 10/30/60/300/900 秒；计数饱和。
  成功、手动或凭据重置仍可清计数；claim 和服务端 retry-after 保留自己的策略。
- `edcf9ca`：完整请求 metadata 与 request_id 一起冻结；成功响应缓存至本地提交完成。
  config、cursor、content targets 一次提交到 journal，成功后才发布。
  journal 是唯一权威 checkpoint，不再读写旧 NVS 两个控制键。
- 本文同批修复：本地保存失败按 5/15/60 秒补写（attempt 饱和、上限 60 秒），
  不要求 WiFi；控制响应与当前 journal 意图各有独立待提交 deadline，其他 lane 的成功
  不能取消待提交控制响应。凭据变化先废弃旧响应，不能把它提交给新配对状态。
- 内容完成先准备候选，不提前发布 applied/Clean；保存失败保留每域一个有界待发布槽，
  暂禁同域再次 claim，任何包含该完成状态的成功 journal 提交后再发布。
- 精确字节去重只比较 StorageService 内的上次成功落盘内容；失败的文件提交使缓存失效。
  缓存不由 RAM 意图或备份读取播种；启动后第一次保存仍实际创建/修复主文件。
- 坏主文件恢复时不再覆盖有效备份；短写、flush/fsync/close 和 rename 失败均不能报成功。
  journal 读取上限 8 KiB，写入前回读解析并核对大整数，避免提交不可读/舍入后的状态。
  schema 1 仍可读并规范化到 schema 2；schema 2 必填字符串缺失按损坏处理。

未改：v3 线协议、云端、分区、词包/会话、配对凭据、outbox 业务记录、AI/UI 架构。
旧 NVS 控制键保留但不再推进；没有 journal 从零 bootstrap，损坏且无有效备份则报错，
不偷偷导入可能撕裂的 NVS 两键状态。旧固件降级可能读到过期 NVS 控制值，需单独兼容验证。

## 保存必要性与测量

| 保存原因 | 保留规则 |
|---|---|
| `control-response` | target 与 config/cursor 合并一次；仅完全相同的成功态可跳过 |
| `full-retry` / `outbox-retry` | 有效身份、attempt、原因、deadline 的变化仍写；相同已落盘字节跳过 |
| `protocol-latch` | 保留跨重启防风暴锁存；失败由独立本地 dirty retry 补写 |
| `install-marker` | 安装前必须成功；不能为速度删除 |
| `content-complete` | applied/clean 或内容 backoff 的真实推进必须保存，失败不得提前发布 |
| `startup-recovery` | fetching/installing 恢复为 pending 等真实恢复转换仍保存 |
| `local-retry` | 补写最新意图，成功才清 dirty；没有网络 RPC |

新增独立日志行，不输出 payload、token 或凭据：

```text
sync journal save: reason=control-response outcome=written bytes=... payload_hash=... total_us=... result=ESP_OK
```

`outcome` 为 `written/skipped/failed`。hash 仅诊断，不能据 hash 相同推断字节相同；
实际去重比较完整字节。该行 `total_us` 是 backend（含序列化与文件步骤），**不含队列等待**；
须与原 `save-sync-journal` StorageService transaction 的 queue/exec 配对观察。
参数/序列化检查在文件 I/O 前失败可能没有该行，不能因此把失败当作 skipped。

历史四份日志共 26 次 journal 保存：backend 中位 2205.769 ms、p90 2750.768 ms、
最大 3149.159 ms。这是旧固件跨日志分布，不是本次修复后的性能。
**尚未测到新固件各原因的 written/skipped 次数、真实节省时间及前台排队改善。**
主机去重计数能验证代码省掉重复轮转，不能预测实机收益。
`pp-manifest-save` 等其他长存储事务仍可阻塞共享队列，未在本轮偷偷改写。

## 可复现的主机检查

从固件目录运行：

```bash
python3 scripts/test_sync_retry.py
python3 scripts/test_sync_control.py
python3 scripts/test_sync_journal.py
python3 scripts/test_word_ack_upload.py
python3 scripts/hil_check.py --selftest
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-ai-local-s3 build
git -c core.whitespace=cr-at-eol diff --check
```

主要覆盖：真实 admission/helper、v3 JSON builder、journal codec、存储队列接口、
临时主机文件系统、调用边界故障、备份恢复、分配失败与单次互斥锁释放。
不覆盖真实 RTOS 竞争、NOR 半写/半擦或线上去重数据库。

本轮结果：退避 36/36、控制 51/51、journal 34/34、ACK 上传 24/24；
另 10 组相关单词/空包回归合计 507/507。ESP-IDF 构建 EXIT 0、0 warnings，M8 passed。
主机故障点不是实机事故样本，也不能替代下面的 HIL。

反向 mutation 必须 EXIT 1，不能只看绿灯：

```bash
python3 scripts/test_sync_retry.py --mutation reset-on-auto
python3 scripts/test_sync_retry.py --mutation wrap-attempt
python3 scripts/test_sync_control.py --mutation mutable-sync-metadata
python3 scripts/test_sync_control.py --mutation publish-before-commit
python3 scripts/test_sync_control.py --mutation replay-successful-rpc
python3 scripts/test_sync_control.py --mutation drop-local-retry
python3 scripts/test_sync_control.py --mutation early-completion
python3 scripts/test_sync_control.py --mutation cache-failed-write
python3 scripts/test_sync_journal.py --mutation destroy-backup
python3 scripts/test_sync_journal.py --mutation cache-failed-write
```

诊断产物：`analysis/sync-v3-audit.Wqoy5JkU/`（gitignored）。原只读审计 REPORT.md
描述修复前状态；它的旧诊断 runner 不是新代码的回归入口，应使用以上已入库脚本。
判据自检 353 PASS / 0 FAIL。重放旧 `serial-COM7-261010-003535.log` 仍是
18 PASS / 1 FAIL / 33 SKIP；FAIL 是旧 PK 双侧区间（open_seek 中位 67 ms），
未改阈值或涂绿，不能拿旧日志验收新镜像。

## 实机必补清单

当前均 **SKIP（没有测到，不是通过）**。普通使用检查先做，不清会话、不全擦。
故障注入、真实断电或任何文件修改，须另约受控操作与数据保护，不能随意破坏唯一设备。

1. 保留当前数据升级，先记录模式/未答卡，进入确认恢复；触发一次手动同步，
   观察词包、笔记、错题各域和 outbox，不以全局“成功”代替各域 applied 检查。
2. 正常联网，无内容变化，多次同步；采集上述原因/指纹/结果行和对应 queue/exec，
   确认重复保存出现 skipped，同时观察前台预取等待；不能只量空 journal。
3. 受控网络失败，验证实际自动退避、同 ID 请求体冻结和重启恢复；
   server retry-after、无 token claim 不混入标称阶梯统计。
4. 受控本地保存失败、网络已经成功：恢复存储后应本地补写，无第二次相同 RPC；
   同时断网应不阻止该补写。另验证凭据变更会废弃旧缓存响应。
5. outbox 清空/协议锁存保存失败后，RAM 已无变化仍应本地补写，成功后才消除 pending；
   不把协议阻断解除或 token 清除当作补写替代方案。仅真实 HTTP 401 可清 token。
6. 内容安装完成保存失败：applied/Clean 不提前发布；同域不能重装；另一域和更高 target
   可继续产生意图，最终 durable 提交不得被旧完成快照覆盖。
7. 受控真实断电，覆盖 installing、完成 checkpoint、主/备轮转和备份修复；
   恢复完整旧态/新态，跨域内容及待上传业务记录不丢。USB 重连、软件 reset 不算断电。
8. 深睡/唤醒与压力：测栈 HWM、互斥等待、StorageService 队列、公平性及真实耗时。
   本地 dirty/响应缓存本身在 RAM，重启后回到最后有效 journal，可能保守重试或重装；
   不宣称“未成功落盘的意图跨掉电仍必保留”。

只有补齐这些证据后才能判新链路 HIL 通过；本轮本地提交不是发布或合并批准。
