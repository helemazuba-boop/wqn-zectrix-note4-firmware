# 第十批：底层按键回放、Release 保全与事件环容量

日期：2026-10-23（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`ca36d80`。

> 当前需要执行的验收见[统一验收清单](FIRMWARE_RELIABILITY_ACCEPTANCE.md)；本文保留历史范围与证据，不要求逐批重复设备矩阵。

## 状态与范围

用户回复“通过 继续”，第九批记为用户确认节点验收；没有附新日志，不补录逐项网络
故障实验或 SDK 收尾时序。本批推进 B4 / REL-006，执行真实底层采样、ring、消费过滤、
PTT 分支和睡眠最终门，修复两处可行为复现的问题，并消除 ISR 标志的非原子共享。

2026-10-23 用户再次回复“通过 继续”，第十批记为用户确认节点验收。没有新附逐项
故障注入结果；已有串口片段在第十一批单列利用，不补录本矩阵的精确硬件/调度结论。
后续进入 B5 的最小故障摘要。

| 提交 | 内容 |
|---|---|
| `ae0597a` | 过期 Confirm Release 继续进入 reducer，保全录音和导航清理 |
| `d776f2d` | ring 容量契约、共享标志/计数原子化、完整回放与 CI；中间目标构建被无锁断言阻止 |
| `198418f` | 适配 IDF PSRAM 原子兼容层，ISR 共用状态显式放入内部 DRAM，形成最终候选 |
| 本记录所在提交 | 前批验收、问题证据、资源变化、候选身份与实机矩阵 |

## 已确认问题与补丁

### 已开始的 PTT 可能因过期 Release 无法收尾

旧 ShouldDropStaleButtonEvent 将超过两秒的 Confirm Press、HoldPress、Release 一起丢弃，
注释称“丢弃整段旧手势”。过滤实际逐事件运行：如果 Press/Hold 已被正常消费并启动
录音，UI 随后阻塞，晚到 Release 就会被单独丢掉。导航的 held_mask/blocked_mask 也依靠
Release 清除，因此同一过滤可能保留旧键状态。

回放用真实 GPIO/采样任务启动一轮录音，正常消费 Hold 后松键，让 UI 三秒后消费尾部。
旧实现中 Release 被过滤，生产 Flash PTT 分支没有调用 OnFlashButtonReleased，
recording 端口仍为 true；Agent 同样依赖 Release 的 stop 分支。补丁只过滤过期的启动
边缘 Press/Hold，Release 无论年龄都可进入 reducer。

回归分别验证 Flash/Agent：已开始的录音收到延迟 Release 后停止一次；整段 Press/Hold/
Release 都过期时没有启动录音，也没有 stop 空录音；真实导航 reducer 能清理 Confirm
held/blocked。Flash 保留正常松键提交语义；Agent 松键继续仅停止/转录，后续确认策略
未改变。测试的音频命令和 Agent 快照是端口，没有调用真实服务或上传。

### HoldPress 使原事件环容量承诺失效

旧实现和头文件承诺：64 个槽可保留 UI 阻塞时 20 次完整短按，按每次三个事件计算。
实际一个触发 HoldPress 但尚未长按的手势会产生 Press、HoldPress、ShortPress（或
DoublePress）、Release 四个关键事件。200 ms HoldPress 已进入状态机，容量计算却
仍为旧模型。

在 100/1000 Hz 下，真实采样任务回放 20 次 300 ms 按住、150 ms 间隔，从空环开始，
UI 暂不消费。旧实现均只保留 64 个事件，明确记录 dropped_critical=16；补丁后保留
80 个事件，dropped_critical=0，峰值 80，事件 FIFO、生产时间和序号顺序符合回放。

ring 改为 84 槽，即 20×4 加一组手势余量。尾部同键 repeat 合并、满环优先淘汰最老
repeat、关键事件再无空间时计数并返回的策略保持原样。有限容量不保证无限输入无损，
也不保证任意已有积压下还能接纳 20 次新手势。满环丢弃关键事件仍先发布活动，最多
多取消一次睡眠，不允许排入 ring 却未发布活动。

### ISR 共用标志与诊断读取

isr_disabled 原为普通 bool，由 GPIO ISR 写、采样任务读写，缺少并发访问约束。现在用
32-bit atomic，任务以 acquire 读取，ISR/任务以 release 发布。重新启用中断仍先清标志、
再 gpio_intr_enable：若低电平立刻再触发 ISR，新的 disabled 状态会保留下来。注入该
交错并交换顺序的错误变体会被拒绝。

三个诊断计数改为原子读写，以支持现有公共 getter 的跨任务读取；每个计数独立读取，
没有新增“多字段一致快照”保证。ring 的 head/tail/payload 继续由原 spinlock 保护。

目标构建发现最初的 is_always_lock_free 断言不适用于当前工具链：sdkconfig 开启
CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND，库必须兼容位于 PSRAM 的 atomic。
核对 IDF v5.5.6 的 newlib/priv_include/esp_stdatomic.h、src/stdatomic.c、
src/port/xtensa/stdatomic_s32c1i.c 与 linker fragment 后，明确把 g_buttons 放到 DRAM_ATTR；
SDK 在需要兼容 helper 时可对内部 DRAM 地址转入硬件路径，当前配置把相关实现置于
IRAM；本候选的实际 ISR store 已直接内联，见下方反汇编结果。断言改为检查 32-bit
atomic 的存储大小，不把标准库全局的“始终无锁”特征当作硬件能力证明。
中间 `d776f2d` 没有生成或交付设备候选，最终镜像必须包含 `198418f`。

## 验证范围

新增 scripts/test_button_input.py：包含完整生产 button_input.cpp 和真实 public header，
执行 Init/Start、低电平 ISR、ButtonInputTask、分类、入环/出环和 idle park。以可控 GPIO、
单调时钟、RTOS tick、通知/调度端口回放；宿主异常只用于结束一轮任务或模拟不返回的
deep sleep，不修改产品任务或添加产品 reset API。

另外提取执行生产 ShouldDropStaleButtonEvent、Flash/Agent PTT 原分支、实际 AiTier/
UiGestureState，以及完整 PublishUserActivity、NoteUserActivityAtMs 和 CommitDeepSleep。
导航使用完整生产 navigation_model.cpp。音频操作、SDK GPIO/RTOS、保留标量、诊断记录、
UART、电源/UI-policy 读数和睡眠入口为明确端口。没有声称运行了完整 DeviceUiTask、
整个 ApplyButtonEvent 或真实 codec/网络服务。

覆盖短按、原始抖动、hold/long/repeat 阈值、release 尾事件、双击 300/301 ms 边界、
双键交叠、开机键已按住、长时间按住、idle 不轮询、ISR 重启交错；ring 绕回、合并、
淘汰、关键溢出与恢复；UI 积压后的 PTT/导航清理；事件生产先发布活动、保留生产时间、
重复事件不发布、UART settle 期间输入使真实最终睡眠门取消，以及无输入的入睡对照。

| 检查 | 本轮结果 |
|---|---|
| 生产按键/消费/睡眠组合回放 | 100 Hz 5270、1000 Hz 4670 项断言通过；ASan/UBSan，保留 LeakSanitizer |
| 旧基线两处故障 × 两种 tick 频率 | 四组均被对应行为断言拒绝，非编译失败 |
| 12 个错误变体 | 均被行为断言拒绝，非编译/环境失败 |
| 既有上层导航/模态/组合回归 | 1488 项断言通过 |
| 既有 ADC/UI guard | 100/1000 Hz 各 535 项通过，保持第五批卡顿回退 |
| SDK、符号和内存 | 见候选身份；初次断言失败日志单独保留 |
| 新设备 HIL、远端 CI | 未执行 |

错误变体：丢过期 Release、恢复 64 槽、入环后才发布活动、repeat 也发布活动、满环
直接丢新关键事件、不合并重复、颠倒 repeat 标志、用消费时刻替换生产时间、跳过最终
activity gate、先启中断后清标志、idle 持续轮询、漏掉 Release 尾事件。

新增独立 CI button-input job；无需 SDK 即可跑宿主测试，远端本轮未运行。日志为
`/tmp/note4-b10-{buttons-final,navigation,power,controls-final,build}.log`；旧实现写入
`/tmp/note4-b10-control-before-<case>-<hz>.log`，最终错误变体写入
`/tmp/note4-b10-control-<name>-final.log`。首次目标构建断言失败保留在
`/tmp/note4-b10-build-initial.log`，不能计作最终 PASS。临时文件可能被系统清理。

宿主回放证明受控时序下的代码决策，不证明实际 GPIO 电气抖动、跨核竞争概率、ISR
延迟、PSRAM/cache 状态、低功耗唤醒或真实 UI 阻塞时间。手势阈值仍为 debounce 40、
hold 200、long 650、repeat 260、double 300 ms；采样 pdMS_TO_TICKS(15) 保持原值，
100 Hz 对应 10 ms、1000 Hz 对应 15 ms。ADC 延时和分钟刷新策略没有修改。

## 候选身份与资源变化

运行时提交 `198418fe94dfd7b61285ab78fb9b94f716beb034`，烘焙标识 `198418fe`，无 dirty 后缀。
从干净跟踪工作区运行 `idf.py -B build-wsl reconfigure build`，EXIT=0，ESP-IDF v5.5.6，
零编译警告，M8 ownership 与编译后 UI 栈门通过；随后仅更新文档。

| 对象 | 大小 / SHA-256 |
|---|---|
| build-wsl/wqn-zectrix-note4.bin | 3,647,920 B；`6710b3c60f8c54915791af4cc18d5db34c5054801dc40c4ec8963e6ecc84c804` |
| build-wsl/wqn-zectrix-note4.elf | 41,815,396 B；`a39b22b02bd4616ff97ce849bf6dccf5b55a47ff9dfa4a6cfbb851171a8d3abb` |
| sdkconfig | 76,758 B；`70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

sdkconfig 与第九批相同。最终 ELF 的 g_event_ring 为 0xd20（3360 B），此前为
0xa00（2560 B），即 **事件环增加 800 B 静态内部 RAM**；g_buttons 仍为 0xc0（192 B），
三个计数仍各 4 B。g_buttons 地址为 0x3fca8db8，位于内部 DRAM。反汇编中 ISR 的
release store 是 memw / s32i 直接写入，未调用原子 helper、分配或互斥函数；这不证明
整个 ISR 调用链都可在 cache-off 状态运行，GPIO ISR service 仍按原 flags=0 注册。
有限环在全满时的最长搬移比原容量大，实际 ISR/按键/显示尾延迟仍在设备节点观察。

源码/产物清单为 `/tmp/note4-b10-identity.json`；符号对照为
`/tmp/note4-b10-symbols-{before,after}.txt`，ISR 反汇编为
`/tmp/note4-b10-atomic-disassembly.txt`。精确 heap largest/free、功耗与响应时间不能从
静态字节变化推导。重构建可能改变构建时间和镜像摘要，核对设备实际身份。

## 设备验证矩阵与验收边界

1. 核对最终候选身份。普通页面短按/双击、长按开关导航、进入 Settings 二级界面，
   松键后继续确认/返回；确认旧 held/blocked 状态不残留，交互和分钟刷新正常。
2. Flash/Agent 正常 hold 录音并松开。若有受控 UI 堵塞设施，分别覆盖录音已开始后
   松键延迟消费，以及整段手势过期才消费；前者停止一次，后者不启动录音。自然操作
   未观察到堵塞不能记成精确三秒故障实验。
3. 在受控 UI 忙窗口积压 20 次触发 HoldPress 的短按，核对采集/消费、实际 ring peak/
   drop 与尾 Release；持续 repeat 压力下确认合并/淘汰后仍能正常接收下一轮。注入
   设施需单独记录，不能靠肉眼分页数量替代底层事件证据。
4. 检查实际 GPIO 抖动、两键交叠、键保持低电平时的 ISR 抑制/恢复与 retained standby
   唤醒；在允许自动深睡的场景观察准备/最终提交交界的输入取消。用户主动/紧急关机
   的原策略保持原样，不把普通页面 retained standby 冒充深睡证明。
5. 结合本镜像新增静态 RAM，观察正常录音与显示并发时的余量和响应；既有长录音
   证据继续保留，不无条件重复相同长测。无日志的“通过”仅记录为用户确认节点验收。

B4 的代码和主机验证已形成设备候选，电气/真实调度验证仍在本节点。验收后进入 B5：
先复用现有诊断与容量证据，审查故障摘要，再安排高占用存储尾延迟、诊断开关对照、
低电量负载和最终整机收口；这些项目的物理电流/电压与掉电结果继续单列。
