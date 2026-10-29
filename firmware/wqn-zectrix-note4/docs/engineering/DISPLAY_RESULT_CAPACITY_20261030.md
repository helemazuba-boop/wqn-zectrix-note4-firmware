# R-04 显示终态容量与首次丢失复核

当前源码没有证实“UI 停止消费 → 队满 → 第一次终态丢失”的触发链。因此本批
不计作运行时缺陷修复，也不按超时猜测某个 revision 已丢失。异常队列/owner 故障
后的 UI 账本补偿仍是防御性恢复缺口；需要先给出打破下述约束的真实来源。

## 容量不变量及源码绑定

令 L 为已接受、尚未 ACK 的 intent 数，Q 为已入队但未取出的终态数，D 为已取出
但尚未 ACK 的终态数，T 为尚未发布终态的 intent 数。正常协议保持：

```
L = Q + D + T <= 16
发布者仍有自己的 T >= 1，所以发布前 Q <= 15。
```

这依赖唯一终态、相同队列/账本容量和 receive 后才 ACK。UI 停顿只会保留已有
reservation，阻止继续准入；它本身不会制造第一个无法入队的终态。

| 约束 | 当前生产路径 |
|---|---|
| 相同容量与正确元素 | `EnsureDisplayPipelineStarted` 调用 `xQueueCreate(kDisplayResultQueueDepth, sizeof(DisplayResult))`；owner 和 UI 账本均使用该常量 |
| 保留 reservation | `RequestEpdUiRefresh` 在 refresh mutex 内拒绝重复 revision / 第 17 项；成功项登记 lower ledger，直到 ACK 才退出 |
| 替换与渲染互斥 | pending primary / secondary 的替换在同一 mutex 下发布一次 Superseded；被 owner 取走的 primary 不再走该替换分支 |
| owner 终态一次 | `EpdRefreshTask` 对取出的 intent 只发布一项 Presented 或 Failed；promotion 保留独立 secondary intent |
| ACK 在 receive 后 | `DrainDisplayResults` 的两处 ACK 均在成功 receive 后；缺少 upper record 的分支也不凭空 ACK |
| 提前入队不等于丢 UI 登记 | producer 可在 `taskYIELD` 时让 owner 发布，但同一 UI task 先完成 `TrackDisplaySubmission`，再执行下一轮 Drain |
| 无并行恢复 UI 消费者 | `ShowStorageRecoveryUi` 只在启动业务 UI 前的 `EnterStorageRecoveryMode` 调用；该分支为 noreturn，普通 UI 不会同时启动 |
| 无已知单独 owner 中止/queue reset | refresh owner 为持续循环，无项目内删除/暂停该任务的路径；结果 queue 创建后没有 reset/delete/替换调用。UI 的三个主动退出点都在首次提交前 |

已核对当前 IDF v5.5.6 的 `FreeRTOS-Kernel/queue.c`：有效 task-context queue send
有空间时复制并返回成功，失败路径首先要求观察到满队列，再立即失败或等待超时。
本机 SDK commit/hash 随本批证据记录。非法句柄、内存破坏、第三方代码篡改、SDK
缺陷或未捕获的 owner 故障不在这个容量证明的前提内。

## 主机验证

```sh
python3 scripts/test_display_result_capacity.py
python3 scripts/test_display_result_capacity.py --mutation short-queue
python3 scripts/test_display_recovery.py
```

新 fixture 与既有显示恢复测试共享生产提取器，执行真实启动函数、提交/替换、
完整 refresh owner、UI 跟踪/Drain 与 ACK。RTOS、render、时钟和队列是端口；有界
队列的长度/元素大小直接取自真实启动函数的创建调用，正例只会因真实容量已满而
拒绝 send，不强制注入失败。consumer 明确停顿时不会在发送等待中偷偷消费。

场景覆盖 16 项准入与第 17 项拒绝、primary/secondary 满载替换、owner 即将发布时
producer 补入新项、receive 与 ACK 之间 owner 继续发布、终态先入队再登记 UI、
返回型 render failure，以及 80 组各 64 步的固定种子 API 调度。每步比较队列、
上下账本和 reservation，最终核对每项接受都有且仅有一个终态、租约归零。
这些 API 调度允许比当前单 UI 循环更密集的 producer 调用，是保守的容量压力。

owner 在端口的 idle 点暂停，下一次主机调用重新进入函数；不模拟 FreeRTOS 真正的
CPU 并行，也不据此证明跨调用像素缓存、物理呈现或设备时序。另有计数抽象遍历
969 个合法数量状态及 3264 条准入/发布/取出/ACK 转移；这是对上述前提的抽象
不变量检查，不是对完整固件做了穷尽调度验证。

四个负例分别绕过准入、提前退休 reservation、重复终态、把实际创建队列缩短一项。
它们必须完成行为断言并返回失败；编译错误和 sanitizer 异常不算检出。负例证明
这些前提需要保护，不能用来冒充当前源码已经存在同样的失效来源。正例和负例均
执行 100/1000 Hz，使用 ASan/UBSan/LSan。正例每频率 45,586 项检查通过，既有显示
恢复每频率 715 项及提交 96 项回归通过。

## 处置与后续证据

只更正 owner 发送失败处误导性的背压注释；保留既有运行时行为。若以后提供非整机
reset 的 owner 终止、真实 queue fault 或其他首次丢失证据，应单独设计 owner 权威
generation / dropped 水位的恢复协议，覆盖 upper/lower 账本、lease 与像素提交，
不能仅根据经过若干秒就回收仍可能在途的 revision。

真实 EPD/BUSY、TWDT 行为、设备 RTOS 调度、栈水位和历史现场问题仍按统一验收
清单执行。本批结论不意味着固件所有显示故障均已修复或实机验收通过。
