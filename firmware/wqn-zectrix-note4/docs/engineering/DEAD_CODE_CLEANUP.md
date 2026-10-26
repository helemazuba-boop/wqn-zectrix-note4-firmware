# Note4 死代码清理核实与验收

2026-10-18；本地分批提交。没有推送、刷机或执行设备 HIL。

## 核实口径

原报告列出的函数展开后为 **72 个、对象 `.text` 合计 12,412 B**，
并非 69 个 / 11,706 B。其中 71 个位于链接 map 的 discarded sections，
`HtmlToPlainText` 所属 archive member 未被抽取。这些函数此前就不占用最终固件，
删源码不能把这 12,412 B 当成 Flash 收益。

临时探针则有实际成本：5 个 `__wrap_` 函数被链接参数强制保留，存储调用经过
捕获检查再转发。原报告所说的“每次调用任务句柄查询”不准确，owner 为空时会
短路退出。`StartStorageWriteBench` 也并非无 ELF 符号，原有 54 B 实现会写启动日志。

不能只按类型名或 ELF 独立符号判死：非 scoped enum 的成员可能仍在使用；
内联方法、host 测试 API、条件编译实现与第三方公共接口需要分别处理。

## 已处理范围

| 批次 | 本地提交 | 内容 |
|---|---|---|
| 1 | `bd5558f` | 删除临时 benchmark、NVS smoke probe、分区/SPIFFS GC probe 及 5 组 wrap/undefined 链接选项 |
| 2 | `af6db91` | 删除逐写计时、逐次 NVS 统计扫描、单词包索引/卡片读取的临时测量；提交摘要降到 DEBUG |
| 3 | `69038f0` | 删除主体、存储、时间、UI、显示、协议的无消费者接口及死声明；测试改用实际生产 API |
| 4 | `5202aba` | 删除旧音频播放器、独立 TX 工厂、旧 AI JSON 上传和 HTML 转文本路径；补齐关闭功能时的编译边界 |
| 5 | `450e2ca` | 删除未参与构建的重复 WebSocket 目录和两个孤立字体文件，清理构建注册和声明 |
| 6 | 本报告所在提交 | 区分 DEBUG 提交摘要与退役测量记录，保留历史日志严格校验并记录验收 |

原清单 72 项已逐项归类：**50 个函数实现/重载已删除，1 个缓存统计接口仅在
`WQN_HOST_TEST` 下编译，另外 21 个有理由保留**（下表）。同时删除继发无调用者，
包括 `CreateAudioTxChannel`、`PrepareNoteObservationOutboxForSleep`、旧传输完成代理、
若干时间/模型转换函数、冗余电池与 WiFi 查询接口。

所有 ACK 回归改用带维护门控的批量接口，保留失败、重放、配额、恢复测试。
scoped enum 删除死值时显式保留存活值原来的数值。UI cache 的设备内存上限仍在，
命中/构建统计只进入 host 版本；显示测试使用当前原生字体接口。

删除 5 个没有活跃消费者的 Kconfig 项：`WQN_AI_STREAMING_ENABLE`、
`WQN_AI_V1_FALLBACK`、`WQN_BATTERY_LOW_THRESHOLD_MV`、
`WQN_BATTERY_OTA_THRESHOLD_PCT`、`WQN_CHARGING_DEEP_SLEEP_EXTRA_MS`。
当前 Std/Pro 实现本来就使用流式协议，上述 AI 开关并不能切换旧路径。
活动的极低电压关机策略仍使用原有实现。

配置矩阵还暴露并修复了原有构建缺口：Agent 关闭分支缺少问题回复、离开和中断
三个桩函数；离线分支引用了 WiFi 条件块内的 token helper；无屏配置缺少 EPD
cleanup 配置默认值；Flash v1 编译了未受门控的 v2 音频辅助；UI/深睡眠关闭时
产生未使用告警。token helper 的实现只调整编译可见范围；逐项对照确认 17 个
相关既有函数体未变。深睡眠辅助定义跟随开关，UI 可选入口显式标记保留。没有修改唤醒、睡眠准入、EPD 波形或存储格式/恢复协议。

## 经核实保留

| 项目 | 理由 |
|---|---|
| `RunPairingFlowIfNeeded`、Flash v1 条件分支 | 有实际兼容实现；验证离线/Flash v1 配置，不能凭当前 sdkconfig 推断永久关闭 |
| `CpuPerformanceLease` 移动赋值 | 资源拥有类型的移动语义接口；不以当前调用数量裁剪类型契约 |
| 15 个 WebSocket、4 个 QR 公共函数 | 第三方组件 API，已被链接 GC 丢弃；保持库接口和维护边界 |
| `FullSyncReason`、`SleepDiagnosticFlag`、`SleepPrepState`、`SlotState` | 枚举成员仍参与同步、诊断及状态机 |
| `HomeStatus`、`kDefaultInstructions` | 前者同时声明了活跃的 `home` 字段，后者仍用于 v2 session.update |
| `SourceHanSansSC_Regular_slim.c` | 最终 ELF 中存活，显示和量字路径使用 |
| ConnectivityDemand / AiHistory 内联方法、`DrawHistoryClear` | 有存活调用路径，缺少独立符号不等于死代码 |
| 有意保留的 `[[maybe_unused]]` 辅助和按钮诊断 API | 可选功能/诊断用途，未以词法计数一刀切删除 |

实际构建使用的是根部 `components/esp_websocket_client/`。它已包含 PING 长度
125 B 校验、PONG payload 上限和 `esp_websocket_client_set_tcp_nodelay`；被删除的
`main/components/` 副本较旧，无需搬运补丁。实际 WebSocket/QR 组件内容未改动。

## 性能与日志边界

删除探针使分区读写、擦除、SPIFFS GC 直接走原始 SDK API；删除临时计时和 NVS
统计扫描，减少存储和卡片读取路径的附加工作。逐段检查保留真实 fopen/fwrite、
fflush/fsync、rename、NVS get/set/commit 的次序，以及索引扫描的 watchdog yield。
必要的截止时间、CPU lease、游标比较写入去重与失败恢复仍在。

第一批本地重链比较：镜像从 3,629,760 B 到 3,627,552 B（减少 2,208 B），
`.dram0.data + .dram0.bss` 减少 424 B。ELF 文件包括调试信息，文件大小不当作
Flash 收益。后续整体大小还叠加了同工作区独立的 Note UI 提交，不归因于本次清理。
**没有测量设备上的延迟改善或功耗。**

历史 HIL parser 和 fixtures 保留。新 DEBUG 摘要使用 `word observation committed:`、
`word observation batch committed:`、`word ACK batch committed:`，不带测量字段。
缺少退役测量场景时，性能判据显示 SKIP；SKIP 不表示性能通过。旧测量行缺字段、
错误时间窗和错误事务归属仍判 FAIL。保留的 RAM 接收/批次入队日志只支持相应
采样边界和触发原因，不能证明持久化耗时或设备掉电可靠性。

## 验证

| 验证 | 结果 |
|---|---|
| 默认 ESP-IDF / WSL 构建 | PASS，0 warnings，M8 / 编译后 UI 栈检查 PASS |
| Agent off / headless offline / Flash v1 | 3/3 PASS，0 warnings；核对实际生成配置，不以传参代替生效证明 |
| Python host 回归 | 34 个脚本全部通过，包含新增 Agent disabled 链接回归 |
| 配网浏览器逻辑 / Agent 策略 | 2/2 PASS（离线 host） |
| M8 规则 fixtures | 11 PASS / 0 FAIL |
| HIL parser selftest | 357 PASS / 0 FAIL，新增 4 项退役摘要兼容检查 |
| HIL smoke 日志 fixture | 12 PASS / 0 FAIL / 43 SKIP，5 个 CI 钉住的判据均 PASS |
| 最终对象 / ELF / Ninja 复核 | 106 个项目对象；72 项处置计数吻合；旧 probe/wrap/源注册消失，活字体和 TCP_NODELAY 保留 |
| 源码复扫 | 原报告 27 个死常量消失；死类型/枚举值消失；当前 Kconfig 每项均有 C/C++ 消费者 |
| CRLF-aware diff --check | PASS |

Agent disabled 的新增测试直接引用生产头文件的 22 个公开入口，并链接实际
关闭分支，避免只检查若干已知函数而漏掉链接缺口。

默认构建使用当前 WSL checkout 的 `build-wsl`。并行 UI 工作期间，后续代码批次
先导出独立暂存树到 `/tmp/wqn-cleanup-verify`，使用其已有 `build-wsl` 验证，避免
混入同时进行的 Note UI 工作。最后在实际工作区重新构建整合后的代码。配置矩阵
在该临时树中逐项重配置，不改变设备配置。矩阵定义如下：

| 配置 | 相对默认值 |
|---|---|
| Agent off | `WQN_AGENT_ENABLE=n` |
| Headless offline | WiFi STA、provision、control v3、AI、Agent、EPD UI 均为 n |
| Flash v1 | `WQN_FLASH_PROTOCOL_V2=n` |

可复核的主要命令：

```sh
source <esp-idf-5.5>/export.sh
idf.py -B build-wsl build
for test in scripts/test_*.py; do python3 "$test" || exit; done
node scripts/test_wifi_provision.js
sh scripts/run_agent_round_policy_test.sh
python3 scripts/hil_check.py --selftest
python3 scripts/hil_check.py scripts/testdata/hil_smoke.txt
cmake -DQN_REPO_DIR="$PWD" -DQN_BUILD_DIR=/tmp/note4-m8-fixtures \
  -P cmake/tests/run_gate_fixture.cmake
git -c core.whitespace=cr-at-eol diff --check
```

原始核实及逐项符号结果保存在本次会话的 `/tmp/wqn-note4-dead-code-verification-20261018.md`、
`/tmp/wqn-dead-code-reported-functions-20261018.json`；构建、测试、配置与最终符号复核
的本机输出见 `/tmp/wqn-cleanup-*`。这些临时文件不进入发布包。

本报告的 PASS 仅表示所列本地构建/host/日志 fixture 验证；实际 e-paper、RTOS、
存储设备时延、掉电、音频/网络和功耗仍需设备 HIL。尚未执行发布、远端 CI 或生产验收。
