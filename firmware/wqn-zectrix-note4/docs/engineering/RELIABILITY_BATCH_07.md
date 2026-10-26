# 第七批：睡前 RTC 预算与时间消费者审计

开始：2026-10-22；节点交付：2026-10-23（Asia/Shanghai）。
分支：`fix/firmware-reliability`。起点：`18cdd9d`。

> 当前需要执行的验收见[统一验收清单](FIRMWARE_RELIABILITY_ACCEPTANCE.md)；本文保留历史范围与证据，不要求逐批重复设备矩阵。

## 状态与交付

用户确认第六批“通过 继续”，本节点记为用户确认验收；未附新设备日志，不补录
该批矩阵的逐项结果。本批继续 B3 / REL-004 / REL-005，沿用分批本地提交、节点
构建和实机验证的节奏。

用户随后回复“通过，继续”，第七批节点记为用户确认验收通过；未附新日志，不
补录精确注入或耗时结果。现继续[第八批来源与计时恢复](RELIABILITY_BATCH_08.md)。

| 提交 | 内容 | 状态 |
|---|---|---|
| `7c88893` | prepare deadline 接入 RTC persist；STOP 不确定结果的清理；澄清 trust 注释 | host 通过后本地提交，SDK/HIL 留节点 |
| `1350856` | 完整生产 RTC/时间保全回归、13 个反向控制、历史实现适配 | host 通过后本地提交，SDK 留节点 |
| 本记录所在提交 | 第六批确认、时间消费者现状、候选身份和实机矩阵 | 文档节点 |

## 两个确认的失败路径

1. `PowerCoordinator` 的服务准备和 wake 装配已有绝对截止时间，但中间的
   `PersistSystemTimeToRtc(generation)` 没有接收它。RTC 写钟持锁后，STOP、burst、
   release 和 readback 各自使用 100 ms，截止时间已耗尽仍会开始新操作或发布
   本轮 trust。逐操作有限等待不能证明遵守整轮约定。
2. 旧 `Pcf8563WriteTime` 在 STOP set 返回错误时直接退出，STOP release 返回
   错误时也没有后续清理。一次驱动超时允许出现芯片已应用写入的结果；代码不能
   据此认定 prescaler 仍在运行。burst 失败虽已有清理，但没有覆盖这些交界。

主机端口注入上述结果，证明生产函数的分支及资源/记录处置；没有据此推断现场
曾经发生停钟事故，也不估计电气故障的概率。

## 实现与正常路径

- 保留原无 deadline 的公有入口，新增绝对 `esp_timer` deadline 重载。实际 idle
  sleep 入口传入 `command.deadline_us`，不重新分配整轮预算。
- 写钟入口和等锁之后检查预算；锁参数最多为剩余整毫秒与 100 ms 的较小值再转
  tick。每次驱动操作使用剩余整毫秒与 100 ms 的较小值；不足 1 ms 不开启事务。
- STOP、burst、正常 release、readback 逐步重算剩余预算，并拒绝晚完成。
  persist 仍先撤销旧 trust，写钟返回后再核对截止时间，避免日志/调度延后使
  过期结果成为新 generation 的恢复依据。
- 一旦 STOP 已尝试，失败/逾期路径保留一次单独的 STOP-off 清理，驱动 timeout
  为 100 ms；正常 release 自身失败也允许这一清理。清理失败保留错误日志和
  `RecordError`，函数返回 false，不发布 trust。
- 正常 release 已成功但返回晚时，不重复释放；readback 失败时 prescaler 已
  释放，同样不再增加清理。成功路径仍是一次总线锁、四次传输，payload/顺序
  和原日历字段不变，没有新增 `vTaskDelay`。

STOP → burst → release → verify 全程持有原递归共享锁，清理使用同一锁，防止
RTC 暂停期间插入 codec 事务。本批没有修改 ADC 采样调度或 UI 刷新节奏。

该 deadline 是协作式工作预算。STOP 清理必须有机会发生，允许超出工作截止；
SDK 内部重复等待和调度也不能由 100 ms 参数证明硬实时 WCET。若清理仍失败，
硬件可能继续停钟；本补丁记录失败并拒绝恢复信任，不承诺自动修复物理总线故障。
持久化本身失败沿用非致命政策；外层 wake 装配若已超时，仍按原流程 rollback。

## 时间消费者现状和剩余路线

下表核对的是当前生产调用链，相关行为本批保持。它说明现有契约及剩余工作，不
把合理年份、I/O 完整性与网络校准合并为一个已经完成的可信度结论。

| 消费者 | 当前依据与行为 | 后续边界 |
|---|---|---|
| 启动装配 `main.cpp` | shared bus / WakeContext → RTC restore → build seed，早于 sync/UI；不合理年份才 seed | build seed 是近似墙钟，设备没有时间来源快照 |
| RTC restore / persist | 深睡、有效快照、generation/checksum、VL 和日期范围通过后恢复；任何合理墙钟都可持久化 | trust 证明设备写入与代次一致，也可能承接 build seed；不证明曾经 SNTP 校准 |
| HTTPS `wqn_api.cpp` | `IsClockReasonable` 只检查 2024 下界；合理 build seed 可跳过同步等待；仍使用证书 bundle | 保留现有联网策略；下一步单独处理来源分级、校准失败与 TLS admission，不在本批改变全部请求的前置条件 |
| WiFi `wifi_manager.cpp` | GOT_IP 后主动 `StartSntpOnce`，同步回调记录时间；started 代表初始化，不代表校准完成 | 后续来源状态需来自实际校准事件；避免把已启动作为已同步 |
| 计时 `time_app.cpp` | 当前开机按单调 elapsed 扣减，墙钟修正只移动显示的 phase/session 边界；暂停不扣减 | 保留已有 110 项组合回归；不以墙钟跳变替代经过量 |
| `time_app_retention.cpp` | 跨睡眠运行态使用墙钟差值，回拨恢复值不超过已存 remaining；暂停保留 | RTC restore 被拒绝时可能使用近似墙钟；来源分级后的降级恢复政策仍需补齐 |
| UI 日期/Todo 和 Word/Note/Problem 时间戳 | 使用 reasonable 年份；学习记录无法生成时间戳时沿用 `2024-01-01` 哨兵值，接受 build seed | 时间来源/不确定性尚未进入记录契约；处理前需核对云端约定和历史记录，不在固件单侧改格式 |
| Sync retry / 临时文件年龄 / 诊断 | retry 转换本次单调期限与跨睡眠墙钟，并有最大延后；临时文件按 mtime 年龄；诊断记录墙钟与 uptime | 保留现有各自降级路径；不能仅从年份门推断真实日期或跨启动年龄 |

REL-004 因此保持部分完成。下一阶段先选择最小来源表示，明确合理近似时间可用
于哪些消费者、何时需要实际校准，以及失去连续性时怎样恢复计时和 sync。涉及
学习记录来源的协议扩展应独立协调，不能让它阻塞已经确认的本地预算补丁。

## 本地验证与证据

| 检查 | 结果与范围 |
|---|---|
| `test_rtc_timekeep.py` | 100/1000 Hz 各 2,272 checks，ASan/UBSan 通过 |
| 新增失败/边界 | 已过期、亚毫秒余量、锁拒绝/晚获取、各步骤晚完成、STOP 写入已应用但返回错误、清理再失败、driver 返回后日志延后、旧/新代次均不得恢复失败记录 |
| 正常时序 | 125 ms 预算、每次驱动注入 30 ms，收到 `100/95/65/35 ms`；一次锁和四次传输；这是端口模型，不是 ESP32 性能数据 |
| 历史实现与 13 个错误变体 | 均编译成功，被运行时行为断言拒绝 |
| wake/RTC 回归 | 100 Hz 1,793 / 1000 Hz 1,802 checks 通过 |
| 完整 codec 回归 | 100/1000 Hz 各 4,411 checks 通过 |
| 计时/保留状态 | 110 checks 通过 |
| ADC/UI 完整批次 | 100/1000 Hz 各 535 checks 通过 |
| SDK 构建 | ESP-IDF 5.5.6，EXIT=0，零警告，M8 / 编译后 UI 栈门通过 |
| Python AST / CI YAML / 文档链接 / CRLF-aware diff | 通过 |
| 本地重跑的设备 HIL / 远端 CI | 未执行；用户随后确认节点，逐项结果不补录 |

主回归完整编译生产 `pcf8563.cpp`、`rtc_timekeep.cpp`、`i2c_bus_lock.cpp`，使用
真实头文件；codec 寄存器函数继续按原 fixture 提取。驱动、clock、semaphore、
wake-context、系统设时和 RTC slow memory 是明确的 host 端口。sleep coordinator
的 deadline 传递检查是源码边界检查，没有在主机运行整个 PowerCoordinator。
所有测试保留 sanitizer，没有关闭 LeakSanitizer。

历史文件：`/tmp/note4-rtc-persist-before-{pcf,timekeep}.cpp`，来自 `18cdd9d`。
runner 为缺少新重载的旧源码附加转发适配，忽略 deadline 并调用完整旧函数，使
失败来自行为断言，而不是链接错误。主日志 `/tmp/note4-rtc-persist-final.log`；
历史及各 mutation 日志使用相同前缀和对应 case 名，可能被系统清理。现有独立
`rtc-timekeep` CI job 自动运行扩充后的测试，本轮没有运行远端 Actions。

新增反向控制：`ignore-write-budget`、`ignore-persist-budget`、`late-persist-trust`、
`omit-stop-cleanup`、`stop-error-no-cleanup`、`ignore-late-write`；另七个原控制
继续验证单位、有限等待、信任撤销、generation/snapshot、系统设时及 codec 单位。

## 候选身份

构建基于干净运行时提交 `1350856da9880cef36afc07b27eda1ff18446e38`，烘焙标识
`1350856d`，无 dirty 后缀；随后仅更新文档。本节点 ESP-IDF v5.5.6
`idf.py -B build-wsl reconfigure build` EXIT=0，零编译警告，M8 ownership 与
编译后 UI 栈门通过。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,644,960 B；`d7c1aa2d8e28621660dac6ed42beb9b0aef152591a4f0cb9bf87a3d80081aab3` |
| ELF | 41,734,784 B；`80730bd72f41aa693e3f667f2ffda6d8a792020d198e08d3dfc76c9d45600edc` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

产物在 `build-wsl/`，源码/产物清单为 `/tmp/note4-rtc-persist-identity.json`，
构建日志为 `/tmp/note4-rtc-persist-build.log`。配置仍为 100 Hz，RTC_TIMEKEEP 和
原诊断配置保留。重新构建可能改变时间戳/摘要，设备侧应核对实际镜像身份。

## 用户确认节点及原设备矩阵

本节点涉及睡前 RTC 写入及异常清理，保留候选标识和串口日志后执行：

1. 已有有效墙钟时断网，睡眠/唤醒两轮，确认 persist/restore 和 generation 配对、
   时钟继续走动；再联网 SNTP 校准，检查运行/暂停计时仍正常，交互保持流畅。
2. 测试设施中让 prepare 剩余预算不足、共享锁被占用或 SDK 返回晚成功，核对
   新工作被拒绝、本轮 trust 不成立，以及外层 sleep rollback/后续重试。
3. 受控注入 STOP set、burst、正常 release 的 NACK/超时，单独覆盖写入已生效
   但驱动报错的窗口；确认尝试 STOP-off，失败有日志且不会将该轮标成可恢复。
4. 清除注入后检查 RTC 走时、codec 录音和再次睡眠；持续硬件错误不要求软件
   凭一次调用保证恢复。普通成功操作不替代上述故障注入结果。

用户常规验收和精确故障窗口分开记录，沿用既有录音长测证据，避免默认重复十轮。
本节点已获用户确认，第八批继续 REL-004 来源分级/恢复政策，剩余共总线故障恢复收口，再推进
R-02/R-03/R-08 传输专项以及 B4/B5。B3 尚未整体关闭。
