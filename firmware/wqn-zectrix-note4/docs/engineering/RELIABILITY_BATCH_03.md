# 第三批：运行期 outbox 数据保全

日期：2026-10-22。分支：`fix/firmware-reliability`。起点：`13c436b`。

## 交付范围与工作节奏

本批处理全仓审查的 R-04、R-05、R-06。B2 / REL-003 的启动 schema 保全结论
继续有效；本批是运行期记录整理、追加及恢复，不重新打开启动恢复任务。

用户明确授权：“继续推进 允许在不经测试的情况下创建本地提交 在关键节点
停下来验证”。因此覆盖仓库默认的逐提交测试/HIL 门：先按域本地提交，在三域
outbox 这一关键节点集中验证。该授权不表示设备已验收，也不包含 push、PR 或刷机。
本批没有修改 AGENTS.md，也没有将这个例外写成仓库通用规则。

| 提交 | 内容 | 提交当时的验证状态 |
|---|---|---|
| `f25f3a0` | Word：保留最新主文件，支持受保护发布中断恢复 | 测试/构建延后至节点 |
| `9d2a40f` | Note：发布恢复、失败缓存失效、ACK/隔离追加前修复 | 测试/构建延后至节点 |
| `60d9db8` | Problem：重写备份恢复、JSONL 残尾修复、严格幂等判断 | 测试/构建延后至节点 |
| `9bdf096` | 补齐 Note 挂起追加修复；分块 JSONL；故障回归与 CI 入口 | 随后执行本节点最终构建/回归 |
| 本记录所在文档提交 | 验证结果、候选身份、实机矩阵和后续路线 | 文档检查及已完成的节点验证 |

当前状态：代码和本地节点验证通过；设备断电/并发 HIL 待执行。R-04～R-06
不标记为已经完成实机验收。RTC/I2C 与传输批次尚未开始。

## Word / Note：保留最新主文件，同时保留兼容备份

原 `preserve_backup` 分支既用于从备份恢复，也用于含挂起记录的正常整理。
后者的最新记录在主文件中，旧 `.bak` 可能不含这些记录；先 remove 主文件再
rename 候选，一旦发布失败就可能只能读到过期备份。

本批分别增加 `/storage/wout.rec`、`/storage/nout.rec`：

1. 正常读优先使用有效主文件；主文件读取/关闭 I/O 失败返回错误，不降级到旧数据。
2. 主文件缺失或内容不合规则尝试 `.rec`，再尝试原 `.bak`；使用恢复源时要求
   后续整理成功后再追加。普通 `.tmp` 不被自动选为权威数据。
3. 候选先写完并 flush/fsync/close。若本次主文件是权威源，先清理过期 `.rec`；
   清理失败停止操作，最新主文件仍在。这样旧 `.rec` 不会在下一次普通轮转失败后
   抢过更新的 `.bak`。
4. 受保护发布把有效主文件移到 `.rec`，保留旧 `.bak`；若从恢复源启动，则保留
   该恢复源，只丢弃已判为坏的主文件。rename 候选失败时，最新副本仍可恢复。
5. 主文件发布成功后才尝试清理 `.rec`。清理失败留下一份有界旧副本，主文件仍
   为权威源，不把成功发布误报为数据未保存。

记录布局、CRC、业务协议、schema generation 和 StorageService owner 均未改变。
新增的 `.rec` 是旧固件不知道的中间恢复状态；未完成发布时先由本候选恢复、保护
记录后再考虑降级，不能承诺旧固件能理解这一状态。

## Note：失败后重新读盘，所有标记追加先修复残尾

- 追加或整理返回错误即废弃 owner-task 缓存，下一次按真实文件重建。
- scanner 检查 fread/ferror/fclose，避免把读取故障当成可修复的短尾。
- ACK、隔离以及挂起追加前，若来源为备份或存在短尾，先 checkpoint 会话，再
  整理到完整主文件；确认成功后才建立后续迭代器、追加标记、更新缓存。
- 新回归暴露“已有挂起记录 + 短尾 + 再次挂起”的遗漏，节点补丁增加相同的修复
  前置检查；去掉此检查的错误变体会丢失预期的挂起状态，已被断言拒绝。

## Problem：重写恢复、完整帧与严格幂等

- 增加 `/storage/po_outbox.bak`。重写先提交临时文件，再将主文件轮转为备份；
  发布及回滚都失败时，后续存储 owner 读操作先恢复备份，不返回空队列。
- 空队列也发布一个持久的空主文件。它是权威空状态，清理未完成的旧备份不能
  让已 ACK 的记录重新出现。
- 按 128 B 块读取并识别完整换行帧；修复坏尾后才继续追加。超长或含 NUL 的坏行
  按整行丢弃，不把其后缀当作独立合法记录；保留现有的有效记录和 CRLF 兼容。
- JSON 必须完整解析；同 request_id 必须匹配 problem_id、action 和 occurred_at，
  否则返回 INVALID_STATE。字符串包含、ID 前缀或未完成帧中的 ID 都不能证明成功。
- 已写完 JSON 但缺换行的失败追加不是完整帧；重试修复后写入完整记录。

## 本地关键节点验证

| 检查 | 结果 | 证明范围 |
|---|---|---|
| `test_binary_outbox_recovery.py` / Word | 540 checks，ASan/UBSan 通过 | 生产 packed record、真实公有头、文件发布与恢复源选择 |
| 同上 / Note | 590 checks，ASan/UBSan 通过 | 追加/整理错误失效缓存，ACK/再次挂起的修复前置条件 |
| `test_problem_outbox_recovery.py` | 64 PASS / 0 FAIL，ASan/UBSan | 生产 JSON 编解码、ESP-IDF cJSON、文件故障及完整帧语义 |
| `test_word_outbox_quota.py` | 36 PASS / 0 FAIL | 物理字节配额、批量追加、缓存失效与已有故障端口 |
| `test_problem_empty_cache.py` | 58 PASS / 0 FAIL | 原 codec stub 模型下的空缓存、故障恢复与追加路径 |
| `test_word_session_replay.py` | 195 PASS / 0 FAIL | 会话、checkpoint、ACK、挂起和 owner 调用关系 |
| `test_word_ack_upload.py` | 24 PASS / 0 FAIL | 生产上传循环及退出时 ACK 处理 |
| 三份原实现作为 `--source` 输入 | 均被行为断言拒绝 | 不是仅证明新代码自己通过 |
| 8 种错误变体 | 均编译成功，运行时被断言拒绝 | 见下面的具体变体 |
| `idf.py -B build-wsl reconfigure build` | EXIT=0、零警告 | ESP-IDF 5.5.6，M8 ownership 和编译后 UI 栈门通过 |
| CI YAML 与 CRLF-aware diff | 通过 | 已接入 IDF build job；Note/Problem 保持 CRLF |
| 远端 Actions / 设备 HIL | 未执行 | 本地结果不能替代它们 |

binary 回归按函数边界提取生产实现并包含真实公有头；heap/ROM CRC/时间及 libc
故障点是 host 端口。Problem 新入口使用生产编解码及 SDK 的 cJSON 源码，原空
缓存测试仍保留 stub codec。Note 的会话 checkpoint 是接口替身，只确认修复步骤
顺序，不证明实际会话文件已正确持久化。检查数包含辅助断言，不是独立设备场景数。

错误变体：Word 的 `delete-latest` / `ignore-recovery`；Note 的 `keep-failed-cache`
/ `skip-marker-repair`；Problem 的 `delete-primary` / `substring-id` /
`skip-tail-repair` / `allow-trailing-json`。三个原实现从 `13c436b` 导出到
`/tmp/note4-outbox-before-{word,note,problem}.cpp`。

“在 rename 返回成功后中断控制流”只模拟 API 边界后的重入。它不模拟 NOR 半擦、
SPIFFS 元数据撕裂或掉电时的真实缓存行为。sanitizer 在获准的沙箱外运行，未关闭
LeakSanitizer；所有测试数据使用独立临时目录，未修改设备文件。

日志位于 `/tmp/note4-outbox-*.log`，正向最终结果为 `binary-final`、`problem-final`、
`quota-final`、`empty-final`、`replay-final`、`ack-final` 与 `final-build`；原实现和
错误变体使用对应标签。CI 新增上述五个核心 host 回归，未将其标记为设备 HIL。

## 关键节点候选身份

本地构建绑定提交 `9bdf096ba370a5fada6b5f76d88c64f533770a03`；烘焙标识为
`9bdf096b`，没有 dirty 后缀。后续本批文档提交不改变运行时代码。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,642,832 B；`34aacb44ec9bcba2241ebd6a50164b562a7af07254c224709d5160ac0643cc38` |
| ELF | 41,715,000 B；`dab063029e0b4b6cd66a31ac0d540ec7aea1c984e177cb5e44fe1fdb67f53fad` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

使用现有 100 Hz、本地内存诊断及 24 小时历史配置；不是无诊断功耗控制组。
源码 hash 清单及产物身份位于 `/tmp/note4-outbox-identity.json`。

## 当前停止点：设备验收

在可清空、已备份的受控设备上，先核对固件身份，再执行：

1. 三域分别准备多条已确认保存、尚未上传的记录，保存完整内容、ID、数量，以及
   Word/Note 的游标和挂起原因。正常整理、ACK 后重新启动，检查内容和状态。
2. Word/Note 准备“旧兼容备份 + 含更新记录与挂起标记的主文件”，在临时文件
   写入、主文件移入 `.rec`、新主文件发布及清理阶段中断；恢复后核对最新记录与
   挂起状态，不能只看能否进入页面。
3. Note 覆盖短写/flush/fsync/close 失败后，分别重试观察追加、ACK、隔离和再次
   挂起；确认游标恢复及完整 CRC 记录。受控故障注入与真实断电分开记录。
4. Problem 覆盖主文件轮转后断电、发布/回滚失败、残尾后追加、相同 ID 的同内容
   重试与不同内容冲突，以及空主文件旁残留旧备份时不会复活已 ACK 记录。
5. 在 SPIFFS 空间紧张和同步/显示并行时复核保存结果及 owner/lease；记录实际
   queue/exec 时延，不从 host 数据推断 Flash 性能或发生概率。

精确 API 边界注入需要受控测试设施；本批没有增加产品里的通用故障入口。真实断电
必须排除 USB 等其他供电，F2 reset、串口重新附着或 host 异常不计掉电证据。
确认本节点后再进入 R-01/R-07 与 B3 的 RTC/I2C 修复；已有 B1/B2 证据按各自
候选保留，不默认重复整套历史长测。
