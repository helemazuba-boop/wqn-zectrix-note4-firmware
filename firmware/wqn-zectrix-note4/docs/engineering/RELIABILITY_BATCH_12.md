# 第十二批：存储证据与内存诊断对照构建

日期：2026-10-23（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`d5ef83e`。

## 状态与交付

用户回复“通过，继续”，第十一批记为用户确认节点验收。新串口片段没有提供逐项
RTC 复位/故障注入结果，保留上一批的精确证据边界。本批继续 B5 / REL-008、REL-009，
交付可复核的采集入口、离线分析和诊断对照镜像；实机容量、尾延迟、电流和低电关机
测量待完成，不将工具和构建通过记为这些指标已经通过。

| 提交 | 内容 |
|---|---|
| `7ca9664` | 复用设置容量查询，输出查询时间范围；FAULT 导出附带内存诊断开关 |
| `cb673fc` | 存储日志分析、诊断构建对、配置/归因回归及 CI 入口 |
| `997f293` | 清除旧编译日期、排除过时 CMake 参数，核对应用描述符和配套 Flash 文件 |
| 本记录所在提交 | 第十一批用户验收、本批身份/验证、采集步骤和剩余设备节点 |

本轮沿用用户“允许在不经测试的情况下创建本地提交，在关键节点停下来验证”的
授权，按逻辑单元本地提交后集中构建。未推送、未创建 PR、未刷机，远端 CI 未执行。

## 运行时改动与开销

`ReadStorageCapacitySnapshot()` 保持原有的一次 SPIFFS 查询和一次 NVS 统计。
只为已有 SPIFFS 查询记录 `esp_timer` 起止时间，并输出：

```text
STORAGE:capacity query_start_us=... query_end_us=... total_bytes=... used_bytes=... result=ESP_OK
```

记录由既有设置快照刷新触发，没有新增定时器、worker 或每笔事务后的 SPIFFS 扫描。
失败的查询也输出 result，不能继续沿用旧容量作为新观测。查询可能与其他任务写入
重叠，因此字段描述观测时间范围，不能代表整个测试区间的精确占用率或峰值。
新增日志有格式化/日志锁/串口成本；本批不把这一点描述为零开销。

`FAULT:boot` 追加 `mem_diag=0/1`，启动和既有电源睡眠诊断导出都可读取。
该字段描述当前运行镜像的 **RAM 诊断总开关**。关闭它同时移除该 recorder 的周期/业务
heap 采样、失败 hook、诊断 worker 与 PSRAM 历史；RTC 数字故障摘要、原错误环、
开发者菜单、事务日志和日志级别仍在。因此这是 RAM 诊断整体开/关对照。

存储容量准入、GC/淘汰、事务生命周期、低电阈值/紧急保存预算、ADC 等待和 UI
分钟刷新均没有改动。

## 离线分析规则与已有日志

入口：`scripts/analyze_storage_log.py`。可同时输入多份日志，输出文本、JSON、CSV：

```sh
python3 scripts/analyze_storage_log.py logs/<本轮日志>.log \
  --json /tmp/storage-evidence.json --csv /tmp/storage-evidence.csv
```

- 每个文件与观测到的 reboot 段独立；冷启动 boot ID 可重复，不跨文件自动合并。
- 只有当前 `FAULT:boot` 为其后事务绑定 build/boot/开关；迟到身份头不追认先前片段，
  历史 `FAULT:record` 不能当当前身份。ROM/boot 标志、执行时钟回退、身份变化会切段。
  缺失所有重启线索时无法保证识别每一次复位，分段数量也不是设备重启精确计数。
- 校验必要字段、非负耗时和 esp_timer 起止/毫秒的一致性；截断、格式错误、重复行
  单列。前台优先会越过旧后台 request ID，不能按 request ID 下降误判重启。
- 输出总计与各 owner 的样本数、结果、queue/exec；NOT_FOUND 单列为正常空查询，
  其他错误数另记。timeout/abandoned/overrun/低栈是事件，不伪装成完整事务样本。
- queue_wait 包括入队准入/排队到开始执行；exec 结束于事务函数返回，未计完成日志、
  caller 唤醒、UI ACK 或面板呈现。低栈分支没有完整完成日志，串口丢行亦无法精确计数。
- 容量只列观测点；事务可关联此前最近观测行及年龄，不插值、不把它当事务瞬时占用。
  与事务执行起点重叠、失败或损坏的新查询会阻断旧容量关联。
- 分位数采用 nearest-rank，P50/P95/P99 至少 20/200/1000 项 **同 owner** 样本；
  这是报告门槛，不是统计置信保证或验收阈值。混合 owner 总计与未知镜像段抑制分位数；
  小样本只给 n/max。结果混合情况保留在结果计数中，不能忽略失败样本来宣称性能通过。

本轮扫描 10 月 22–23 日 11 份 COM7 日志，得到 287 条完整事务；全部缺当前
build/boot 身份与容量观测，未并入本候选容量曲线。最近片段
`serial-COM7-261023-122209-519-37236.log` 有 15 条完成记录，3 条正常 NOT_FOUND，
最大 exec 为 `save-sync-journal` 的 2612 ms。它仅提供慢事务线索。
复核报告：`/tmp/note4-b12-all-storage.json`，含每份输入 SHA256；不以文件名时间猜镜像。

## 对照构建与身份

从已加载 ESP-IDF 的固件目录执行：

```sh
python3 scripts/build_diagnostic_pair.py --output dist/<新的对照产物目录>
```

工具要求 tracked worktree 干净、原始 sdkconfig 开启 RAM 诊断、两个输出目录为新目录。
默认开启版沿用 `build-wsl`；关闭版使用 `build-b5-diag-off-20261023` 的独立 sdkconfig，
不覆盖默认配置。开启版先 fullclean，保留当前 API/WS/CMake build type 覆盖；两版使用
相同 SOURCE_DATE_EPOCH 与 WQN_BUILD_TIME，防止旧对象留下不同编译日期。

完整 sdkconfig 按 symbol 比较，只有 RAM 诊断开关及其 Kconfig 依赖项可不同；其他
变化立即拒绝。每版必须零 warning、M8 与编译后的 UI 栈门通过。随后校验实际 bin
中的应用描述符一致，以及 bootloader、partition-table、ota_data_initial 三者哈希相同。
默认 `sdkconfig` 和 `build-wsl` 最终保持诊断开启；两版 app/ELF/map、配套 Flash 文件、
私有 sdkconfig 和 SHA256 清单保存在本地 dist。sdkconfig 可含 WiFi 凭据，目录与
配置副本限制权限，不纳入 Git；工具仅打印差异 symbol 名，清单仅保留允许的诊断值。

首轮未用于交付：人工核对发现旧开启版对象保留了 10 月 20 日编译日期；关闭版被
零警告门拒绝，新目录收到过时 `WQN_DEBUG_PROBLEM_IDS` 空参数而产生一条 CMake warning。修正工具后全新重建，
只以下面这组最终产物作为本节点候选，保留首轮日志但不计入通过结果。

最终产物目录：`dist/b5-diagnostic-pair-20261023-clean/`；清单：`manifest.json`。
两版源提交 `997f293`，完整提交 `997f293d8ad725cd92fe9b441d119e2c17e9d398`，烘焙身份 `997f293d`；
SDK `v5.5.6`，应用编译时间 `Oct 23 2026 04:39:53` UTC，WQN_BUILD_TIME `2026-10-23 04:39` UTC。

| 项目 | RAM 诊断开启 | RAM 诊断关闭 |
|---|---|---|
| sdkconfig SHA256 | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` | `0d3492e21dda7471b3753544bf5fd85b29273d1dfaa6f62b3c24eb2ce15162cc` |
| app 大小 | 3650144 B | 3645664 B |
| app SHA256 | `1c15b6b6fa2247e11d461dfaaef61698718baf9c2948d58634f13b4274db2603` | `7325216412faa9e1d3d950241b3f19ef8f82095837f9bd51e0cafc0181097b3f` |
| ELF SHA256 | `9df950deac2ef879e18c5daf90199ed6590743a3bf8f3066650c9f36dc652517` | `dbaf2e30ed41ecafd7bbe143be91908f24aa6a6aaa2e4ebf9fb8c0f250667d52` |

ELF 符号检查确认关闭版没有 runtime DiagnosticTask、AllocationFailed、诊断 rings 或
PSRAM history 指针；两版仍包含 RecordFault 与 DumpFaultDiagnosticsToLog。符号摘录
保存在产物目录 `on-diagnostic-symbols.txt` / `off-diagnostic-symbols.txt`。

配置差异仅四项：DIAGNOSTICS y→n，SAMPLE_SECONDS=30、HISTORY_ENABLE=y、
HISTORY_HOURS=24 因依赖消失。heap tracing 仍 off；PM、深睡、联网、音频、UI、
开发者菜单及存储配置一致。镜像体积差异不等于运行时 RAM、CPU 或功耗差异。

## 本地验证

- `test_storage_evidence.py`：23 项通过，覆盖迟到/伪历史身份、重复冷启动 ID、
  优先队列越序、时钟回退、重复/截断记录、容量失效、分位数门槛、实际 CLI 导出、
  配置漂移/凭据不回显、过时缓存参数和二进制描述符拒绝。
- `test_fault_diagnostics.py`：RAM 诊断开/关各 5634 检查通过，ASan/UBSan/LSan；
  附带开关标记且保留既有 journal/隐私/并发/撕裂模拟覆盖，仍不等于硬件 RTC 保留。
- 最终两份 ESP-IDF 5.5.6 构建均零警告，M8 和编译后的 UI 栈门通过。
- CRLF-aware diff check 通过；本批未修改存储协议、ADC 或按键时序，未重复无关矩阵。

## 设备节点与下一步

2026-10-23 用户回复“通过，继续”，本批记为用户确认节点验收。新日志已取得
`d65fa8d1` / boot=1 / mem_diag=1、23 条身份头后事务与五个 27.145% 容量点；
精确边界、活动临时文件回收补丁和包含补丁的最新构建对见
[第十三批](RELIABILITY_BATCH_13.md)。本批 997f293d 产物保留为历史基线，
不能与新代码镜像混用后只归因于诊断开关；高占用、电流及低电测量仍单列。

本批停在两份候选已经可核对的设备测量节点。先做存储采集，再做供电对照；沿用已有
正常学习/连续录音证据，补本批的容量与电气测量，不要求重新累积相同功能轮数。

| 顺序 | 实机动作 | 必须保留的证据与判断 |
|---|---|---|
| 1 | 开启版正常使用、打开设置/存储详情；中途接入时先开电源睡眠诊断 | `FAULT:boot` 的 997f293d / boot / mem_diag=1、之后的容量查询、流畅度；若头缺失保留未绑定状态 |
| 2 | 用可重新下载的包/缓存形成实际低/中/高占用，分别采集日志 | 实测 total/used、每段 workload 与前后观测；旧/新包及备份并存、图片淘汰、空间拒绝后恢复；不预定“75% 就应通过” |
| 3 | 包更新中执行 Word/Note/Problem 正常学习、保存、outbox 追加/ACK/整理 | 各 owner 的 queue/exec/n/max/结果，前台 durable ACK 与 UI 响应单独记录，重启后核对数据；不清理未上传 outbox 作为腾空间手段 |
| 4 | 同板、同供电、相近容量与温度下，对 RAM 诊断开/关做相同空闲/负载动作 | 核对 app SHA 与 mem_diag 标记，记录外部仪器的平均/峰值电流和动作时段；区分 USB 在线与电池/USB 断开，日志导出不纳入空闲测量 |
| 5 | 在可控供电夹具上测低电 WiFi/音频/EPD 负载及紧急关机 | 稳态/瞬态电压、实际持久化完成与重启内容；3430 mV 和 2 秒预算仍是现有策略，不承诺所有未提交意图都能保存 |

两份镜像的分区/bootloader 相同，比较时使用保留 NVS/SPIFFS 的 app-only 刷写流程；
不要用带整片 erase 的入口轮换镜像，否则数据、容量和配对状态已经改变，不能作为
同条件对照。刷写导致的复位本身也不计入上一批软件重启保留验证。本轮未执行刷写。

若仅有少量事务、缺占用率，先交付原始计数/最大值与缺口；没有外部电流/电压测量则
REL-009 继续待测。待设备结果显示具体超时/失败/能量缺口后，再决定是否需要容量
水位、回收调度或紧急保存策略补丁；不凭工具输出调整这些参数。
