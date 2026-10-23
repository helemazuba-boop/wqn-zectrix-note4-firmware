# 第十一批：跨重启最小故障摘要

日期：2026-10-23（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`394a60f`。

## 状态与范围

用户回复“通过 继续”，第十批记为用户确认节点验收，没有新附逐项 HIL 结果。
本批开始 B5 / REL-007：补固定容量、无 Flash 写入的 RTC 数字故障摘要；REL-008 的
高占用存储尾延迟和 REL-009 的诊断对照/低电量测量仍未完成。工作区现有串口片段的
利用单列，不把它当成本批镜像的性能验收。

| 提交 | 内容 |
|---|---|
| `5f64e72` | 双副本 RTC journal、旧错误入口桥接、启动日志导出、完整生产回归与 CI |
| `4f01236` | 启动阶段、恢复错误码/generation、存储事务错误/排队超时 request ID、显式导出 |
| `b667fe3` | 排除正常空查询 NOT_FOUND，形成最终运行时候选 |
| 本记录所在提交 | 第十批验收、保留策略、资源/镜像身份、现有日志利用与设备矩阵 |

## 原缺口与最小交付

error_recorder 的八条格式化错误位于普通 RAM。已有睡眠 RTC 诊断记录睡眠流程，
PSRAM 内存历史记录趋势，均不能直接代替普通错误的跨重启上下文。新的 fault_diagnostics
保留最近四条固定格式数字记录，旧八条文本错误继续按原 UI 入口显示。

记录字段包括编译时 Git 身份、journal boot ID、启动复位原因、装配阶段、域/错误阶段、
错误码、数字 operation/generation、该 boot 的单调 uptime、墙钟值及其来源/epoch/
RTC 连续性。字段用于关联同一保留周期中的报告，boot ID 不是永不重复的设备标识。
冷重建会从 1 开始；初始化提交之前发生的重启也不能被可靠计数。

保留区不含格式化错误消息、tag 原文、路径、SSID、token、用户内容、指针或服务器 UUID。
旧 RecordError 的消息仍只在原普通 RAM 环中；新摘要只映射固定 domain，错误码使用
INT32_MIN 表示“该入口没有提供错误码”，没有把它伪装成 ESP_OK。

## RTC 介质、恢复与损失边界

使用两个 RTC_NOINIT 副本，每份包含完整四条记录及元数据，带 magic/version/revision/CRC。
数据类型为 trivial 固定布局，不依赖保留对象的构造函数。先清除非活动副本的有效标志，
写入 body/CRC，经过内存屏障后发布标志；另一份完整副本保留到本次发布完成。

初始化校验 CRC、版本、索引、数量、枚举边界和字符串终止，并按可绕回 revision 选择
较新的完整副本；一个损坏时回退到另一个。两个均不可用时重建。软件、panic、WDT、
deep-sleep、CPU-lockup reset 是允许尝试保留的原因；poweron、brownout、power-glitch、
USB、JTAG、未知等不可信原因清空本摘要自己的区域，不影响其他 RTC 模块。

这不是全掉电持久化，也不是 panic hook/coredump。它只能带回复位前已发布的任务上下文
报告及装配阶段；panic reset 原因不证明最近一条报告就是 panic 根因。未发布的最新报告
可能丢失，最新副本损坏后回退也可能少一次更新。CRC/双副本证明的是受控数据决策，
不证明任何电气掉电形态下都能保留 RTC。USB_UART_CHIP_RESET 会破坏 RTC；继续使用
现有 no-reset monitor，不能把 F2/USB 重置当作 ESP_RST_SW 的保留验证。

计数独立解释：

- reports 为本保留周期已记录的报告数，不是独立事故数量；不同层上报可对应同一故障。
- overwritten 为满四条后主动替换最老报告的次数。reports/overwritten 达 UINT32_MAX
  后饱和；序号、revision、boot ID 绕回时跳过 0。
- integrity_rejections 累计被拒绝的非空副本，包括 CRC、版本或结构不合格；它不是精确
  的丢失故障条数。回退或重建可能损失无法计数的报告。
- recovered=0、count=0 不等于设备从未失败，也不等于历史零丢失；它可能是新介质周期。

## 接入与资源成本

录制只执行时钟读取、固定格式赋值、CRC 和 RAM/RTC 拷贝，受专用 spinlock 保护。
不新增 worker、定时采样、堆查询、动态分配或 NVS/SPIFFS 写入。只支持任务上下文，
不从 ISR、heap-failure callback 或 panic handler 调用。旧错误环锁释放后才进入新摘要，
避免两个记录锁嵌套。错误风暴仍会消耗 CPU/RTC 总线时间，固定容量不表示零开销。

- app_main 在安全引脚配置成功后初始化，记录 Platform → Storage → Services → UI →
  StartupComplete。最后一个阶段在 StartPowerCoordinator 返回成功后发布，只表示
  启动装配返回，**不是业务健康或 OTA 有效性门**；REL-010 的确认点没有修改。
- EnterStorageRecoveryMode 记录原错误码与 observed generation，业务启动门保持原样。
- storage owner 记录真实执行失败及数字 request ID；队列准入失败/未开始就超时记录
  QueueTimeout。已开始事务仍等待其完成，保留原 context 生命周期，没有修改超时策略。
- ESP_ERR_NOT_FOUND 是通用空查询的正常结果，owner 不自动记成故障；业务域若认定
  缺失违反其契约，仍可明确上报。直接 owner 内嵌调用和所有错误枚举没有被逐一迁移，
  本批只承诺这些接入点的结构化上下文。
- PrintBootDiagnostics 导出一次 FAULT:boot 和至多四条 FAULT:record。已有 USB-host 延迟
  导出和显式睡眠诊断导出也附带摘要；热路径录制不打印，UI 不新增采样或同步写入。

FAULT 日志中 domain 为 Other/Boot/Storage/Audio/RTC/Network/AI/UI/Persist/Sync 对应 0..9；
phase 为 ReportedError/BootInit/StorageExecution/StorageQueueTimeout 对应 0..3；boot_phase
为 Platform/Storage/Services/UI/StartupComplete 对应 0..4。clock_source 沿用 wall_clock.h：
0 unknown、1 build、2 SNTP、3 RTC approximate、4 RTC SNTP-derived。时间和来源是相邻
观测，不构成任意 SMP 校时窗口的原子快照，也不证明时间经过认证或绝对准确。

## 候选身份与实际预算

运行时提交 `b667fe3a59d1ad03caf9e5c81b4d78afbb9ae322`，烘焙标识 `b667fe3a`，无 dirty 后缀。
从干净跟踪工作区执行 ESP-IDF v5.5.6 的 `idf.py -B build-wsl reconfigure build`，
EXIT=0，零编译警告，M8 ownership 与编译后 UI 栈门通过。随后仅更新文档。

| 对象 | 大小 / SHA-256 |
|---|---|
| build-wsl/wqn-zectrix-note4.bin | 3,649,920 B；`865bd4fb903984461a5c50cbe69661578198468f11a68810e1627bba047bc3d6` |
| build-wsl/wqn-zectrix-note4.elf | 41,838,760 B；`0663f49571e6309ebb6078e4d930cc3c01ef7498921739828af1fb194b6ec949` |
| sdkconfig | 76,758 B；`70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

最终 ELF/map 确认 g_copies 在 `.rtc_noinit`，总计 720 B；RTC slow 总量从第十批
0x1850（6224 B）变为 0x1b20（6944 B）。RAM cache 为 360 B，另有锁 8 B、active index
4 B、initialized 1 B，链接对齐另计。不会从内部堆再分配新的诊断任务栈或 PSRAM 历史。
这些是静态预算，不是实际 CPU/电流或 heap largest 的测量结论。保留区布局变化也不能
用来承诺任意旧固件升级后的 RTC 内容兼容。

配置摘要与第十批相同；故障摘要始终编译，后续关闭大型 memory diagnostics 的对照
也应保留它，以隔离该开关自身的代价。源码/产物清单为 `/tmp/note4-b11-identity.json`，
对象符号为 `/tmp/note4-b11-fault-object-symbols.txt`；构建日志为
`/tmp/note4-b11-build.log`。重构建可能改变构建时间和镜像摘要，核对设备实际身份。


## 验证与已有证据

| 检查 | 本轮结果 |
|---|---|
| 完整生产 fault journal、error_recorder、wall_clock | 5633 项断言通过，ASan/UBSan，保留 LeakSanitizer |
| 写入中断 | 实际非活动副本发布的 body 每个字节边界注入一次 reset，恢复上一份完整报告 |
| 反向控制 | 9 个 journal/入口错误变体及 1 个“空查询误报”变体均被行为断言拒绝 |
| 旧 error_recorder 接入 | 搭配新 journal 运行旧完整入口，因未产生保留报告被行为断言拒绝 |
| 生产 storage owner/caller | 错误码/request ID、排队超时、空查询不误报；原未开始取消/已开始等待行为通过 |
| 生产启动 | WiFi+Agent、WiFi/Agent off、offline 三种 host 配置通过；恢复原错误/generation，失败不发布完成阶段 |
| 既有 B4 回放 | 100/1000 Hz 各通过，延迟 Release 与 ring 修复保持 |
| 既有 ADC/UI guard | 100/1000 Hz 各 535 项通过，保持卡顿回退 |
| 新设备 RTC 保留/真实复位、远端 CI | 未执行 |

Journal 回归执行完整生产源文件与 public 类型。GPIO/SDK reset 原因、时钟系统调用、
ROM CRC、portMUX 是宿主端口；ROM CRC 端口校验标准向量。RTC 的 memcpy 写边界可控，
模拟重启时只重建 BSS 并保留/破坏选定字节，未给产品新增 reset/test API。还覆盖损坏/
随机内容、结构越界、序号绕回、计数饱和、任务并发报告与快照、旧 boot 来源保留、
敏感文本不进入 RTC、录制不进行 C++ 分配/日志。它们不证明物理 RTC 保留或 ESP32 SMP
真实时序；分配断言不能泛化为任意 libc/SDK 配置下的全系统无分配证明。

启动和存储组合继续执行完整生产 app_main/恢复入口及 worker/caller，摘要 API 在这些
fixture 中是计数端口；journal 自身的字节发布和恢复另由完整模块测试。记录范围没有
扩大为真实 NVS/SPIFFS、音频驱动或设备服务运行。

CI 新增 fault-diagnostics job，旧启动 job 随 fixture 更新覆盖阶段字段；本轮未运行远端。
主日志为 `/tmp/note4-b11-{fault,audio-runtime,boot,buttons,power,negative,build}.log`，
反向控制日志为 `/tmp/note4-b11-control-<name>.log` 与
`/tmp/note4-b11-empty-query-control.log`；旧入口为 `/tmp/note4-b11-before-error.log`。
临时证据可能被系统清理。

## REL-008/009：本轮证据利用和下一路线

额外核对工作区已有 `logs/serial-COM7-261023-111305-362-40024.log`：匹配到 20 条带
queue/exec/result 的事务完成记录，其中 4 条为 NOT_FOUND；queue 最大 9 ms，exec 最大
2369 ms，对应 save-sync-journal。片段没有可绑定的镜像身份或 SPIFFS 占用率，因此只用来
确认正常空查询语义和已有秒级事务，不作本候选性能、某占用率下的尾延迟或分位数结论。
摘录统计为 `/tmp/note4-b11-storage-log-review.json`。

当前 sdkconfig 已重新核对：memory diagnostics 开启，30 秒采样、24 小时历史、heap
tracing off。此前长录音/RAM证据继续保留；本批没有改变诊断任务节奏、低电阈值或紧急
保存预算，也没有把“没有 Flash 写入”当作无 CPU/锁/电流成本。

本节点通过后，下一批按以下顺序推进：先整理按镜像/boot 分组的现有 queue/exec 与
占用率证据；再准备与默认候选仅差诊断开关的对照配置，保持代码、供电和测试动作一致；
最后进行高占用内容更新/outbox/恢复、低电量负载和实际关机留存。没有占用率/样本数的
日志不进入容量曲线，少量样本只报告数量和最大值，不宣称已完成 P95/P99 或功耗验收。

## 当前设备节点

2026-10-23 用户回复“通过，继续”，本批记为用户确认节点验收。新增
`serial-COM7-261023-122209-519-37236.log` 仍未捕获 build/boot 身份或容量，
不能补记为下面逐项复位/故障注入的实测。后续转入
[第十二批](RELIABILITY_BATCH_12.md) 的存储证据和 RAM 诊断开/关对照。

1. 核对最终候选身份；启动时读取 FAULT:boot，正常学习/保存/录音与页面操作流畅，
   正常空 outbox 不持续增加 StorageExecution 故障。
2. 复用可控且不破坏现有离线数据的错误路径，记录报告的 code、operation/generation、
   build、boot ID 与时间来源；通过已有导出入口读取，再覆盖第五条替换/计数。
3. 用能确认 ESP_RST_SW 的软件重启与真实 deep-sleep 路径核对保留、前一装配阶段和
   原记录 boot 来源。F2/USB/JTAG reset 不算这项；没有相应入口或设施时保持待测，
   不以普通页面 retained standby 冒充 deep sleep。panic/WDT 的受控注入另记。
4. USB 重置/冷启动核对 recovered=0 的保守重建；物理断电后不承诺保留。实际重启时
   的 RTC 单副本损坏/撕裂验证和 CPU/时间成本不能用 host 注入代替。

用户常规“通过”与逐项复位/故障注入日志分别记录。该节点只验收最小故障摘要，B5
剩余的高占用、诊断对照、低电量和最终整机验证继续保留。
