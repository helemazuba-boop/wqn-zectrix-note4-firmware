# Note4 固件可靠性问题与补丁路线

记录日期：2026-10-21。分支：`fix/firmware-reliability`。
文档基线：`7851989`（文档归档）；最近的运行时代码提交为 `f936771`。

本文件记录当前问题、证据边界、维护空档和后续补丁的执行顺序。
初始版本只落地文档；后续实施状态见第 8 节与
[第一批实施记录](RELIABILITY_BATCH_01.md)。本文的方案和验收标准不表示设备已通过
验证，也不改变 [AGENTS.md](../../AGENTS.md) 中的所有权和提交规则。

## 1. 目标与范围

本分支围绕时间单位、离线数据保全、计时与 RTC、输入采集、故障留证推进。
采用以下顺序：记录基线 → 建立能捕获原问题的回归 → 最小行为修复 → 相关设备
验证 → 分批评审与提交。没有发现缺陷的模块可以以审计结论和回归补充收口。

第一轮交付限定为 B0–B2：统一验证基线、修复时间单位契约、复审启动存储恢复。
B3–B5 根据前一轮结果继续推进，允许每一轮独立验收。

补丁范围保持以下边界：

- 保留 generation 3、现有文件格式、设备协议和硬件 owner；修改恢复决策前先写清
  初始化、迁移、主动重置、异常恢复各自的允许行为。
- 保留 UI 异步持久化、operation_id ACK、EPD owner、按键生产时发布活动代数等
  已有契约。主机测试通过不替代设备并发或物理显示验证。
- 故障留证先考虑容量有界的摘要及导出路径。完整 OTA、分区重划、文件系统迁移、
  Secure Boot/Flash Encryption 属于独立设计项。
- 每个问题使用稳定的 `REL-xxx` 编号。修复后更新状态和证据，保留原始触发条件；
  单个问题关闭不等于整机或发布验收完成。

相关既有记录：

- [架构与资源所有权](../architecture/ARCHITECTURE.md)
- [音频设备验收矩阵](AUDIO_RUNTIME_REPAIR.md)
- [v3 同步与 journal 修复](SYNC_V3_REPAIR.md)
- [EPD idle 调度与待测场景](../ui/EPD_IDLE_SCHEDULER.md)
- [分钟刷新优化与证据边界](../ui/EPD_MINUTE_REFRESH.md)
- [固件与云端发布清单](../release/RELEASE_CHECKLIST.md)

## 2. 判断口径与当前基线

### 2.1 问题类别

| 类别 | 含义 | 关闭条件 |
|---|---|---|
| 确定缺陷 | 当前源码与实际 API/配置的契约存在明确不一致 | 回归捕获原错误，补丁通过相关检查，记录所需设备验证 |
| 策略待复审 | 源码行为明确，但需要根据现在的数据和产品要求重新判断 | 决策表、兼容边界和验收用例齐全，落实经评审的行为 |
| 验证缺口 | 有机制或测试，但证据没有覆盖所讨论的场景 | 补齐对应测量或故障注入，允许结果证实原设计无需修改 |
| 审计候选 | 历史维护空档或依赖演进提示需要复查 | 完成有边界的检查，记录发现或说明无需补丁的证据 |

P1 表示第一轮优先处理；P2 表示后续可靠性批次；P3 表示独立规划或按发现升级。
优先级不代表已经发生现场故障。

### 2.2 可复核的基线

| 项目 | 2026-10-21 快照 | 使用边界 |
|---|---|---|
| 分支/HEAD | `fix/firmware-reliability` / `7851989` | 后续结果须重新绑定实际提交和镜像身份 |
| 当前运行时基线 | `f936771`，含 `9ae4075`、`2456b45` 等近期显示修改 | 显示相关既有记录仍有 HIL 待测项 |
| 本地 SDK | 当前 `build-wsl` 使用 ESP-IDF 5.5.6；`dependencies.lock` 记录 5.5.6 | 锁文件不能独自证明 CI 或设备使用相同 SDK |
| CI SDK | `.github/workflows/firmware-ci.yml` 固定 `espressif/idf:v5.5.4` | 存在版本漂移，未在本次重跑远端 CI |
| 本地 tick | 忽略的 `sdkconfig` 中 `CONFIG_FREERTOS_HZ=100` | B0 必须导出有效配置；不能要求其他机器依赖此本地文件 |
| 本地内存诊断 | 30 秒采样、24 小时历史已启用 | 不是无诊断的最终功耗/性能配置 |
| 存储布局 | 两个 `0x3f0000` app 分区、8 MiB SPIFFS、16 KiB NVS | 当前无独立 coredump 分区 |

### 2.3 已有长测证据继续有效

此前的同次开机 RAM 记录覆盖 `78,418,969 ms`，约 21 小时 47 分钟。
记录含 10 组录音 begin/end、14 组 AI、21 组 Agent 操作；分配失败、sample_lost、
failure_lost 均为 0。初始化后内部/DMA 最大连续块均为 31,744 B，内部空闲量相对
约一分钟基线减少 768 B。该轮支持“原内部 RAM 耗尽症状未复现”。

对应分析位于工作区外 `doc/ram-review-20261020/analysis.json` 和 `RAM_REVIEW.md`；
它们是历史设备证据，不是本补丁候选的验收结果。原始串口日志 SHA-256 为
`9ed69f0ebbcd1123802f36091c49e8dcc065c03e3b8c965da5df2cd0eefd7b26`。

该证据没有证明所有 WiFi 问题已解决、所有音频模式均完成验收，也没有证明脱离
USB 后的功耗。新增补丁按影响补测，不为了延长时间而重复整套已覆盖的长测。

## 3. 问题总表

| 编号 | 问题/复查对象 | 类别 | 优先级 | 对应批次 | 初始状态 |
|---|---|---|---|---|---|
| REL-001 | I2S 毫秒与 tick 混用 | 确定缺陷 | P1 | B1 | 源码/API 已核实，未修复 |
| REL-002 | 1–2 ms 延时在 100 Hz 下变成 0 tick | 换算已确认；调用语义待审计 | P1 | B1/B3 | 未逐调用点验收 |
| REL-003 | schema/NVS 异常可触发全量本地重置 | 已修复，用户确认验收 | P1 | B2 | `1b95015` 禁止异常自动擦除，授权重置持久化；按用户确认收口 |
| REL-004 | 冷启动、离线与校时的时间可信度 | 策略待复审、验证缺口 | P2 | B3 | 未补专门回归 |
| REL-005 | 共享 I2C 锁与调用期限的关系 | 审计候选 | P2 | B3 | 未证实当前必然死锁 |
| REL-006 | 按键采集状态机与事件环边界 | 审计候选、验证缺口 | P2 | B4 | 上层导航测试不覆盖底层采集 |
| REL-007 | 统一错误记录和跨重启故障留证 | 能力与验证缺口 | P2 | B5 | 当前仅有有限普通 RAM 错误环等机制 |
| REL-008 | SPIFFS 高占用下的存储尾延迟 | 验证缺口 | P2 | B2/B5 | 已有容量/GC 策略，缺本候选压力证据 |
| REL-009 | 诊断开销、低电量负载与紧急持久化预算 | 验证缺口 | P2 | B5 | 无关闭诊断的功耗对照结论 |
| REL-010 | 镜像有效确认早于完整业务启动 | 策略待复审 | P3 | 后续独立设计 | 已核实顺序，未实现完整升级健康门 |
| REL-011 | 本地与 CI SDK、host 时间语义不一致 | 确定配置漂移、测试边界缺口 | P1 | B0/B1 | 未统一 SDK，未增加 I2S 边界测试 |
| REL-012 | Note/Problem 云端适配层的维护空档 | 审计候选 | P3 | 后续按发现进入 | 不等于整个业务模块失修 |

## 4. 问题详情与处理要求

本节描述审计基线 `7851989` 的证据与处理要求。修复后的状态以第 8 节和
第一批实施记录为准，不能把基线代码描述当作补丁后的现状。

### REL-001：I2S 超时参数发生重复单位转换

证据入口：[audio_service.h](../../main/services/audio_service.h)、
[audio_service.cpp](../../main/services/audio_service.cpp) 的 `ReadAudioChannel` /
`WriteAudioChannel`，以及 [flash_session.cpp](../../main/flash_session.cpp)、
[audio_capture.cpp](../../main/audio_capture.cpp) 的调用点。

服务接口接收 `TickType_t timeout`，调用方传入 `pdMS_TO_TICKS(...)`；服务将数值
原样交给 `i2s_channel_read/write`。ESP-IDF 5.5.6 的新 I2S API 接收
`uint32_t timeout_ms`，驱动内部再换算成 tick。

| 调用方标称毫秒 | 100 Hz 下首次换算值 | 实际传入 I2S 的毫秒值 |
|---|---|---|
| 100 | 10 | 10 |
| 200 | 20 | 20 |
| 250 | 25 | 25 |
| 1000 | 100 | 100 |

驱动内部还会按 tick 分辨率截断，例如 25 ms 变成 2 tick 的等待预算。驱动可能
在多个等待点分别使用该预算，表格不是整个读写函数实测耗时或绝对完成期限。
Flash 采集注释明确期望 200 ms，当前实参却是 20 ms。该错误可减少抖动余量，
但不能仅凭静态发现断言已有录音全部丢帧或将所有爆音归因于它。

历史关联：`bb4960d`（7/14 音频修复）、`cbc3f4b`（7/19 音频所有权封装）。
这些提交用于定位契约演变，不单凭 blame 推断最初引入错误的提交。

处理要求：

- 明确服务边界采用毫秒，参数命名体现单位；核对全部调用者和无限等待语义。
- 增加真正经过生产读写封装的测试，检查传给驱动的实参，覆盖 100/1000 Hz。
- 回归必须能捕获原始代码的错误；只将宏定义成原值不能完成此项验收。
- 设备验证覆盖采集 warmup、持续录音、播放、停止/取消及与显示工作重叠。
  保留 PCM、超时和清理证据，观察等待预算变长是否影响停止响应。

API 依据：[ESP-IDF 5.5.6 I2S 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/peripherals/i2s.html)。
本地对应实现位于 IDF 的 `components/esp_driver_i2s/i2s_common.c` 和
`include/driver/i2s_common.h`。

### REL-002：短延时没有得到所写的毫秒等待

入口包括 [wake_controller.cpp](../../main/power/wake_controller.cpp) 的
原 `pdMS_TO_TICKS(2)`、[power_manager.cpp](../../main/power_manager.cpp) 的 ADC 采样间隔，
以及 [display_service.cpp](../../components/display_service/display_service.cpp) 中的
原有的 `pdMS_TO_TICKS(1)`。

100 Hz 下，1 ms 和 2 ms 均换算成 0 tick。`vTaskDelay(0)` 可能让出执行权，
但不保证写在注释里的硬件稳定时间或采样间距。换算效果确定，各调用点是否违反
实际硬件要求还需逐项判定。

建立调用点清单，分别标注“仅让出 CPU”“至少等待某个时长”“轮询间隔”，再选择
适当的延时机制。保持 100 Hz 基线；不能用全局升高 tick 或统一替换为一个 tick
掩盖单位问题。若涉及 wake pin 稳定或 EPD 时序，按硬件修改验证。

第五批尝试区分 6 处用途，但 ADC 的粗粒度最小等待引入设备卡顿，用户验收失败。
`0feba06` 撤回 ADC 部分，恢复原有 yield；原表达式断言被完整批次/同步 UI 调用
延时回归替代。wake/EPD 等待保留，ADC 物理采样间隔需在移出 UI 调用链后另行
设计。先完成[响应性复验](RELIABILITY_BATCH_05_HOTFIX.md)，不将本项标为收口。

### REL-003：启动恢复对所有本地数据采用同一重置策略

入口：[storage_schema.cpp](../../main/runtime/storage_schema.cpp) 的
`EnsureStorageSchema`、`ReadSchemaMarker`、`PerformFullLocalReset`；
[main.cpp](../../main/main.cpp) 的启动门和 `EnterStorageRecoveryMode`。

第一批基线（`52bb42b`）已核实的行为；第二批已提交修复改变了下列重置决策，见
[RELIABILITY_BATCH_02.md](RELIABILITY_BATCH_02.md)：

- `ESP_ERR_NVS_NO_FREE_PAGES`、`ESP_ERR_NVS_NEW_VERSION_FOUND` 会进入重置流程。
- marker 读取出错会转成“不可信 generation”；marker 缺失、generation 不匹配或
  reset_id 大小不正确，也不能通过 current-schema 判断。
- `PerformFullLocalReset` 擦除默认 NVS，并格式化整个 storage SPIFFS；恢复 marker
  在格式化及校验成功后提交，以支持被打断后的重做。
- current-schema 分支的 SPIFFS 挂载失败会进入恢复模式；这里
  `format_if_mount_failed=false`。不能把它误写成“所有挂载失败都会自动格式化”。
- 现有恢复模式阻止业务启动，展示错误并等待维护；这提供了故障隔离，但不等于
  已有可导出数据、可自主修复的完整恢复工具。

`24463fb` 的 generation 3 cutover 明确把本地数据视作可丢弃。后来 Word/Note/
Problem 已有尚未上传的 durable observation，必须复审这两种假设的适用边界。
云端内容缓存可重建，不代表未上传记录也能重建。

先补真实实现回归，再决定行为。现有 `test_sync_boot_recovery.py` 验证启动顺序和
journal 门，但其 fixture 将 `EnsureStorageSchema()` 替换为直接返回 Ready 的桩，
没有执行擦除/格式化决策。新测试应执行生产 schema 实现并注入 NVS/SPIFFS 返回值。

必须形成并评审的决策表：

| 场景 | 回归必须记录的事实 | 补丁决策目标 |
|---|---|---|
| 全新设备、明确空介质 | 初始化和 marker 提交次序、一次预期重启 | 保留可用的首次初始化 |
| 当前 generation、有效 marker | 不调用 erase/format | 正常启动保留内容与 outbox |
| marker 不可读/部分缺失 | 实际错误、介质与旧记录存在性 | 信息不足时采用可解释的保全/恢复策略 |
| NVS 两类特殊错误 | 错误来源及 SPIFFS 是否仍可读取 | 避免把 NVS 问题直接等同于业务记录可丢弃 |
| 明确旧 generation | 原迁移承诺、回滚兼容与数据处置 | 明确允许的迁移行为，保持幂等性 |
| 主动恢复出厂设置 | 明确用户意图与持久化操作顺序 | 保留既有主动重置能力 |
| erase/format/marker commit 任一步失败 | 终止点、下一次启动结果、业务是否被门控 | 失败可诊断，业务不越过完整性门 |

仅靠本轮补丁不能承诺修复物理损坏的数据。真实断电和损坏注入使用受控测试设备，
先保护唯一设备上的未上传记录；软件 reset、拔插串口不等同于断电验收。

### REL-004：时间可运行、可用于 TLS 与可信墙钟尚需区分

入口：[rtc_timekeep.cpp](../../main/power/rtc_timekeep.cpp)、
[time_app.cpp](../../main/time_app.cpp)、
[time_app_retention.cpp](../../main/ui/time_app_retention.cpp)、
[wifi_manager.cpp](../../main/wifi_manager.cpp)、[wqn_api.cpp](../../main/wqn_api.cpp)。

`RestoreSystemTimeFromRtc` 限制为深睡恢复且信任记录有效；其他启动可能使用编译
时间。HTTPS 的 reasonable 检查主要使用 2024 年下界。WiFi 获得 IP 后会主动启动
SNTP，因此问题不是“编译时间让设备永远不再校时”，而是离线、首次校时前及校时
跳变期间，哪些消费者允许使用当前时间。

`RunTimeAppStateSelfTest` 已覆盖部分暂停、阶段转换和校时修正，不能写成完全没有
测试；缺口主要是 RTC 信任、状态保留、启动顺序与这些行为的组合验证。

B3 先定义各消费者语义，再补以下用例：深睡恢复、冷启动离线、RTC VL 置位、RTC
读写失败、SNTP 前后跳变、运行/暂停中校时、跨睡眠恢复和多轮番茄钟。保持单调
时间用于本次开机的经过时长、墙钟用于跨睡眠恢复的边界可解释。可信度的表示方式
与是否需要新增快照字段由实际审计决定，不预先要求扩大架构。

第四批 RTC 信任节点已由用户确认；第五批补齐生产计时/保留状态组合回归，并修复
“墙钟回拨一小时使睡前 120 秒恢复成 3,720 秒”。恢复值现在不超过睡前预算。
这些补丁不等于完成 build seed / TLS / 可信时间来源的全部消费者政策。

第七批已按真实入口记录消费者现状：RTC trust 表示已验证写入和睡眠代次，未记录
SNTP 来源；TLS、UI 和学习记录仍可使用合理年份的 build seed。计时的本次开机
经过量继续使用单调时间。完整现状、剩余来源分级与恢复政策见
[第七批消费者表](RELIABILITY_BATCH_07.md#时间消费者现状和剩余路线)，不将 REL-004 标成关闭。

### REL-005：共享 I2C 的等待是否服从上层期限

入口：[i2c_bus_lock.h](../../main/i2c_bus_lock.h)、
[i2c_bus_lock.cpp](../../main/i2c_bus_lock.cpp)、[pcf8563.cpp](../../main/pcf8563.cpp)、
[audio_service.cpp](../../main/services/audio_service.cpp)。

共享总线已有递归互斥和 codec 的部分 reset/重试路径。锁接口默认
`portMAX_DELAY`；需逐调用点核对外层 deadline 是否覆盖等待锁、实际事务和恢复。
默认无限等待本身不证明当前存在死锁，也不能把 codec 已有恢复写成整个 I2C 都没有
恢复机制。

审计输出应包含调用者、资源锁顺序、剩余预算、返回错误、故障后受影响设备范围。
验证持锁竞争、RTC NACK/超时、codec 失败及睡眠准备时的组合。只修复能证明违反
调用期限或所有权的路径，避免统一重置总线影响另一个正在工作的设备。

第五批已将 wake 的同一个绝对 deadline 传入 RTC timer 每步锁/驱动操作、回退
和回滚，并拒绝晚成功。IDF 内部多重等待不能由调用参数证明硬实时 WCET；codec
完整调用预算和外层电源事务仍须继续审计，不能把此项标成全部关闭。

第六批 `30ea9c3` / `3f6ec1a` 已为 codec 配置及独立寄存器调用加入 5 s 协作式
预算，显式限制共享等锁；整组配置复用同一预算并保留逾期清理。关闭新操作准入
后的私有句柄清理保留 session/槽位检查，清理失败停止整组重试。生产完整配置、
正常路径成本对照和故障回归通过；SDK 内部等待/清理的硬上界和设备共总线证据
仍不能由参数模型替代，见 [第六批记录](RELIABILITY_BATCH_06.md)。

第七批 `7c88893` 将 prepare 的绝对 deadline 接入睡前 RTC persist、整组写钟和
校验；超时撤销信任，STOP 结果不确定时保留一次单独的释放尝试。主机回归覆盖晚
返回及设备已应用的错误结果；这个工作预算不能替代 SDK/清理硬上界或真实故障恢复
证据，见 [第七批记录](RELIABILITY_BATCH_07.md)。

### REL-006：底层按键采集没有随上层交互得到同等回归覆盖

入口：[button_input.cpp](../../main/button_input.cpp)、
[button_event.h](../../main/button_event.h)、
[ui_input.cpp](../../main/ui/ui_input.cpp)。

核心 ISR/采样/静态事件环在 7 月末建立。`33e1a29` 的 10 月修改仅将双击窗口引用
统一为共享常量。新的导航、模态框和录音逻辑增加了上层消费者组合。

`test_ui_navigation.py` 执行上层生产处理逻辑，但输入为 fixture 构造的
`ButtonEvent`，不能由此认定 GPIO 采集、去抖、ring 满时策略已经验收。B4 测试应
执行真实采集状态机与入环逻辑，以可控时间/GPIO 输入覆盖：

- 短按、hold、长按、双击边界、release 尾事件、两键交叠和长时间按住。
- 事件环满、repeat 合并/淘汰、关键事件丢弃计数及恢复；以既有 ring 契约为准，
  不承诺有限容量下无限输入零丢失。
- UI 忙与唤醒窗口中，活动 generation 在关键事件发布前更新的顺序。

静态回放可以证明状态机行为，GPIO 实际抖动、跨核竞争和深睡交界仍需设备证据。
排查时分别记录输入被采集、AppState 已变化、EPD 已呈现，避免把显示延迟误诊为
按键没收到。

### REL-007：错误记录缺少跨重启留证闭环

入口：[error_recorder.cpp](../../main/error_recorder.cpp)、
[error_recorder.h](../../main/error_recorder.h)、
[memory_diagnostics.cpp](../../main/runtime/memory_diagnostics.cpp)、
[分区表](../../partitions/16m.csv)。

统一错误环深度为 8，位于普通 RAM；当前本地配置 coredump 为 NONE，分区表没有
coredump 分区。已有 RTC 睡眠诊断和 RAM 历史各有范围，不能将项目描述成完全没有
诊断，也不能将内存趋势当作 panic 调用栈。

先明确最小摘要：固件身份、错误所属阶段/域、错误码、operation/generation、启动
与复位原因、时间可信度和覆盖/丢失计数。决定普通 RAM、RTC 保留或持久化介质的
适用范围，给出掉电后能留下什么、不能留下什么。摘要有容量上限、无 token/凭据，
错误风暴不引入频繁 Flash 写入。

完整 core dump 涉及分区容量、任务栈位置、PSRAM 数据覆盖和恢复工具，进入独立
设计；本轮不预先以全量转储作为最低交付要求。

### REL-008：容量准入不能替代高占用下的尾延迟验收

入口：[storage.cpp](../../main/storage.cpp) 的 `EnsurePackDownloadCapacityTransaction`、
[storage.h](../../main/storage.h) 的 256 KiB 默认安全余量、
[storage_service.cpp](../../main/services/storage_service.cpp) 的事务生命周期。

当前已清理可丢弃临时文件/图片缓存，并尝试有上限的 GC。StorageService 对所有写入
串行化；前台排队可超时放弃，已运行事务需等待完成以保护 context 生命周期。
因此不能把 15 秒排队/等待参数表述为单个事务的硬完成期限。

IDF 的 SPIFFS 说明明确其不是实时文件系统，可靠可用容量约为分区的 75%，空间
紧张时 GC 可使一次写入耗时数秒。该说明是设计边界，不是本设备测量结论；也不能
直接将 75% 当作本产品已经验证的精确容量阈值。本地依据为对应 IDF 版本的
`docs/en/api-reference/storage/spiffs.rst`。

先测不同占用率下的 queue/exec P50/P95/P99/最大值、失败码和前台保存响应，再考虑
容量水位或回收调度补丁。用例包括多域内容包更新、旧/新包及备份并存、图片淘汰、
outbox 追加/ACK/整理，以及空间不足后恢复。只有确实测到的分位数才报告分位数；
少量样本记录最大值与原始样本数。

### REL-009：低电量与诊断开销需要独立测量

入口：[power_manager.cpp](../../main/power_manager.cpp) 的 `ReadPowerStatus`、
`IsBatteryVeryLow`、`RunBatteryEmergencyShutdown`，以及
[memory_diagnostics.cpp](../../main/runtime/memory_diagnostics.cpp)。

当前极低电压判断使用 3430 mV；ADC 有多次采样和校准路径。紧急关机等待 storage
blocker 的预算为 2 秒，之后继续服务准备及关机。它是能量不足时的既有取舍，不能
宣称能保存尚未持久化的所有意图。REL-002 还涉及 ADC 采样间隔的真实语义。

当前诊断任务每秒唤醒；周期及业务边界采样会查询 internal/DMA/PSRAM heap。
已有评估约占 115.5 KiB PSRAM 与 1.6 KiB 静态内部内存。其 heap 查询和导出也有
CPU/锁/串口成本；诊断没有主动联网或持续写 Flash，不应误称为持续 Flash 磨损源。

设备测量覆盖低电量时 WiFi/音频/EPD 各负载及实际可出现的重叠、稳态与瞬态电压、
关机前持久化完成情况。对照构建除诊断开关外保持一致；USB 主机连接会改变睡眠
策略，电池测量与 USB 日志分开解释。改阈值、滞回或等待预算以前先取得证据。

### REL-010：升级健康判定与维护恢复能力待独立完善

入口：[main.cpp](../../main/main.cpp) 的 `ConfirmRunningApp`、
[sdkconfig.defaults](../../sdkconfig.defaults)、[分区表](../../partitions/16m.csv)。

已有双 OTA 分区和 bootloader rollback 配置；正常启动中有效性确认早于 AI、UI、
SyncService 和 PowerCoordinator 的完整启动，迁移重启前另有确认调用。前一个
确认点不能直接当成“完整业务健康已经通过”。

后续需分别定义迁移意图重启、正常启动健康门、失败回滚及已有离线数据的兼容关系。
源码中现有调用和分区不证明已经有完整的设备 OTA 下载/验签/升级产品流程。
本轮先保留问题记录，不移动确认点或更改分区。

### REL-011：验证环境与测试替身会掩盖设备差异

入口：[dependencies.lock](../../dependencies.lock)、
[CI workflow](../../../../.github/workflows/firmware-ci.yml)、
[audio_runtime fixture](../../scripts/testdata/audio_runtime.cpp)、
[sync_boot_recovery fixture](../../scripts/testdata/sync_boot_recovery.cpp)。

本地 SDK 与 CI 容器版本不一致已确认；本记录没有由此推断远端构建必然失败。
B0 选择经核验的同版 SDK/容器，按正常依赖解析过程核对 lock，而非仅手改版本号
后声称环境一致。核查从 defaults 新建配置时，关键功能和内存配置实际生效。

音频 runtime fixture 将 `pdMS_TO_TICKS(ms)` 定义为原值；该套测试主要验证 token、
存储分派和 idle cleanup，不能据此说它已经测试过 I2S 驱动时间语义。新增边界测试
应针对 REL-001；同样，启动门测试不覆盖被替换掉的 schema 实现。

### REL-012：较旧的 Note/Problem 云端适配层

入口：[note_cloud.cpp](../../main/note_cloud.cpp)、
[problem_cloud.cpp](../../main/problem_cloud.cpp)、
[ui/note_cloud.cpp](../../main/ui/note_cloud.cpp)、
[ui/problem_cloud.cpp](../../main/ui/problem_cloud.cpp)。

较久未改的是内容同步与 UI/cloud 适配层，不是整个 Note/Problem 功能。Problem
outbox 10/9 仍有 `dc1f2b4` 修复；近期页面、分页和呈现也有实质更新。Problem 的
当前离线 pack 浏览没有 Note 式 session/candidate paging，不能套用同一迁移清单。

后续审计关注新页面与旧适配层的 operation/generation 绑定、失败后的 lease/busy
归还、隐藏页面资源、Note 的 session/candidate 持久化和同步恢复。发现具体缺陷后
分别进入 Note 或 Problem 的小补丁，保留两域独立验证。

## 5. 提交历史显示的维护空档

统计固定在基线 `7851989`，使用非合并提交和当前路径的历史；窗口从 2026-09-21
开始至基线。次数是“触及该文件的提交数”，不等于工作量、审查次数或质量分数。
选中模块另用 `git log --follow` 和提交 diff 核对清理、日志与实质行为变更。

| 当前实现 | 路径历史提交数 | 窗口内提交数 | 最后修改及解释 |
|---|---|---|---|
| `main/runtime/storage_schema.cpp` | 1 | 0 | 7/19 `24463fb`，后续 outbox 需求变化后仍待复审 |
| `main/time_app.cpp` | 6 | 1 | 10/18 `69038f0` 仅删未使用接口；核心行为主要停在 8/23 |
| `main/ui/time_app_retention.cpp` | 1 | 0 | 8/19 `3a20ea0` |
| `main/power/rtc_timekeep.cpp` | 2 | 0 | 8/31 `a242136` |
| `main/pcf8563.cpp` | 6 | 0 | 9/7 `d653085` 仅加错误记录；此前实质更新在 8/24 |
| `main/i2c_bus_lock.cpp` | 1 | 0 | 8/24 `d6a06a6` |
| `main/button_input.cpp` | 12 | 1 | 10/16 `33e1a29` 仅统一常量引用；核心行为停在 7/31 |
| `main/error_recorder.cpp` | 1 | 0 | 9/7 `d653085` 创建 |
| `main/note_cloud.cpp` / `main/problem_cloud.cpp` | 9 / 3 | 0 / 0 | 两者均为 8/17 `a43e51d` |
| `main/ui/note_cloud.cpp` / `main/ui/problem_cloud.cpp` | 14 / 6 | 0 / 0 | 分别为 8/26 `7c7d03c`、8/11 `1e3abb6` |
| 对照：`main/opencode_session.cpp` | 45 | 42 | 10/20，持续活跃 |
| 对照：`main/word_study_store.cpp` | 36 | 21 | 10/18，持续活跃 |
| 对照：`main/device_ui.cpp` | 74 | 20 | 10/21，持续活跃 |

维护空档用于决定审计顺序。小型稳定驱动、已完成的静态协议实现或没有需求变化的
组件可以长期不改。EPD、电源协调、音频和同步服务均有持续修复，不能统称为长期
忽视；反过来，近期 UI 重绘也不等于底层采集和存储恢复已得到验证。

复查示例，在仓库根目录执行；后续应显式固定希望比较的基线：

```sh
git log 7851989 --no-merges --date=short --format='%h %ad %s' -- \
  firmware/wqn-zectrix-note4/main/runtime/storage_schema.cpp
git log 7851989 --follow --no-merges --date=short --format='%h %ad %s' -- \
  firmware/wqn-zectrix-note4/main/button_input.cpp
git show 69038f0 -- firmware/wqn-zectrix-note4/main/time_app.cpp
git show 33e1a29 -- firmware/wqn-zectrix-note4/main/button_input.cpp
```

## 6. 分批实施路线

### B0：固定代码、配置、工具链和证据基线

交付：本记录、候选身份清单、同版本 SDK/CI 决策及基线检查结果。

1. 记录 HEAD、有效 sdkconfig、IDF revision、依赖锁、镜像 SHA-256 与分区布局。
   配置只保存去除凭据后的版本，日志也不包含设备 token/WiFi 密码。
2. 解决 REL-011 的 SDK 漂移，验证新建配置与本地关键设置的差异；记录差异原因，
   不把忽略的本机 sdkconfig 当成所有构建的默认值。
3. 跑现有构建和相关测试，记录既有失败/待测项；将既有 RAM、音频、同步、显示
   证据绑定其原候选，补丁前后可以对照。

建议提交粒度：文档记录与 SDK/CI 配置分别提交。文档基线先建立不表示 SDK 漂移
已经解决。第一轮后续依赖本批身份记录。

### B1：修复时间单位并建立真实 API 边界测试

对应 REL-001、REL-002、REL-011。先补会捕获原问题的 I2S 回归，再调整接口和
调用方。短延时按调用用途单列处理；EPD/唤醒时序部分可以延后到已具备 HIL 的批次。

验收：100/1000 Hz 下驱动收到正确毫秒实参；正常读写、超时、部分完成、取消和
资源释放有覆盖；设备录音/播放与停止响应符合预算。调整较大等待预算时检查
既有 DriverOperation 收尾机制，不能靠吞掉错误或持续加长等待掩盖失败。

建议提交：音频单位契约及其回归一组；短延时按硬件/模块再分组。

### B2：建立启动存储实现回归，再落实保全策略

对应 REL-003，包含 REL-008 的初步压力/容量基线。

先直接执行 schema 实现并注入返回值，覆盖第 4 节决策表。评审后以最小改动区分
首次初始化、授权重置、明确迁移与不确定故障。保持 generation 3 和既有记录格式，
检查旧设备升级及掉电重入，不通过抹掉原数据来制造“恢复成功”。

验收：正常启动不擦除；主动重置可完成；选定故障策略符合保全决策；任一完整性
步骤失败阻止业务越门；受控设备的异常恢复和真实断电证据齐全。
先提交测试基础，再按经评审的策略提交行为补丁。不要用测试桩代替生产决策逻辑。

### B3：计时、RTC 信任与共享 I2C 的期限

对应 REL-004、REL-005，以及剩余 wake 短延时。

先复用现有计时自测扩充边界，建立真实 RTC/保留状态逻辑的回归，再核对调用链锁
顺序与预算。审计通过的路径记录结论；需要改行为的计时、RTC、I2C 补丁分别评审。

验收：校时不破坏已定义的计时语义；异常 RTC 不被当作可信墙钟；故障及锁竞争不
悄悄超出上层约定；跨睡眠、按键唤醒和音频共总线的实机组合通过。

### B4：输入采集与消费链路

对应 REL-006。将底层采样、状态机、ring 策略纳入可控时钟/GPIO 的生产代码回放，
与现有导航/模态/UI 回归组合。修复仅限复现或契约证据明确的行为。

验收：手势边界、ring 压力和尾事件有覆盖；可区分采集、状态、呈现三段；不破坏
生产时发布活动的顺序。实机补 GPIO 抖动与睡眠交界场景。

### B5：最小故障摘要与整机收口

对应 REL-007、REL-008、REL-009。先补错误归因和容量有界的摘要，再完成低电量、
高占用存储和诊断开/关对照。若引入跨重启保存，先说明介质、保留条件与故障下
可写性，避免把所有错误路径变成同步 Flash 写入。

整机候选应具有明确代码/配置/镜像身份，复用既有证据，只补补丁影响的场景。
本分支的可靠性补丁验收与联合云端发布门分别记录。REL-010、REL-012 可在独立
审计完成后形成后续计划，不自动延长第一轮交付。

## 7. 验证与提交门

### 7.1 每个行为补丁的共同要求

- 回归说明原触发条件和补丁效果；能执行生产实现的地方优先执行生产实现，避免
  复制算法后仅证明复制版本通过。
- 默认构建 0 warnings，M8 ownership gate 和编译期 UI 栈检查通过；涉及条件编译
  的修改按影响选择 Agent off、headless/offline、Flash v1 等配置。
- 相关测试通过后仅因新变化或未解决问题扩大/重复测试；文档变更无需重跑无关的
  全量设备或 host 矩阵。
- 每个逻辑批次可独立评审和回退。回退时保留失败现场与身份记录，不通过恢复
  出厂设置掩盖回归。
- 遵循 AGENTS.md 的 HIL-before-commit 和评审规则；既往对某一批 HIL-pending
  本地提交的批准不是本分支所有高风险补丁的通用豁免。

### 7.2 已有本地检查入口

以下从固件目录执行；它们是现有入口，不代表本文件初始版本已经执行全部命令。
先加载本机实际 ESP-IDF 环境，沿用已有 `build-wsl`。

```sh
source <esp-idf-dir>/export.sh
idf.py -B build-wsl build
git -c core.whitespace=cr-at-eol diff --check

# 按补丁影响选择现有回归，不把它们的范围扩大为所有硬件行为。
python3 scripts/test_audio_runtime.py
python3 scripts/test_audio_rx_dma.py
python3 scripts/test_audio_channel_timeout.py
python3 scripts/test_storage_schema.py
python3 scripts/test_sync_boot_recovery.py
python3 scripts/test_sync_journal.py
python3 scripts/test_sync_control.py
python3 scripts/test_sync_retry.py
python3 scripts/test_ui_navigation.py
python3 scripts/test_memory_diagnostics.py
python3 scripts/test_epd_refresh_windows.py
python3 scripts/test_epd_idle_scheduler.py
```

I2S 超时边界与真实 schema 决策回归已新增。尚待补充的实现回归为 RTC/计时保留和
底层按键采集，当前不预设它们已经存在的脚本名称。

### 7.3 针对变更补充的设备证据

| 批次/场景 | 重点证据 | 不能用来替代的证据 |
|---|---|---|
| B1 音频时间单位 | PCM 连续性/错误、warmup、取消/停止、显示负载重叠 | 仅 host 实参正确或仅有 begin/end |
| B1/B3 短延时与唤醒 | 实际 wake line、正常/取消睡眠、EPD 时序 | 仅 100/1000 Hz 模拟通过 |
| B2 schema/恢复 | 保留数据升级、介质错误、明确初始化/重置、受控断电重入 | 桩函数返回、USB 重连或软件 reset |
| B3 计时/RTC/I2C | 深睡恢复、校时、RTC 故障、codec/RTC 竞争及回收 | 单纯计时 UI 像素测试 |
| B4 输入采集 | 实际 GPIO 抖动、长按尾事件、ring 压力、睡眠交界 | 只构造上层 ButtonEvent |
| B5 存储容量 | 不同占用率、真实写入尾延迟、失败后恢复与数据内容 | 空 journal 或仅测队列等待 |
| B5 功耗/低电 | 同板同配置对照、实际电流/电压、负载与关机记录 | USB 在线待机或 RAM 稳定趋势 |
| B5 故障摘要 | 重启后可取回的内容、覆盖/丢失计数、构建身份匹配 | 同次开机 RAM 中还能读取 |

### 7.4 每批证据最小字段

记录问题编号、提交范围、有效配置/SDK、app SHA-256、测试命令与退出结果、
设备/供电/运行模式、触发操作、预期与实际、日志或测量位置、未覆盖项及回退点。
每条结果明确标注 host、build、设备 HIL 或人工观察；SKIP/PENDING 均不计为 PASS。

## 8. 执行台账

状态更新于 2026-10-23；下面仅表示本补丁路线的新工作，不抹除历史测试结果。

| 批次 | 状态 | 当前已具备的内容 | 下一交付 |
|---|---|---|---|
| B0 | 本地基线完成 | 文档 `6dcbd8a`；CI 对齐 5.5.6、tag 核验、干净 defaults 构建及配置差异记录 | 远端 CI 未执行；后续候选继续绑定自身身份 |
| B1 | 音频代码与 host/build 完成 | 7 处调用毫秒契约及 100/1000 Hz 回归；原基线与两种错误变体均被捕获 | 音频 HIL；REL-002 短等待已在第五批实施，硬件节点单列 |
| B2 | 已本地提交，用户确认验收 | `1b95015`：异常保全、严格空介质检测、持久重置意图与逐命名空间清理；第二批记录见 RELIABILITY_BATCH_02.md | REL-003 已收口；REL-008 容量/尾延迟验证留 B5，后续推进 B3 |
| B2 后续：运行期 outbox | 用户确认节点验收通过 | `f25f3a0` / `9d2a40f` / `60d9db8` / `9bdf096`；用户于 2026-10-22 回复“通过 继续推进”，未附逐项设备日志 | 第三批范围与证据边界保留；进入 B3 |
| B3 | 第六批用户确认；第七批睡前 RTC 补丁已实施 | `7c88893` / `1350856`：共享 prepare 剩余预算、不确定 STOP 的清理、晚成功撤销信任；时间消费者现状已入档；ADC 回退保持 | RELIABILITY_BATCH_07.md RTC 设备节点；来源分级和恢复政策仍待后续实现，再进入传输专项 |
| B4 | 未实施 | 采集层与上层测试范围已区分 | 真实采集/ring 回放与设备场景 |
| B5 | 未实施 | 既有诊断与历史 RAM 证据可复用 | 摘要、尾延迟/功耗证据和候选收口 |

后续在对应行补充提交、验证记录和剩余项。确定无需行为补丁的审计也应记录
检查范围和依据。新增事实推翻早期推断时，修订问题类别与优先级，不保留误导性的
“确定故障”措辞。

### 全仓审查补充问题的交付映射

审查以 `13c436b` 为基线，R 编号保留来源，不将其混同于 REL 编号。用户授权先
本地提交、在关键节点集中验证；第三批已按此节奏实施，设备验收仍单列。

| 审查编号 | 所属批次 | 当前状态 |
|---|---|---|
| R-01 RTC I2C 时间单位 | B3 / REL-005 | `074d4b6` 已修复；第四批用户确认验收，未附逐项设备日志 |
| R-02 HTTP SSE 整轮截止时间 | 传输专项 | 待最小复现及有界请求补丁 |
| R-03 累计帧溢出未被旧消费者处理 | 传输专项 | 待消费者回归及终止行为修复 |
| R-04 三域 outbox 发布失败丢数据 | B2 后续 / 第三批 | 代码和 host 回归通过；用户确认节点验收，未附逐项设备日志 |
| R-05 Note 失败追加后缓存仍有效 | B2 后续 / 第三批 | 同上，含 ACK/挂起修复 |
| R-06 Problem 残尾和不严格查重 | B2 后续 / 第三批 | 同上，含生产 codec 和完整帧回归 |
| R-07 RTC 写失败沿用旧信任 | B3 / REL-004 | `f986842` 已修复；第四批用户确认验收，未附逐项设备日志；第五批继续通过相关回归 |
| R-08 REST/WS 地址覆盖不一致 | 传输/配置专项 | 待路由一致性回归与修复 |

## 9. 初始文档落地验证

2026-10-21，初始文档批次只新增本计划并更新文档索引及中英文入口，未修改运行时代码、
配置或 CI。下面的 PASS 仅属于这次文档变更，不关闭 REL-001–REL-012，也不将
B1–B5 标为已实施。

| 检查 | 结果与范围 |
|---|---|
| 问题/批次完整性 | REL-001–REL-012、B0–B5 标题与台账齐全 |
| 文档相对链接与代码围栏 | 4 个变更文档中 70 个相对链接存在，围栏成对 |
| 历史 RAM 证据身份 | 重新计算原始日志 SHA-256，与分析文件及本计划记录一致 |
| `idf.py -B build-wsl build` | 增量构建 EXIT=0，0 warnings，M8 ownership 与编译后 UI 栈检查通过 |
| CRLF-aware `git diff --check` | PASS |
| 新的行为测试/设备 HIL | 未执行；本次没有运行时补丁 |

本机构建输出保存在 `/tmp/note4-reliability-docs-build.log`。该文件只是本次检查
日志，可能被系统清理；后续补丁候选须按第 7.4 节重新留存自己的身份与验收记录。
