# 第八批：时间来源与保守计时恢复

日期：2026-10-23（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`6511b7a`。

## 状态与范围

用户确认第七批“通过，继续”，本批继续 B3 / REL-004。此前设备节点记为用户
确认验收，未附新日志，不补录精确故障注入或逐项结果。

| 提交 | 内容 | 状态 |
|---|---|---|
| `c4d7fb3` | 原子来源/epoch、WiFi/HTTPS 实际同步回调与系统时间读回校验 | host 通过后本地提交 |
| `c9554d3` | RTC 保留来源、计时相对锚点与失去连续性的降级恢复 | host 通过后本地提交 |
| `60e355f` | 完整回调/RTC/计时链路、反向控制及 CI 描述 | host 通过后本地提交；SDK 留节点 |
| `c678e8b` | SNTP 与 RTC 单调推算一致时保留连续性，避免联网唤醒反复跳过计时 | 最终 host / SDK 构建通过，设备节点待验收 |
| 本记录所在提交 | 前批验收、候选身份、策略与设备矩阵 | 文档节点 |

## 确认的问题与采用的策略

旧恢复只要当前时间达到 2024 下界，就用 `phase_end - now` 扣除剩余量。第五批
已限制回拨时不能增加预算，但 RTC 恢复被拒绝后，向前的 build fallback 或
SNTP 跳变仍能把已保存的 120 s 直接变成到期。另一个交界是保存计时之后、写
RTC 之前发生校时，两份状态可能使用不同时间基准。

本批以“能确认连续性才扣除未知睡眠经过量”为策略：

- RTC 恢复成功、保存计时与 RTC persist 的 epoch 一致、捕获时间合理时，用
  相对锚点扣除经过量；回拨不增加余额，耗尽才进入 alert。
- RTC 恢复失败、使用 build fallback、先收到改变时间基准的 SNTP 校时，或两份记录的
  epoch 不一致时，保留已保存的剩余秒数，立即重建显示的结束时间。
- 降级保留可能延长计时；它不能证明设备实际睡了多久。本次开机的运行/暂停
  扣减继续依靠原单调时钟，不用墙钟跳变代替已经经过的秒数。

## 来源、epoch 与记录

`runtime/wall_clock` 用一个 32-bit atomic word 同时发布来源、28-bit epoch 和
RTC 连续性标志，不新增任务、动态分配或 I/O。来源包括 unknown、build、SNTP、
RTC approximate 和 RTC SNTP-derived。实际 build 应用或无法确认 RTC 连续性的合格同步推进 epoch；
RTC 恢复沿用写入前的 epoch。

WiFi 的真实 `TimeSyncCallback` 和 HTTPS fallback 配置的 callback 都接入此
状态。仅初始化 SNTP 不升级来源；回调为空、年份不合理、系统读回与目标相差
超过 2 s 时保留原来源。这是观测到同步并读回相符，不证明 SNTP 来源经过认证、
RTC 精度或该次校准永不过期。RTC 恢复时另外保存墙钟/单调时间锚点；同步与其
推算值相差不超过 2 s 时保留 epoch 和连续性，升级来源后仍扣除睡眠经过量。
明显跳变才撤销连续性，避免正常联网唤醒每次都延长计时。当前 SDK 默认 immediate
sync；没有新增 smooth
sync 策略，也没有修改服务器。

RTC persist 保存 epoch 和是否观察到网络校准，校时在采样/I/O 期间改变 epoch
则拒绝发布本轮 trust。恢复仍先验证深睡、快照、generation、checksum、VL、
日期范围和系统设时结果，成功后才发布 RTC 来源。build seed 经过多轮睡眠仍
保持 approximate；日志新增 `basis` / `calibrated` 便于核对，不将一次 RTC
读回直接升格为新 SNTP 校准。

计时保存新增相对锚点和 epoch。锚点对齐到该余额最后一次单调扣减的时刻，包含
按钮在两次 tick 之间触发保存时尚未扣除的清醒秒数，避免给正常计时额外加时。
保存期间观测到 epoch 变化时，锚点失效；恢复后立即调用生产的纯重对齐函数，
首帧就使用新结束时间。暂停、手动确认阶段转换和原参数范围继续沿用。

RTC volatile 格式分别升为 trust v2 / timer v4，旧版本按校验规则拒绝，不迁移
旧布局。Flash/NVS 中的离线学习记录和协议格式保持兼容；设备验收应在新固件
上建立新计时状态，再验证其跨睡眠恢复。

ELF 符号核对：trust record 16 → 24 B、timer record 136 → 152 B，共新增
24 B RTC 符号数据；普通 RAM 为来源 word 4 B 和两个 RTC 锚点各 8 B，共 20 B。
没有新增同步等待。尺寸核对不代表实际延迟或功耗结论，ADC/UI 完整批次回归继续
执行。最终符号记录为 `/tmp/note4-clock-policy-symbols-final.log`。

## 消费者边界和剩余路线

| 消费者 | 本批政策 |
|---|---|
| HTTPS | 保留 reasonable 年份准入，允许 build/RTC 近似时间，证书 bundle 与现有等待预算保留；不额外要求所有请求先等 SNTP |
| 计时 | 当前开机使用单调经过量；跨睡眠需要 RTC 连续性及同一 epoch；未知间隔保留余额 |
| UI/Todo 与学习时间戳 | 继续允许合理近似时间及现有哨兵；不在固件单侧新增云端时间来源字段 |
| Sync / 文件年龄 / 诊断 | 继续沿用第七批已记录的 reasonable/fallback/上限约束；未将新来源字段接入全部消费者 |

REL-004 的来源基础和计时恢复已补齐，其他消费者更严格的来源/新鲜度政策需独立
协调。REL-005 的持锁与 deadline 已有分批补丁，持续 SDK 清理失败和精确硬件
故障窗口仍保留证据边界。本节点确认后优先进入 R-02/R-03/R-08 传输专项，B4/B5
及剩余来源联动仍在总计划跟踪，不把本批写成整个 B3/整机可靠性已关闭。

## 本地验证

| 检查 | 结果与范围 |
|---|---|
| 组合 `test_rtc_timekeep.py` | 100/1000 Hz 各 3,325 checks，ASan/UBSan 通过 |
| 正常链路 | approximate / SNTP-derived 各连续睡眠两轮，余额 120 → 90 → 60；近似来源不升级；联网无跳变也扣除睡眠经过量 |
| 失败/交界 | RTC VL 拒绝后向前 fallback、保存后才校时、RTC 写入期间校时、恢复后先校时、校时早于保存但显示时间未重对齐、两次 tick 间保存包含 5 s 清醒经过量；延后 7 s 的 SNTP 回调及 ±2/±3 s 边界 |
| 来源准入 | 初始化不升级、空/错误回调不升级、系统读回未应用目标不升级、HTTPS 合理 build 直接通过、fallback 成功/失败与其他初始化方 |
| 23 个错误变体 | 均编译成功，被运行时行为断言拒绝 |
| 两份历史实现 | 分别替换 timekeep / retention，均编译成功，被运行时行为断言拒绝 |
| 独立计时/保留状态 | 111 checks 通过，含旧 volatile 版本拒绝 |
| ADC/UI | 100/1000 Hz 各 535 checks 通过 |
| wake/RTC | 100 Hz 1,793 / 1000 Hz 1,802 checks 通过 |
| 完整 codec | 100/1000 Hz 各 4,411 checks 通过 |
| SDK 构建 | ESP-IDF 5.5.6，EXIT=0，零警告，M8 / 编译后 UI 栈门通过 |
| Python AST / CI YAML / 文档链接 / CRLF-aware diff | 通过 |
| 新设备 HIL / 远端 CI | 未执行 |

组合回归执行完整生产 `rtc_timekeep.cpp`、`pcf8563.cpp`、`i2c_bus_lock.cpp`、
`wall_clock.cpp`、`time_app.cpp` 和 `time_app_retention.cpp`，还执行 WiFi 的
完整 SNTP 初始化/回调函数及 HTTPS 的完整 clock admission 函数。公有类型和
预算类使用真实头文件。driver、RTOS、clock、SNTP init/wait、wake-context、
watchdog、系统设时及 slow memory 是明确端口；BSS 重置在 fixture 中模拟，
没有为产品新增测试 reset API。sanitizer 保留 LeakSanitizer。

这些结果证明受控交界的代码决策，不证明 SDK 实际 settimeofday 与回调构成原子
事务，也不证明任意 SMP 抢占窗口或真实网络/振荡器时序。单个原子快照解决字段
一致性，不能据此声称系统时钟与回调元数据的采样天然原子化。

最终主日志：`/tmp/note4-clock-policy-{rtc,time}-final.log`，既有回归日志使用
`/tmp/note4-clock-policy-{power,wake,codec}.log`；历史文件为
`/tmp/note4-clock-policy-before-{timekeep,retention}.cpp`，来源 `6511b7a`。
旧实现适配和既有的 deadline 兼容端口继续保留。错误变体日志使用同前缀与
mutation 名及 `-final.log` 后缀；临时文件可能被系统清理。CI 继续使用现有
`rtc-timekeep` job，
更新描述后自动覆盖完整组合测试，本轮没有运行远端 Actions。

## 候选身份

构建基于干净运行时提交 `c678e8b20f4a1010afce00976b432513f8359142`，烘焙标识
`c678e8b2`，无 dirty 后缀；随后仅更新文档。ESP-IDF v5.5.6 的
`idf.py -B build-wsl reconfigure build` EXIT=0，零编译警告，M8 ownership 与
编译后 UI 栈门通过。中间实现 `60e355f` 未交付为最终候选，匹配 SNTP 的连续性
由 `c678e8b` 补齐后完成最终构建。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,645,936 B；`b0cfa8c9c9b7b09ce20c20607112cbe8b8baf19e93926d94e4729847b298a81c` |
| ELF | 41,755,400 B；`ca6e576d72560426c633d7d38888c02fc2aa155e7350d9e8edabaeceebc50e73` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

产物在 `build-wsl/`，源码/产物清单为 `/tmp/note4-clock-policy-identity.json`，
最终构建日志为 `/tmp/note4-clock-policy-build-final.log`。配置保持 100 Hz 及原
RTC/诊断设置。重新构建可能改变构建时间与摘要，应核对设备实际镜像身份。

## 当前设备节点

1. 在新固件上新建运行和暂停计时，记录候选身份、剩余秒数及 RTC 日志。
2. 已经校时后断网，连续睡眠/唤醒两轮，核对 generation/basis/calibrated；
   再覆盖尚未联网校时的 build/RTC approximate 路径，不将后者标成已校准；
   联网且 SNTP 与 RTC 走时一致时，计时同样扣除睡眠经过量。
3. 运行/暂停中联网收到 SNTP，观察余额、显示结束时间及阶段切换。反复页面和
   按键操作，确认交互流畅、分钟刷新维持原节奏。
4. 在测试设施受控拒绝 RTC 恢复，再让 fallback 时间向前或向后跳变，核对余额
   保留、首帧结束时间重对齐及后续单调扣减；精确故障窗口单列。
5. 覆盖保存后/RTC persist 期间校时，核对 epoch 不匹配时降级或拒绝 trust；
   并验证普通离线学习、首次 HTTPS、录音及后续睡眠仍可用。

常规验收与精确注入分别记录；没有新日志时只记录用户节点确认，不补录每项 PASS。
