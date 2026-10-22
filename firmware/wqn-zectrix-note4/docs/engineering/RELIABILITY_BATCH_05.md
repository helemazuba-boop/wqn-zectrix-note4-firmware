# 第五批：唤醒截止时间、短等待与计时恢复

日期：2026-10-22。分支：`fix/firmware-reliability`。起点：`755d87e`。

**验收更正：第五批设备验收失败。** 用户反馈严重卡顿并提供 COM7 日志；
已确认 `6071ef4` 的 ADC 等待放大了同步 UI 电池读取成本，该部分已在 `0feba06`
撤回。原 host/build 通过不构成设备验收通过。根因、替代回归和复验候选见
[第五批回归修复记录](RELIABILITY_BATCH_05_HOTFIX.md)。以下保留原候选的交付记录。
响应性修复节点随后获用户确认通过；原候选失败和其余未补测矩阵仍分别保留。

## 范围与交付节奏

用户对第四批回复“通过 继续推进”，本批继续 B3 / REL-002、REL-004、REL-005。
沿用用户授权的“分批本地提交、关键节点集中验证”节奏；没有修改 AGENTS.md 的
通用门禁。第四批记为用户确认节点通过，未补造逐场景设备日志。

| 提交 | 内容 | 提交时验证状态 |
|---|---|---|
| `ec4d6c7` | 唤醒、RTC 每步等锁/传输、timer 回退及回滚共用截止时间；保证 GPIO 最小等待 | 回归及构建延后至本节点 |
| `6071ef4` | EPD BUSY 阻塞让步；EPD status 和 ADC 采样使用最小实际等待 | 回归及构建延后至本节点 |
| `2e94def` | 恢复计时不超过睡前保存的剩余预算 | 原实现行为复现已失败；最终回归/构建随后执行 |
| `96bf4e6` | 完整生产源码回归与 CI；修正旧的“wake 只等约 4 ms”注释 | host 与反向验证通过；随后构建通过 |
| 本文档所在提交 | 候选身份、证据与设备矩阵；记录第四批用户确认 | 文档检查；不改变运行时代码 |

代码、host 和构建检查已通过。本批改变 wake/EPD/ADC 等硬件等待，需要下面的设备节点；
没有把局部补丁算作整个 B3 已关闭。远端 Actions、刷机和新的设备测试未执行。

## 唤醒装配共用截止时间

原 `ArmWakeSources` 只在两段延时之后检查截止时间。RTC 每个寄存器步骤、timer
失败后的清理和回滚仍能重新取得 100 ms 参数，EXT1 最后装配逾期也能返回成功。

本批增加有截止时间的 RTC timer overload，保留原来的无参数/单参数接口给旧
调用者。正数 deadline 使用绝对 `esp_timer` 微秒时间；旧接口仍按原有单步限制。

1. 每次寄存器操作前检查 deadline；共享锁的 tick 等待不超过剩余预算和原 100 ms。
2. 取得锁后重新计算驱动的毫秒参数，限制在剩余整毫秒和原 100 ms 内；不足
   1 ms 时不发起传输。驱动返回后再次检查，逾期成功按超时处理。
3. 初次清 flags、timer 编程、失败清理和回滚使用同一个绝对 deadline。预算耗尽
   后不重新开始 RTC I/O。ESP32 timer 和 EXT1 在失败回滚中禁用，轻睡按键唤醒恢复。
4. 两次 GPIO 稳定等待开始前预留完整的 RTOS 粗粒度等待预算，等待后检查逾期。
   GPIO 采样和最后 EXT1 配置也检查截止时间，逾期不能形成成功装配结果。

这是协作式截止时间：SDK 5.5.6 的同步 I2C 实现会把同一个 timeout 用于内部 mutex、
命令、事件队列以及 NACK 后 BUSY 等多个等待。一次已经开始的驱动调用仍可能
越过 deadline，任务调度也可能延后返回。本批保证参数、步骤准入和逾期结果拒绝，
没有声称能强制中断驱动或证明整段函数的硬实时最大执行时间。

逾期时若外部 RTC 已部分装配，清理不能获得新预算；日志会报告清理失败或预算
耗尽。这次睡眠不能提交，下次装配仍先清理 AF/TF。PowerCoordinator 外层回滚
还保留独立清理调用；整个电源事务、persist 写墙钟和 codec 编程的统一硬期限
没有在本批全部实现。codec 的 5 个默认无限等锁入口仍按 REL-005 继续审计。

## 6 处短等待的语义

新增 `runtime/task_delay.h`：物理最小等待使用 `ceil(ms × Hz / 1000) + 1` tick。
额外一个 tick 覆盖“紧邻 tick 边界调用”的相位；只保证传入一个 tick 不保证至少
经过 1 ms。仍使用默认 100 Hz，没有全局改变 RTOS 配置或增加忙等。

| 原位置 / 数量 | 新行为 | 主机确认范围 |
|---|---|---|
| wake 两处 2 ms | `DelayAtLeastMs<2>()`，并检查整体预算 | 100 Hz 请求 2 tick，1000 Hz 请求 3 tick；边界相位仍满足最小等待 |
| EPD BUSY 轮询一处 1 ms | 阻塞一个 tick | 避免零 tick 循环；不承诺严格 1 ms 轮询周期 |
| EPD status 两处 1 ms | `DelayAtLeastMs<1>()` | 成功后及重试前均有实际间隔；命令错误和 BUSY 超时仍向上传播 |
| ADC 采样一处 1 ms | 原 `DelayAtLeastMs<1>()` 已撤回 | 表达式测试漏掉 10 次采样和同步 UI 调用的累积延迟，见回归修复记录 |

100 Hz 下 1/2 ms 的最小等待都会请求 2 tick，实际通常在 10–20 ms；调度延迟
可更长。ADC 批次和 EPD status 因而比原来的零 tick 循环慢，必须测真实采样批次
时延、显示稳定性和睡眠准备时间，不能把主机通过解释成功耗/性能收益。

## 计时恢复的追加发现与修复

完整生产计时/保留状态回归复现：睡前剩余 120 秒，恢复时墙钟比睡前回拨一小时，
旧实现得到 3,720 秒。之前的上限是全局 7,200 秒，未约束这次计时的剩余预算。
RTC 恢复失败后使用较早的编译时间也能走到这条路径。

`RestoreRetainedTimeApp` 现在将恢复剩余量限制在睡前已保存的 remaining 内。
下次正常 tick 沿用已有 `AlignWallClockTiming` 重定墙钟时间戳。运行中仍按单调
毫秒扣时，运行时校时只调整墙钟标记；暂停不扣时；番茄钟报警后仍等待显式确认
进入下一阶段，没有增加自动跨阶段补跑。

跨睡眠的经过时长仍依赖恢复时的墙钟。本批避免回拨奖励额外时间，不证明 fallback
build seed 是可信时间，也没有解决错误的向前跳变导致提前报警。离线 seed、TLS
reasonable 下界与可信时间来源的消费者政策仍属于 REL-004，需继续有界审计。

## 本地节点验证

| 检查 | 结果与边界 |
|---|---|
| `test_wake_deadline.py` | 100 Hz：1,799 checks；1000 Hz：1,808 checks，ASan/UBSan 通过 |
| wake/RTC 故障 | 完整生产 wake、RTC、共享锁源码；逐步逾期、锁消耗预算、驱动晚成功、timer/EXT1 晚成功、GPIO 活动、/STDBY 掩码及 ESP32 fallback |
| 短等待 | 3 种 tick 相位；完整 EPD BUSY/status 函数；ADC 只执行生产等待表达式 |
| `test_time_retention.py` | 110 checks，ASan/UBSan；完整计时/保留状态源码与真实 TimeAppState，含已有自测 |
| 计时用例 | 运行/暂停时前后校时、首次校时、恢复已过期/未过期、校验和破坏、恢复时清除确认态、两轮四阶段番茄钟及回拨不扩长预算 |
| `test_rtc_timekeep.py` | 100/1000 Hz 各 1,524 checks；上一批 RTC 信任、传输单位和 codec 回归继续通过 |
| 4 份旧实现 + 10 个错误变体 | 都编译成功，被运行时行为断言拒绝 |
| `idf.py -B build-wsl reconfigure build` | EXIT=0，ESP-IDF 5.5.6，零编译警告，M8 / 编译后 UI 栈门通过 |
| Python AST / CI YAML / CRLF-aware diff / 文档链接 | 通过 |
| 远端 CI / 新设备 HIL | 未执行 |

硬件端口为可控时钟、GPIO、驱动、信号量和 wake/sleep SDK；它们不证明真实锁
竞争、I2C 电气超时、EPD 波形或 ADC 输出。计时保留源码完整编译，仅 UI 头依赖
换为最小端口，RTC_DATA_ATTR 在主机上是普通内存；不证明掉电/重启保留行为。
检查数包含端口辅助断言，两个频率的差异来自实际轮询次数。

唤醒错误变体：`renew-rollback`、`ignore-rtc-deadline`、`accept-late-i2c`、
`ignore-ext1-deadline`、`zero-settle`、`omit-tick-phase`。计时错误变体为
`wall-elapsed`、`spend-paused`、`keep-action`、`skip-checksum`。四份历史源码来自
本批起点，分别为 wake、display、power 和 retention；runner 可用 `--*-source`
参数重新执行。`wall-elapsed` 也被既有生产自测拒绝，不宣称全部保护都是本批新增。

日志在 `/tmp/note4-b3-*.log`，正向分别为 `wake-positive`、`time-positive`、
`rtc-regression`，反向用对应变体标签。sanitizer 未关闭 LeakSanitizer。本批新增
独立 `wake-and-timer` CI job，未推送或运行远端 Actions。

## 候选身份

最终构建基于干净提交 `96bf4e6641b8c36a05a1830d7e50b4f0e3f49bea`，烘焙标识
`96bf4e66`，没有 dirty 后缀。首次构建也通过；修正注释并更新本地提交身份后，
重新 reconfigure/build 得到本候选。后续文档提交不改变运行时代码。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,643,840 B；`47db6ea5475ec6f73fb9b1df6d09c0c7e6127ec887320d62ddaeb49e16914bce` |
| ELF | 41,723,808 B；`afcd3e6f7297c9ec6c946096cf79416f0a620cb5cd2b1c8803ffafa1df5b2de0` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

有效配置仍为 100 Hz、RTC timekeep 启用，沿用本地内存诊断配置，不是无诊断
功耗控制组。最终构建日志为 `/tmp/note4-b3-build-final.log`；9 个相关源码文件、
配置和产物的 SHA 清单为 `/tmp/note4-b3-identity.json`。临时日志/清单可能被
系统清理。本次没有刷机或声明设备功耗/性能结论。

## 原设备矩阵；当前先复验响应性回归修复

第五批已被用户拒收。先按回归修复记录确认响应性恢复，再继续下列未完成矩阵；
不继续在未通过的候选上叠加后续 B3 改动。

核对候选标识与 SHA，记录设备、供电、网络和日志位置，然后补：

1. 正常 idle sleep，timer 与按键分别唤醒；在两次 GPIO 等待期间按键、RTC INT
   活动或插入 USB，确认睡眠取消且按键不丢失，轻睡按键唤醒继续可用。
2. 受控 RTC NACK/超时、锁竞争、timer 配置失败和预算临界点，记录退出/回滚耗时；
   RTC 清理成功才允许 ESP32 timer fallback，逾期不能提交睡眠。精确阶段需测试
   设施，普通成功操作不能代替。记录 SDK 调用超过预算时的实际延后量。
3. EPD 正常全刷/局刷、BUSY 持续低、status 重试和超时恢复；确认输入和其他任务
   仍响应，画面结果正确。既有分钟刷新频率及 EPD 单 owner 不变。
4. 比较 ADC 电压/百分比与独立仪表，记录批次耗时；确认较长采样等待没有破坏低电
   告警和紧急持久化预算。本批不声称修复或验证电压校准本身。
5. 计时运行和暂停时校时前跳/回拨，再做跨睡眠恢复；覆盖倒计时和两轮番茄钟。
   RTC 失败走 seed 时不能扩长睡前剩余量，阶段报警仍需手动确认。

本节点确认后继续审计 codec 完整调用预算和时间可信度消费者，按证据收口 B3；
随后推进 R-02/R-03/R-08 传输专项、B4 输入和 B5 留证/容量/功耗。
