# 第四批：RTC 信任与共享 I2C 的首个修复节点

日期：2026-10-22。分支：`fix/firmware-reliability`。起点：`f72c6bf`。

## 范围与状态

用户确认第三批“通过 继续推进”，本批继续执行既有路线。沿用“允许在不经测试的
情况下创建本地提交，在关键节点停下来验证”的授权：运行时补丁分别本地提交，
随后集中执行本节点回归和构建。没有将此授权改写为 AGENTS.md 的通用例外。

本节点修复全仓审查 R-01、R-07，并处理同一总线审计发现的两处 codec 单位错误、
RTC 无限等锁和设时失败误报成功。代码、host 和 build 验证通过；设备 HIL 待执行，
不关闭整个 B3 / REL-004 / REL-005。传输专项及其他批次仍未完成。

| 提交 | 内容 | 提交时验证状态 |
|---|---|---|
| `074d4b6` | RTC 的 7 处驱动参数使用毫秒，3 个锁入口明确有限等待；报告 STOP 清理失败 | 测试/构建延后至本节点 |
| `f986842` | 每次 persist 先撤销旧信任；恢复校验睡眠快照及代次；检查两处 settimeofday 返回值 | 测试/构建延后至本节点 |
| `78b666f` | ES8311 两处寄存器传输使用毫秒 | 测试/构建延后至本节点 |
| `82029ac` | 生产源码回归、CI 入口、显式包含 setenv 所需标准头 | host 及反向验证通过；随后构建通过 |
| 本文档所在提交 | 记录候选、设备矩阵与剩余路线，更新第三批用户确认状态 | 文档检查；不改变上述运行时产物 |

## 行为变化

### RTC 和 codec 时间单位

ESP-IDF 5.5.6 的 `i2c_master_transmit` / `i2c_master_transmit_receive` 最后一个
参数为 `int xfer_timeout_ms`。原来的 `pdMS_TO_TICKS(100)` 在默认 100 Hz 下传入
10 ms，1000 Hz 下恰好传入 100 ms。本批修复 RTC 的 7 处调用及共享总线 ES8311
寄存器读写的 2 处同类调用。codec probe 原来就直接传 100 ms，无需修复。

RTC 的读寄存器、写寄存器、整组写时钟都显式以 `pdMS_TO_TICKS(100)` 获取共享
递归锁：默认配置是 10 tick，1000 Hz 是 100 tick。等锁失败不发起驱动事务。
这些是互斥等待和驱动各自的参数；不承诺整段函数或 wake assembly 严格在 100 ms
内结束。SDK 内部等待、调度、恢复及多步骤累加仍须在完整调用期限审计中核对。

写时钟仍以一个锁覆盖 STOP → burst → 释放 STOP → 读回。burst 失败继续尝试
释放 STOP；释放也失败时补充日志和已有错误环记录。失败始终向上返回 false，
没有增加第二总线、跨设备 reset 或自动重试。物理总线故障可能使 STOP 无法释放，
本补丁不承诺时钟一定继续走动；信任撤销阻止把这次失败当作成功持久化。

### 本轮写入失败不能借用旧信任

`PersistSystemTimeToRtc` 在校验当前时间和访问 RTC 之前就清空信任记录。因此
时间不合理、超过 2099 年、等锁失败、任一传输失败或读回不符均不会留下旧信任。
只有写入并读回成功才发布本轮 generation 和校验和。

`RestoreSystemTimeFromRtc` 要求深睡恢复、reset reason 为 DEEPSLEEP、信任校验
有效、睡眠快照有效且 generation 相等，之后才读 RTC。该 generation 来自
`CaptureWakeContext` 在启动时装载的已提交 SleepSnapshot；正常 idle sleep 的
persist 和 snapshot 使用同一个 PowerCoordinator generation。取消后留下的
信任记录不能凭自身校验和绕过快照检查。保留记录布局和版本未改变。

RTC 的 VL、读失败、无效字段和 2024 年下界仍按原路径拒绝。恢复和 build-time
seed 两处都检查 `settimeofday`，失败返回 false，日志不再宣称设时成功。
本批没有重新定义合法日历日、TLS 信任策略或计时器的校时语义。

## 调用链审计与未解决边界

| 路径 | 当前资源与错误处理 | 本节点结论 / 剩余项 |
|---|---|---|
| 启动 RTC 恢复 | safe pins → shared bus → CaptureWakeContext → restore → build seed，早于调度/同步/UI | 真实启动顺序已核对；host 替换 wake-context 端口，不证明硬件保留状态 |
| idle sleep persist | 服务 quiesce 后、wake assembly 前写时钟；失败不阻止睡眠 | 本批撤销旧信任并限制 RTC 等锁；尚未将整个事务的剩余 deadline 传入驱动 |
| RTC timer/flags | 每次寄存器事务持有限等待的共享锁，错误逐层返回 | 各步骤可退出；整段 deadline、失败后的清理耗时仍待审计 |
| codec program | 外层递归总线锁覆盖整组编程，寄存器助手再次取得同一锁 | 保持既有序列；编程/probe/read/write/reset 的 5 个锁入口仍默认无限等待 |
| codec operation | DriverOperation 在短 critical section 注册 inflight，离开 critical section 后才等总线 | 未发现本次修复引入反向锁序；host 的生命周期端口不证明真实任务调度 |
| codec recovery | 已有 retry、总线 reset 与 GPIO42 恢复均在 owner 内执行 | 本批保留；增大实际传输等待后应实测失败重试尾延迟，不能只核对单次参数 |

`ArmWakeSources` 的外层预算当前为 2 s（`kWakeArmTimeoutUs`），两个“2 ms”
等待在 100 Hz 下仍变成 0 tick。不能直接改成一个 tick 并认为已满足 2 ms 的硬件
稳定时间；最小等待、任务让步和整体 deadline 需要一起处理。ADC 采样间隔及 EPD
BUSY/status 的 1 ms 调用也仍属于 REL-002，尚未改动。

计时器已有自测和保留状态代码继续有效；本节点没有补完运行/暂停时 SNTP 前后
跳变、跨睡眠、多轮番茄钟组合。reasonable 下界与“可信墙钟”的消费者语义仍属于
REL-004。上述剩余项保留为待办，不因为 R-01/R-07 修复就标为关闭。

## 集中验证与证据范围

执行 `python3 scripts/test_rtc_timekeep.py`：

| 检查 | 结果 |
|---|---|
| 100 Hz / 1000 Hz | 各 1,524 checks PASS，ASan/UBSan 通过 |
| RTC 驱动 | 初始化、读写、VL、写入各失败阶段、STOP 清理双失败、读回不符、有限等锁拒绝、timer arm/clear/flags 的失败和验证路径 |
| 时间信任 | 无信任、成功恢复、快照/代次/reset 不匹配；旧成功后 9 类失败使新旧代次都不能恢复；后续成功可重新建立信任 |
| 系统设时 | RTC 恢复和 build seed 的 settimeofday 失败均返回 false；正常 seed 及 UTC 世纪/闰日边界 |
| codec | 生产寄存器读写与重试分类函数；检查两种单位、可重试/不可重试错误、锁失败、供电和恢复错误的传播 |
| 3 份历史源码 | 各自编译成功，被运行时行为断言拒绝 |
| 7 个错误变体 | 各自编译成功，被运行时行为断言拒绝 |
| `idf.py -B build-wsl reconfigure build` | EXIT=0，ESP-IDF 5.5.6，零编译警告，M8 ownership 和编译后 UI 栈门通过 |
| Python AST、CI YAML、CRLF-aware diff、文档链接 | 通过 |
| 远端 CI / 新的设备 HIL | 未执行 |

回归完整编译生产 `pcf8563.cpp`、`rtc_timekeep.cpp`、`i2c_bus_lock.cpp` 并包含
真实公有头；codec 两个寄存器函数、重试判定及常量按边界提取。I2C、FreeRTOS
semaphore、系统时间、wake context、codec 生命周期/供电/恢复是明确的 host 端口。
断言检查预算参数和决策，不实测互斥阻塞时间、任务公平性、电气 NACK 或深睡。
检查数包含辅助断言，不是 1,524 个独立设备场景。

错误变体为 `driver-ticks`、`infinite-lock`、`keep-old-trust`、`ignore-generation`、
`ignore-snapshot`、`ignore-settime-error`、`codec-ticks`。历史源码位于
`/tmp/note4-rtc-before-{pcf8563,timekeep,audio-service}.cpp`；runner 支持对应的
`--pcf-source`、`--timekeep-source`、`--codec-source` 参数。

最终日志：`/tmp/note4-rtc-positive-final.log`、`/tmp/note4-rtc-*-final.log` 和
`/tmp/note4-rtc-build.log`。sanitizer 在获准的沙箱外运行，未关闭 LeakSanitizer。
CI 新增独立 `rtc-timekeep` job；只完成本地 YAML 检查，未推送或运行远端 Actions。

## 候选身份

构建基于干净提交 `82029ac0c294f5926269ad1289e8e980a9857ae2`，烘焙标识
`82029ac0`，没有 dirty 后缀。后续文档提交不改变运行时代码。源码及产物清单在
`/tmp/note4-rtc-identity.json`，临时日志和清单可能被系统清理。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,643,184 B；`4a01e7ff19bed1602503c005b7d1a6773500f6bb6d56bbaebaeb34c3c700d035` |
| ELF | 41,716,592 B；`62e428413ca51189d064af39e69814605ca528b6cc30663c7fb5d5e2139d6e2e` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

有效配置为 100 Hz、RTC_TIMEKEEP_ENABLE=y，沿用本地内存诊断及历史配置。
不是无诊断功耗控制组。本次未刷机；待设备操作授权后再绑定实际设备和供电记录。

## 当前停止点：RTC/共总线设备节点

先核对候选身份，再在受控设备上执行。避免重新跑无关的历史 RAM/录音长测。

1. 在线获得可靠时间后离线，正常入睡并由 timer、按键分别唤醒，核对 persist 和
   restore 的 generation 一致、墙钟走时正常；正常取消睡眠后再入睡也能恢复。
2. 先成功持久化一次，再受控注入 RTC NACK/超时、读回不符或等锁失败，允许本轮
   睡眠。下次唤醒不能记录旧信任校准成功，日志应走拒绝/seed 路径。产品没有新增
   通用故障开关；精确阶段注入需测试设施，普通操作成功不能代替此项。
3. 覆盖 VL、断电冷启动和软件 reset，确认不借用深睡信任；读写时钟与 timer
   flags 正常。STOP 清理失败后实际时钟是否继续走动单独记录，不能推断自恢复。
4. 做一次 codec 初始化、录音开始/结束及退出后的睡眠/唤醒，确认共享 I2C 能重新
   使用；受控持锁和总线故障时记录 RTC 返回、codec 重试和回收的实际耗时。
5. 记录正常唤醒及失败回退的日志和设备/供电条件。当前 host 端口不替代电气竞争，
   也不证明最坏执行时间。不能把 F2 reset 或串口重新附着当作真实掉电。

本节点确认后，先收口 B3 的完整 deadline、wake/ADC/EPD 短延时和计时校时组合，
再推进 R-02/R-03/R-08 传输专项，随后 B4 输入及 B5 留证/容量/功耗。
REL-010 OTA 健康门和 REL-012 云适配仍按各自有界审计推进。
