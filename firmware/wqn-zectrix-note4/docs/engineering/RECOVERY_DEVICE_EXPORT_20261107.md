# R-08 恢复模式：设备导出与电脑落盘确认

本轮把已实现的原始证据采集器接入启动存储恢复界面。它是保全工具，
不安装候选、不修改业务存储、不授权维修，也不能证明可信硬件来源。
主机故障测试、SDK 构建与实际硬件验收分别记录；本轮 HIL 待做。

## 入口与范围

启动存储检查失败、进入“存储恢复”界面后，选择“保全数据到电脑”。
可选“学习记录与上下文”或“完整主 Flash 原始镜像”。警告必须呈现，
再以新的确认键按下/松开事件持续至少 2 秒，才能提交任务。
上键取消；正在导出时也能取消，但必须等对应任务的终态，不能提早释放邮箱。

任务在唯一 StorageService 的内部 RAM 栈上运行。传输窗口在内部堆分配，
不放到 UI、协调任务或存储任务栈上。先验证启动恢复模式、owner、期限和取消，
再保留 USB 通道；收到电脑绑定本次随机 capture_id 的 READY 后才调用采集器。
因此仅打开接收工具、仅进入警告页、没有 READY 或被提前取消均不能读取源介质。

逻辑证据只含既定文件/NVS 白名单；原始镜像包含主 Flash 全范围，可能包含
WiFi 与配对信息。原始镜像需要设备警告及电脑 `--sensitive-main-flash` 双重确认。
原有大小匹配、逐字节导出、全范围重读、摘要和保留不完整状态的规则继续适用。
外部 Flash、PSRAM、eFuse、原子快照与启动前快照均不在证明范围内。

目前入口仅适用于启动恢复页。正常启动后遇到 SEQUENCE_GAP 的设备，仍需后续
增加不写业务介质的维护启动入口，不能直接在活跃同步期间调用冻结采集器。
损坏分区的原始字节解释、可信来源、受控安装/回滚仍是 R-08 后续工作。

## 电脑接收

当前工具运行于 WSL/Ubuntu，通过 Windows PowerShell 使用既有 NativePort 后端。
先退出占用设备 USB 串口的监视器。以下命令中的 MAC 是无冒号的 12 位十六进制；
ELF 摘要来自同一镜像的 baseline.json。输出目录必须不存在。

```sh
python3 scripts/capture_recovery_evidence.py \
  --windows-port COM7 --kind raw --sensitive-main-flash \
  --mac <12位MAC> --firmware-elf-sha256 <64位ELF摘要> \
  --output /可信私有目录/本次导出
```

选择逻辑导出时使用 `--kind logical`。本工具不会刷机或重启设备。
Windows 原生 Python 的目录 fsync/权限语义尚未验证，不作为支持的接收环境。
COM5 是官方固件检查点；开发设备使用实际确认的端口（当前开发约定为 COM7）。
不要通过通用 SerialPort/pyserial 替换后端，以免打开时的 DTR/RTS 中间状态重置设备。
可按 Ctrl+C 停止；保留输出、`.transport-incomplete` 与任何部分证据。

输出为 0700 新目录，文件 0600。`evidence-<capture_id>` 内的规范证据可用原有
raw_flash_evidence_receive.py 或 storage_evidence_receive.py 独立校验；外层
delivery 日志不修改规范清单，避免把传输确认混同于来源真实性。

退出码：0 表示完整源证据及最终交付确认；1 表示失败或源证据不完整；
2 表示电脑已保存并独立校验，但设备是否收到最终 ACK 未确认；130 为用户取消。
退出 2 仍应保留证据，用清单摘要校验，不能当成数据丢失后覆盖重试。

## WQNT1 通道

所有字段为 ASCII，以 LF 结束，capture 为 16 字节随机数的 32 位小写十六进制。
MAC、ELF 为上下文 pin，不能替代认证。

| 类型 | 字段（`WQNT1` 后） | 语义 |
| --- | --- | --- |
| Offer | `O capture mac elf R或L` | 设备声明本次身份/范围 |
| Ready | `R capture` | 电脑目录已建且同步到磁盘 |
| Data | `D capture seq8 len4 sha256 base64` | 内层 WQR1/WQE1 原帧，序号与内层一致 |
| ACK | `A capture seq8 window_sha256` | 最近一组帧已 fsync，最多 16 帧 |
| Commit | `C capture seq8 window_sha256 manifest_sha256` | 完成清单落盘及独立重放校验 |
| Finish | `F capture manifest_sha256` | 设备已收到精确匹配的最终 C |
| Cancel | `Q capture` | 电脑停止本次等待/传输 |
| Error | `E capture error8` | 提交确认前的源或传输错误 |

每帧不超过 523 字节；每行不超过 1024 字节。窗口最多重试 3 次；
普通 ACK 等待 2 秒、最终 C 30 秒、单行写入 2 秒、READY 60 秒，
全部受绝对 30 分钟期限与设备取消约束。SDK 每次读写最多 128 字节、等待不超过
50 ms。循环按时间片让出，不按每个 USB 小块固定 sleep，避免大镜像被人为降速。
固件保留窗口以重发；电脑缓存最近 16 帧摘要，相同重发不重复喂给证据解析器。
丢失/损坏帧和暂时超前帧不推进 next 或 ACK，最多允许 256 个丢弃帧。
其他 capture、已持久化序号的不同内容、内外身份不一致立即拒绝。
总输入预算为 512 MiB，接收队列最多 64 行，主机输入队列最多 8 行。

`C` 必须在数据/目录 fsync、规范清单提交、独立重放校验及外层 delivery 日志
同步后发出。最终 C/F 丢失只影响交付确定性：规范证据与传输状态分别保存。
规范清单中的 `transport_delivery_acknowledged`、`identity_authenticated`、
`repair_authorized`、`installation_ready` 不因这些 ACK 被改成 true。

## USB 所有权与故障边界

本板主控制台为 UART0，USB Serial/JTAG 是次控制台。首次预留后，
`components/recovery_usb` 将 ESP_LOG 路由到已有主 UART stdio，关闭 ROM 的次通道，
再独占 USB 有界驱动；不重复初始化 UART。仅此组件可直接调用 USB SDK I/O，
M8 仍限制其他文件，且 stdio 写例外仅精确覆盖该组件的 `/dev/uart/0..2`。
预留后 USB 日志保持关闭，重启恢复。失败重试复用主 UART FILE，不泄漏文件句柄。
驱动保留到重启，避免任务迁移后卸载 ISR 违反 IDF 同 CPU 约束。

panic 的直接 ROM/USB 输出、物理拔插、USB 控制线时序、Flash 加密与真实读出、
满容量耗时、主机磁盘故障、设备取消延迟和实际栈/内部堆余量仍需 HIL。
协议校验发现混入或损坏时保留不完整证据；不声称 panic 时也能完整导出。
主机 fsync 表示操作系统已接受持久化请求，不替底层磁盘提供额外断电保证。

## 本地验证入口

```sh
python3 scripts/test_evidence_transfer.py
python3 scripts/test_storage_recovery.py --tick-hz 100
python3 scripts/test_storage_recovery.py --tick-hz 1000
python3 scripts/test_recovery_serial_proxy.py
pwsh -NoProfile -NonInteractive -File scripts/test_monitor_serial.ps1
python3 scripts/run_robustness_gate.py
```

传输测试编译真实 export worker、EvidenceTransfer、raw collector 与 boot-mode 模块，
用进程管道替代 USB SDK，故障注入覆盖部分/零/错误写、缺失/错误 READY、错误/丢失 ACK、
损坏/丢失数据、最终 C 重试、F 丢失、源错误、OOM、owner/模式拒绝及持久化失败。
逻辑成功路径重放由真实逻辑采集器生成的流；该 worker 测试中的逻辑采集端是端口，
完整逻辑源采集覆盖仍由 test_storage_evidence_capture.py 提供，不能合并为 HIL 结论。
`trust-ack` / `skip-ready` 负对照必须因目标语义断言失败，编译失败不算检测成功。
