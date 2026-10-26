# 第六批：codec 整次调用预算与清理

日期：2026-10-22。分支：`fix/firmware-reliability`。起点：`623f458`。

> 当前需要执行的验收见[统一验收清单](FIRMWARE_RELIABILITY_ACCEPTANCE.md)；本文保留历史范围与证据，不要求逐批重复设备矩阵。

## 状态和范围

用户确认第五批 ADC 回归修复“通过 继续推进”，本批继续 B3 / REL-005。该确认
只记为响应性修复节点通过，没有新增逐项设备日志或删除第五批原候选的失败记录。
本批不改 ADC 调度、UI 刷新间隔或音频寄存器值。

用户随后回复“通过 继续”，本节点记为用户确认验收通过；未附新日志，不补录
逐项故障注入或耗时结果。现继续[第七批 RTC 持久化预算](RELIABILITY_BATCH_07.md)。

| 提交 | 内容 | 验证状态 |
|---|---|---|
| `30ea9c3` | 共享配置预算、有限等锁、热复用 session 验证、清理失败停止重试 | 完整 host 通过后提交；SDK/HIL 留节点 |
| `578a0ff` | 三种 profile 完整生产配置回归、成本/顺序对照、独立 CI job | host 和反向控制通过；后续取消回归补齐 |
| `3f6ec1a` | 已准入配置在 drain 中清理私有句柄；公开入口仍受准入门约束；真实 close/drain/finish 函数回归 | 最终 host / SDK 构建通过；随后用户确认节点验收 |
| 本记录所在提交 | 接受响应性复验、记录候选和设备矩阵 | 文档检查及本节点构建 |

## 审计发现与补丁

codec 配置由录音、Flash duplex 和自测工作路径同步调用，既有 `Call()` 的 5 s
队列等待并不自动约束这些直接调用。原 probe、整组编程、寄存器读写和整组失败
reset 的共享锁均使用默认无限等待；每个寄存器最多 10 次、整组最多 2 次重试，
但没有共同截止时间。独立寄存器重试还在持有该次锁时等待 backoff。

本批以已有 `kCallTimeoutUs`（5 s）定义 codec 工作准入预算：

1. 正数绝对截止时间贯穿 probe、整组程序、寄存器重试、原有稳定等待及整组重试。
   每次等共享锁最多使用剩余预算及 100 ms，驱动收到剩余整毫秒及 100 ms 的较小值。
2. 驱动、恢复和等待之后检查逾期；逾期不能成为成功配置或可复用缓存，不能给下
   一个寄存器重新分配 5 s。独立公开读写各自获得一份 5 s 预算。
3. 正常寄存器顺序、payload、重试次数上限、clock/reset 稳定等待不变。独立读写
   在 backoff 前释放本次锁；整组配置继续串行化整个程序，并扩展到 attach/remove。
4. 配置从入口持有 `DriverOperation`。热缓存复用也校验当前 session，不能只凭
   非零旧 token 和匹配 profile 就直接返回成功。
5. 私有 codec handle 即使在工作预算耗尽后也必须清理。清理失败立即返回错误，
   不再开启第二组程序并重复登记句柄。

完整回归发现并补齐了取消交界：正常 session drain 先关闭新操作准入，再等待已
准入的 `DriverOperation`。旧准入门若同时拒绝该配置的私有移除，资源登记会留下，
使后续 EndActivity 失败。因此仅对已准入配置的私有清理允许通过 admission 门；
仍要求 session 匹配、slot owner 匹配、slot 未在关闭。公开 codec/channel 移除
继续使用原门禁。清理完成、准入已关闭时返回 INVALID_STATE，不进行新配置重试。
`578a0ff` 的中间实现也被这条取消回归拒绝，未交付为最终设备候选。

## 预算与所有权边界

共享总线在配置的 attach → program → remove 期间保持递归锁，避免私有句柄在
清理时重新等待另一个设备的事务；整组失败的 reset 和 backoff 也在这个配置锁内。
正常路径的额外等待由对照测试检查，故障时 RTC 仍按其有限等待返回错误。真实
codec/RTC 竞争及取消必须在设备上补测，不能仅从递归互斥模型推断任务公平性。

这是协作式预算，不是硬实时 WCET。IDF 同步 I2C 内部可能重复使用 timeout，reset
和移除 API 也没有整段硬截止接口。强制紧急关机的供电切断不由此预算改写。
清理沿用既有 drain/SDK 移除，不能为了守住工作预算跳过资源回收；若 SDK 持续拒绝
移除，则返回错误并保留 owner 登记、禁止重试，不声称自动恢复已完成。这类持续
SDK 故障的业务恢复仍需继续审计。

## 完整正常路径对照

以本批起点生产源码和最终源码分别执行相同端口。对比的是完整 probe、程序、移除
和热复用，固定驱动调用成本为 100 us、RTOS tick 使用相位模型：

| profile | I2C 寄存器传输数 | 100 Hz 冷配置模型 | 1000 Hz 冷配置模型 | 顺序/内容摘要 |
|---|---:|---:|---:|---|
| Capture | 44 | 61,500 us | 72,500 us | `8537d48d` |
| Playback | 47 | 61,800 us | 72,800 us | `596264b0` |
| Duplex | 47 | 61,800 us | 72,800 us | `0f6730ee` |

六行旧/新结果一致。摘要覆盖读写标识、寄存器地址和内容，辅助断言检查 REG16、
ADC/DAC profile 值及重复 REG44 开头。热复用不增加 I2C/RTOS 等待。模型不计
真实 CPU 指令和日志成本，不能据此宣称 ESP32 初始化耗时等于表中值或完全无开销。

ADC/UI 延迟回归继续运行，默认 100 Hz 下没有恢复已撤回的 2-tick 采样等待。

## 最终本地验证

| 检查 | 结果与范围 |
|---|---|
| `test_audio_codec_budget.py` | 100/1000 Hz 各 4,411 checks，ASan/UBSan 通过 |
| 故障与生命周期 | 持续 NACK、跨寄存器预算耗尽、锁拒绝/晚返回、probe/事务/reset/移除晚成功、热缓存旧 session、关闭准入后的私有清理、公开移除仍拒绝、清理失败停止重试 |
| 原实现 + 中间取消实现 | 均编译成功，分别被有限等锁和取消清理行为断言拒绝 |
| 8 个错误变体 | 均编译成功，被运行时行为断言拒绝 |
| 正常路径对照 | 两种频率 × 三种 profile 的 IO 摘要、传输数、模型等待完全相同 |
| RTC/codec 原回归 | 100/1000 Hz 各 1,524 checks 通过 |
| ADC/UI 回归 | 100/1000 Hz 各 535 checks 通过 |
| I2S 超时回归 | 100/1000 Hz 各 1,222 checks 通过 |
| Python AST / CI YAML / CRLF-aware diff / 文档链接 | 通过 |
| SDK 构建 | ESP-IDF 5.5.6，EXIT=0，零编译警告，M8 / 编译后 UI 栈门通过 |
| 远端 CI / 新设备 HIL | 未执行 |

完整 codec 配置、Add/Remove、寄存器读写、寄存器程序、retry 分类、恢复和 close/
drain/finish 函数均按边界提取生产实现，预算类和公有类型使用真实头文件。SDK、
RTOS、资源登记、DriverOperation 生命周期、session 匹配及 critical section 为
明确的 host 端口；没有声称复现真实 SMP 竞争或 PCM 数据。sanitizer 未关闭
LeakSanitizer。旧 RTC fixture 已适配内部 helper，仍执行生产公开读写入口。

错误变体：`infinite-locks`、`renew-register-budget`、`skip-cleanup`、
`accept-late-cache`、`skip-session-guard`、`hold-backoff`、
`retry-after-cleanup-error`、`deny-admitted-cleanup`。
原实现为 `/tmp/note4-codec-before.cpp`，中间实现为 `/tmp/note4-codec-before-drain.cpp`；
runner 支持 `--source` / `--happy-only`。最终正常/故障日志为
`/tmp/note4-codec-drain-final.log`，对照为 `before-happy-final` / `happy-final`，
各错误变体使用同前缀及 `-final.log`。临时文件可能被系统清理。

## 候选身份

构建基于干净运行时提交 `3f6ec1a3d223474ebabbf2efe0e0738db79f9fff`，烘焙标识
`3f6ec1a3`，无 dirty 后缀。随后仅更新文档，不改变此运行时。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,644,688 B；`26956a6a9fd5bbd5e9004e79853272d88bac73d8141d9c8526aa721924bc439a` |
| ELF | 41,732,824 B；`7b00c0e9f8b5f529ee29e0d3445e4c5fbba97ec258bf2bfa7ebe2f8a2cb4155e` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

产物位于 `build-wsl/`；完整源码/产物摘要为 `/tmp/note4-codec-identity.json`，
最终构建日志为 `/tmp/note4-codec-build-final.log`。配置仍为 100 Hz，诊断配置
未切换。后续重新构建可能改变构建时间和摘要，设备验收时应核对实际镜像身份。

## 已确认节点及原设备矩阵：音频配置与共总线

核对候选标识并保留串口日志，优先执行新增路径，不默认重跑历史十轮录音长测：

1. 冷启动录音初始化，再启动同 profile/音量的第二轮，观察冷配置和热复用日志、
   开始/结束录音及退出后交互；确认没有重新出现 ADC/UI 的 200 ms 倍数延迟。
2. 按实际入口覆盖自测读取和 Flash duplex 的收发、音量变化；确认 profile 切换、
   音频内容、REG16 原增益和 ADC/DAC 路径保持正常。
3. 受控 RTC/codec 锁竞争及 NACK，记录每次配置耗时、重试和返回；不能无限等锁，
   也不能把晚成功写入配置缓存。实际 SDK overrun 单列，不能由主机预算代替。
4. 在初始化尚未完成时停止音频，随后重新启动并正常结束；核对资源和 session 能
   退出，不留下阻止 EndActivity 的私有句柄。精确窗口需测试设施。
5. 音频退出后正常睡眠/唤醒，检查 RTC 共总线仍可用。清理 API 故障与实际硬件
   故障分开记录，不从普通成功操作推断故障路径通过。

本节点已获用户确认，第七批继续 REL-004 时间可信度消费者和剩余电源预算审计，再进入
R-02/R-03/R-08 传输专项，随后 B4 输入及 B5 留证/容量/功耗。
