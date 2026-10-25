# 第五批回归修复：ADC 同步读取阻塞 UI

日期：2026-10-22。分支：`fix/firmware-reliability`。起点：`3352373`。

> 当前需要执行的验收见[统一验收清单](FIRMWARE_RELIABILITY_ACCEPTANCE.md)；本文保留历史范围与证据，不要求逐批重复设备矩阵。

## 结论与验收状态

用户反馈“无法通过……现在设备卡的像 PPT”，第五批标为设备验收失败。确认由本批
引入的一处回归：`6071ef4` 将同步 ADC 采样中的旧 yield 改为最小实际等待，没有
核对 UI 调用链及 10 次采样的累积成本。单条等待表达式的 host 断言漏掉了该问题。

`0feba06` 撤回这处 ADC 改动，恢复原调度行为；`f09127f` 新增完整采样批次、
真实电池保护入口和 UI battery adapter 回归，并接入 CI。低电量阈值、校准、平均
算法和外部供电策略不变，RTC、计时和 EPD 补丁保留。本次没有刷机或推送。

代码和主机检查已通过。用户随后回复“通过 继续推进”，本响应性修复节点记为
用户确认复验通过；用户未附新日志，不补录延迟测量或逐场景 PASS。原第五批失败
候选的记录保留，其余硬件矩阵不由这次简短确认推导为全部通过。现继续
[第六批 codec 预算修复](RELIABILITY_BATCH_06.md)。

## 串口证据

用户日志：`logs/serial-COM7-261022-203903-382-10008.log`，55,243 B，SHA-256：
`782430798c080ac7fec241357489060957a74eb495f4a2f2734da62c7e021d53`。
日志开头被截断，没有完整固件 commit 字段，不能单凭日志确认其二进制身份。

按 revision 配对 `kind=button` 事件和 `display intent accepted`：

| 日志 | 有效配对 | 事件到提交中位数 / 最大值 | display owner 耗时中位数 |
|---|---:|---:|---:|
| 同日 19:29 的 COM7 日志 | 12 | 10 / 20 ms | 542.5 ms |
| 用户提供的 20:39 日志 | 18 | 400 / 1,200 ms | 580 ms |

具体例子：revision 7 在 59,519 ms 记录 button 事件，60,719 ms 才接收显示意图，
增加 1,200 ms 的提交前延迟；revision 21 为 1,000 ms，revision 22 为 800 ms。
多个其他事件为 200 或 400 ms。这里计时起点是 reducer 记录事件，不是物理按键
产生时刻；更早的输入 guard 阻塞不包含在这个数里。末尾未完成的显示不能推定失败。

同日较早日志不是同板同操作的严格 A/B；配置/镜像身份不完整，不能把全部时差
唯一归因于这一个改动。但生产调用链和反向批次测试独立证实了新增同步等待。
各日志 SHA、配对明细和统计留在 `/tmp/note4-ui-lag-analysis.json`。

另见启动时一次 sync-journal 写入耗时 7,261 ms，以及正常部分/全刷耗时；它们
不能解释后续按键路径反复出现的 200 ms 倍数新增延迟，本修复未宣称消除所有延迟。

## 代码根因

`ReadPowerStatus` 一批采样 10 次。旧 `vTaskDelay(pdMS_TO_TICKS(1))` 在默认
100 Hz 下为 0 tick；第五批的 `DelayAtLeastMs<1>()` 请求 2 tick。第一次等待
受 tick 相位影响约 10–20 ms，后续等待通常约 20 ms，每批累计约 190–200 ms，
还没计算 ADC 调用、互斥竞争和调度延后。

这些读取并不只发生在后台：

- `DeviceUiTask` 消费按钮时先调用 `CheckBatteryAfterUserActivity`，它经
  `IsBatteryVeryLow → GetBatteryVoltageMv → ReadPowerStatus` 同步采样。
- `BuildHomeSummary` 和 `UpdateSettingsDiagnostics` 通过 `ReadBatteryStatus`
  再次同步采样；这些入口出现在交互与画面准备路径，多个读取会累加。

因此一次局部“保证 1 ms”的改动，会给单轮 UI 增加数百毫秒甚至形成事件积压。
这是本次实现和验证方法的错误，不应要求用户接受这种响应性退化。

## 修复边界与测试修正

恢复 ADC 原有 yield，保留默认 100 Hz。没有加入忙等、缓存或新的后台任务，也
没有用旧电量跳过保护。原来 ADC 是否需要真实 1 ms 间隔仍需硬件依据；若需要，
应先把采集移出同步 UI 路径，再验证采样间隔、结果新鲜度和低电保护预算。

删除 wake 测试里单独执行 ADC 等待表达式、要求至少经过 1 ms 的断言；它检查了
局部时长，却鼓励了错误的批次行为。替代测试执行生产 `ReadPowerStatus`、电压
换算曲线、`GetBatteryVoltageMv`、`IsBatteryVeryLow`、`CheckBatteryAfterUserActivity`
以及 `ReadBatteryStatus`，包含真实公有数据头。静态核对 UI 的 guard、summary 和
diagnostics 调用点；没有声称执行完整 UI reducer 或实测实际按键延迟。

## 验证结果

| 检查 | 结果 |
|---|---|
| `test_power_sampling.py` | 100/1000 Hz 各 535 checks，ASan/UBSan 通过 |
| 原失败实现作为 `--source` | 编译成功；100 Hz 单批次模型为 200,000 us，被延时预算断言拒绝 |
| 完整采样和 UI guard | 三种 tick 相位、10 次采样、guard + 两次 adapter 的累积成本、平均结果 |
| 错误/保护路径 | ADC 未初始化、空 mutex、锁失败、部分/全部采样失败、无校准、低电触发及 USB/充电抑制，残留 /STDBY 不豁免保护 |
| 保留的 wake/EPD 回归 | 100 Hz：1,793 checks；1000 Hz：1,802 checks，ASan/UBSan 通过 |
| 计时回归 | 110 checks，ASan/UBSan 通过 |
| RTC/codec 回归 | 100/1000 Hz 各 1,524 checks，ASan/UBSan 通过 |
| `idf.py -B build-wsl reconfigure build` | EXIT=0，ESP-IDF 5.5.6，零编译警告，M8 / 编译后 UI 栈门通过 |
| CI YAML / Python AST / CRLF-aware diff / 文档链接 | 通过 |
| 远端 CI | 未执行 |
| 修复后设备复验 | 用户已确认响应性恢复，未附新日志；本地没有刷机或执行设备测量 |

ADC/校准端口分别注入 50/20 us 固定成本；模型中当前 100 Hz 单批为 700 us。
该数字只是检验人为等待是否被放大，不是 ESP32 ADC 性能实测；不能将其作为设备
恢复到 700 us 或 UI 已恢复的结论。1000 Hz 同时验证原调度行为及采样/错误语义。
所有测试保留 LeakSanitizer，未关闭 sanitizer。

日志为 `/tmp/note4-ui-lag-{power-final,power-before,wake,time,rtc}.log`。
临时文件可能被系统清理，用户原始串口日志没有修改。

## 候选与复验

构建基于干净提交 `f09127fe5c0d3a856266e9ce66b56be40e1a20f9`，烘焙标识
`f09127fe`，无 dirty 后缀。后续文档提交不改变运行时代码。

| 对象 | 大小 / SHA-256 |
|---|---|
| app bin | 3,643,840 B；`fd4a83a7bdd90278f78c155fbf4c83ebb42660ee67ef84578e1b40b5f6fe9250` |
| ELF | 41,723,672 B；`baa78218b05d9c938c99c5d03979144633bde1cdee29268ad10a149cd0db91a8` |
| sdkconfig | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

产物位于 `build-wsl/`，清单为 `/tmp/note4-ui-lag-identity.json`，构建日志为
`/tmp/note4-ui-lag-build.log`。保留 100 Hz 与现有诊断配置，未改变配置来制造速度差。

用本候选重复刚才卡顿的页面切换、上下选择和连续按键，特别是设置/诊断与 Agent
页面；同时核对 button event → display intent 的 revision 延迟是否回到较早日志
量级，区分提交前阻塞和正常面板刷新。记录候选标识和新日志，确认无 200 ms
批次成本反复叠加。用户已确认该响应性节点，后续候选仍继续运行完整 ADC/UI
延迟回归，避免重新引入相同阻塞。
