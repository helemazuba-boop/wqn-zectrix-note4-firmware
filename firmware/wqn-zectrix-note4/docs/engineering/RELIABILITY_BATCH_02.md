# 第二批：启动恢复的数据保全

日期：2026-10-22。分支：`fix/firmware-reliability`；起点：`52bb42b`。

## 范围与决策

本批落实 B2 / REL-003 的保全行为，保持 generation 3、业务文件格式、分区表和
StorageService / 启动门的所有权。B1 音频与本批存储 HIL 仍须分别验收。

| 启动状态 | 本批策略 |
|---|---|
| 当前 generation、有效 reset_id、无重置意图 | 校验 SPIFFS 后 Ready，不擦除、不格式化、不提交 marker |
| NVS 初始化的任意错误，包括 NO_FREE_PAGES / NEW_VERSION_FOUND | 保留介质，返回原始错误，RecoveryRequired |
| marker 读取失败，或重置意图读取失败/长度/头部异常 | 保留介质，RecoveryRequired |
| 未来 generation，包括同时存在旧重置意图 | 保留介质，禁止降级固件执行重置 |
| 无有效重置意图的旧 generation 1/2、缺失或不完整 marker | 不再推定可丢弃；RecoveryRequired，维护时先保护数据再决定迁移；明确空介质除外 |
| 无 marker / 意图，NVS 枚举及 stats 确认无值，且 storage 分区逐字节全为 0xff | 明确空介质，先提交重置意图，再初始化 |
| 有明确、严格解析且目标为 generation 3 的重置意图 | 重放授权重置；完整成功后请求一次重启 |
| generation 0 且原 16 B reset_id 有效、无意图 | 兼容上一版明确的 factory-reset 请求；先升级为新重置意图 |

generation 0 兼容入口只接受完整的旧请求形状；generation 0 缺/坏 reset_id 不授权
重置。旧 generation 1/2 的一次性全量清空承诺不再自动延续到本保全版本。
旧固件在擦除 NVS 后留下的“marker 缺失 + 非空 SPIFFS”无法区分授权重置与损坏，
本版本保留现场并进入恢复态，不自动续擦。

## 重置意图与中断协议

新键 `wqn_boot/reset_pending` 是固定长度 blob，包含 magic、协议版本、目标
generation 和本次 16 B reset_id。严格检查完整长度和头部，不把坏 blob 当作请求。
恢复出厂入口只写该单键意图；不再先把 generation 改为 0。

1. 确认并提交意图，发生失败则本次调用不进入破坏步骤。
2. 在启动门内枚举默认 NVS，按命名空间 erase_all + commit，保留 `wqn_boot`。
   在修改前释放迭代器；任何错误立即阻断启动。保留 NVS 的命名空间元数据。
   再用 NVS stats 和 boot 命名空间实际条目数交叉核对；迭代器未暴露的 legacy
   blob 等遗留项会阻断重置，不冒充清理完成。
3. 格式化 storage SPIFFS，再以 `format_if_mount_failed=false` 校验。
4. 写入 generation 3 + 意图绑定的 reset_id，并 commit。
5. 最后删除重置意图并 commit；只有全部成功才请求重启。

因此本批取消启动时的 `nvs_flash_erase`，重置依据不会随默认 NVS 全分区擦除而
消失。任一步失败/中断，只要意图仍有效就可重放；在意图清除之前业务始终不启动。
意图删除已经生效、但 API 返回失败时，下次启动可通过最终 marker 正常校验；
删除意图只发生在所有破坏步骤及最终 marker 提交成功之后。

NVS 多键 set/commit 不是跨键事务。ESP-IDF 5.5.6 的 set/erase 可立即生效，测试
必须覆盖即时写入和延迟提交两种可见性。特别是提交返回失败不证明意图未落盘，
失败的主动重置请求可能在下次启动重放；这仍属于已经明确请求的重置，不得据此
自动重新创建业务写入。测试不声称保证 NOR 部分擦除或底层 NVS 原子性。

空介质检测只在完全缺失 marker 与意图时执行：NVS 无键，且完整读取 storage
分区确认全 0xff。读取失败、非空字节、缺分区都返回恢复态；挂载失败本身绝不
作为空介质证明。正常启动不扫描 8 MiB 分区。

按命名空间清理不能代替全分区擦除修复损坏的 NVS，也不回收空命名空间的编号；
NVS 容量或介质错误仍需维护处理。本批不提供异常现场的数据导出/离线迁移工具。
较老固件不了解新意图，未完成的重置不得直接回退至旧固件；先用本候选完成受控
恢复或在维护环境保护现场，再决定回退。

## 验证与提交状态

实现及下列本地验证已完成。2026-10-22，用户确认“验收通过 提交现有”，批准将
本批存储行为、配套回归及文档分批提交。REL-003 按用户确认的验收结论收口；
B3 尚未开始，REL-008 的容量/尾延迟验证仍留在 B5。

设备验收结论来源为用户确认。代理本轮核对源码、配置、bin/ELF 和完整 review
patch 与上一轮候选一致，重新执行构建和两项重点回归；没有代为重做设备验收，
也未新增设备日志、端口或逐场景测量数据。本轮未 push、创建 PR 或执行远端 CI。

| 本地提交 | 内容 |
|---|---|
| `1b95015` | 存储保全行为、持久重置意图与真实实现/启动门回归 |
| 本记录所在的文档提交 | 第二批决策、验收来源、本地验证、候选身份和路线台账 |

| 检查 | 结果与边界 |
|---|---|
| `python3 scripts/test_storage_schema.py` | ASan/UBSan 10,230 检查、194 场景通过；分别执行延迟提交与即时写入模型 |
| 第一批原实现 `--source /tmp/note4-batch02-original-storage-schema.cpp` | 被新保全回归拒绝，触发禁止全分区擦除的断言 |
| schema 7 个错误变体 | 全部编译成功后由断言拒绝：accept-stale-schema、ignore-reset-error、auto-format、ignore-intent、allow-future-reset、nonempty-is-fresh、drop-intent-early |
| `python3 scripts/test_sync_boot_recovery.py` | UBSan 42 项通过：WiFi/Agent 开启 17、Agent 关闭 17、WiFi 关闭 8；新增 schema 恢复和重启启动门检查 |
| startup 4 个错误变体 | late-journal-gate、ignore-sync-start-failure、ignore-schema-recovery、ignore-schema-restart 全部由行为断言拒绝 |
| `python3 scripts/test_audio_channel_timeout.py` | 100 Hz 和 1000 Hz 各 1,222 项 ASan/UBSan 检查通过；不代替 B1 实机录放验收 |
| `idf.py -B build-wsl reconfigure build` | ESP-IDF v5.5.6，EXIT=0、零警告，M8 ownership 与编译后 UI 栈门通过 |
| M8 gate fixtures | 11 passed / 0 failed |
| Markdown 围栏与相对链接 | 3 个变更文档、50 个相对链接通过 |
| CRLF-aware diff 检查 | 通过；存储调用点只修改日志，main 启动顺序不变 |
| 本批设备验收 | 用户确认通过并授权提交；逐项原始设备证据未在本轮追加，不冒充代理复测 |

存储测试链接整个生产 `storage_schema.cpp` 及真实公有头文件，NVS/SPIFFS/分区/
SleepLease 是故障端口；测试逐项核对模拟凭据、三类 outbox 字节内容、重置意图、
迭代器与句柄释放。覆盖坏/短意图、future schema 加残留意图、空分区首/中/末
非空字节、分区读取故障、遗漏 legacy blob、最终 marker 只写一键、意图删除后
返回错误等。startup 测试注入 schema 返回值验证真实 `app_main` 的门，不冒充
真实 schema + 真实硬件的端到端测试。

初次 ASan 运行在沙箱 ptrace 下遇到 LeakSanitizer 环境限制，获准在沙箱外重新
运行存储和音频回归后通过；没有关闭 sanitizer 或将失败算作通过。
构建/启动/音频/M8 日志分别为 `/tmp/note4-batch02-build.log`（reconfigure）与
`/tmp/note4-batch02-final-build.log`（最终日志文字修正后重建）、
`/tmp/note4-batch02-sync-boot.log`、`/tmp/note4-batch02-audio.log`、
`/tmp/note4-batch02-m8.log`。schema 正向/原实现/7 个变体结果保留于本次工具会话，
以上命令可复现。

提交前重新执行 `idf.py -B build-wsl build`、真实 schema 回归与 startup 回归，
结果分别记录于 `/tmp/note4-batch02-precommit-build.log`、
`/tmp/note4-batch02-precommit-schema.log` 和
`/tmp/note4-batch02-precommit-startup.log`；均通过，构建零警告且 M8/UI 栈门通过。

### 候选身份

原验收候选以 `52bb42b96354d1daf71467a64c3735bdb779fdde` 加工作区变更构建，
烘焙标识为 `52bb42b9-dirty`。下面保留原验收产物身份，不将其标成提交后重新
构建的固件。SDK 与第一批一致，使用现有本地
`sdkconfig` / `build-wsl`（100 Hz、内存诊断及 24 小时历史开启）；身份清单在
`/tmp/note4-batch02-identity.json`。

| 对象 | 字节数 / SHA-256 |
|---|---|
| app bin | 3,641,328 B；`b066bf68cdf2e51d42d62125de14714deecd8f7524b7b1ae01a2a11b378cfca9` |
| ELF | 41,718,756 B；`2a53abdf7773eadf44e5d5be8dfed4da34913e79c80e25c777f318a96558b0bc` |
| `sdkconfig` | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |
| `main/runtime/storage_schema.cpp` | `eb8dbdc4c9d652595fd5b641b35fbc8326e5dde95c64d3b6397ff5f3f77cc129` |

提交前已重新核对上述 hash 与候选源码清单；用户的验收通过及提交授权作为
`AGENTS.md §7` 的验收/评审依据。存储实现和配套回归作为一个逻辑提交，文档及
本记录另提交。后续若需生成烘焙新提交号的固件，先 reconfigure 并记录新的产物身份。

## 设备验收范围

以下保留本批提出的设备验收矩阵，结论见上面的用户确认记录；不在缺少逐项
原始日志时补填逐格 PASS。复现时使用可清空的受控设备，先保存 NVS/SPIFFS 备份
并记录候选 hash：

- 原 generation 3 升级：核对 WiFi/token、内容、Word/Note/Problem 未上传 outbox
  的内容与条数，确认无 erase/format；重启后仍可读。
- 受控注入 marker 读取异常、未来 generation、坏/缺 marker：进入恢复态；核对
  数据内容与介质备份，确认未启动业务、未格式化。
- 全空介质：初始化完成后仅预期重启一次，再次启动 Ready。
  观察首次 8 MiB 扫描期间的启动耗时、主任务栈余量及 watchdog，不能用 3,073 B
  host 分区扫描替代设备证据。
- 明确恢复出厂：全部业务命名空间及 SPIFFS 清空，保留完整 schema，无残留意图。
- 在意图提交后、逐命名空间清理中、SPIFFS 格式化中、最终 marker 两键写入之间、
  意图删除前后分别真实断电；恢复后完成授权重置或停在恢复态，禁止带未完成意图
  启动业务。软件 reset、USB 串口重连不能替代真实断电。
- 原 generation 0 + 有效 reset_id 的授权请求可完成；旧 generation 1/2 及旧协议
  中断的歧义现场按上述策略保留，不声称自动迁移。

B1 针对性音频 HIL：预热、普通录放、取消后立即重试、EPD 刷新并发。复用既有
长时间 RAM 证据作为历史背景，不将其算作本候选验收，也不默认重复长测。
