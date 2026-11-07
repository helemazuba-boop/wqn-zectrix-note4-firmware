# R-08 未挂载时的主 Flash 原始证据

本批提供实际生产 C++ 采集 API、离线主机接收和独立复验核心。尚未接入设备菜单、
实体串口/USB 命令、交付 ACK 或可信设备认证，不能声称设备已能导出，也不是完整维修工具。
已冻结 `val-20261107.2` 包保持原提交与字节，本批不覆盖它。

## 读取范围与前提

`CaptureRawFlashEvidenceOnOwner()` 在同一个 StorageService owner 事务内运行，要求本次
启动已进入恢复维护态，没有活动临时文件或 pack 流 lease，以及调用方明确请求处理可能
包含凭据的主 Flash。该布尔值仅防止意外调用；未来服务层仍须验证新鲜设备确认，不能把它当物理存在证明。

此路径不调用 NVS、文件系统、挂载、初始化、修复、解密、写入或擦除 API。它使用
`esp_flash_get_size()` 和 `esp_flash_get_physical_size()` 核对 SDK 可读容量与检测物理容量，
相同、非零且不超过 64 MiB 才开始读取。测试包含 1 字节及非整块尾部；项目板为 16 MiB。
容量不一致时明确拒绝，不能把可读前缀冒充整个芯片，也不修改驱动容量来绕过限制。

读取使用 `esp_flash_read(nullptr, ...)`，目的缓冲位于内部栈且四字节对齐，每块最多 384 字节。
ESP-IDF v5.5.6 明确规定该 API 在加密区域返回原始密文；不会通过 mmap/cache 自动解密。
主 Flash 的 NVS、分区表、两份应用、storage 和未分配区域均处于同一读取范围，不只挑选可解析的文件。
原始图像可能包含凭据，不输出到现有诊断日志，也没有安装任何实时触发入口。

第一遍读取逐块发送并计算图像摘要，第二遍完整复读并比较 SHA-256，再复核容量和加密标志。
StorageService 的串行事务与启动维护态不等于全芯片硬件冻结；相同复读仍不能证明原子快照，
也不能证明介质在启动 NVS 检查前未被 SDK 修改。eFuse、加密主密钥、RTC/外部介质不在范围内。

## WQR1 wire

所有数值按小端逐字段编码，无 C 结构体 padding。帧头为 `<4sBHI>`：magic `WQR1`、
kind、payload 长度、连续 u32 sequence。长度不超过 512，sequence 从 0 开始。
它与已有逻辑文件/NVS 的 `WQE1` 格式分离。

| kind | payload | 语义 |
| --- | --- | --- |
| 1 | `<16s6s32siIIIBBB>`，73 字节 | capture ID、MAC、ELF SHA、启动错误、generation、配置/物理容量、加密标志、敏感范围请求、raw-byte 标志 |
| 3 | `<I>` offset + 1～384 bytes | 必须连续，从地址 0 开始的原始 Flash 字节 |
| 5 | `<IiII32s32s32sB>`，113 字节 | 已发送字节数、源错误、失败 offset、已复读字节数、两次图像摘要、此前所有帧 transcript 摘要、复读一致声明 |

失败 offset 为 `UINT32_MAX` 时表示无特定读取失败地址，例如整体摘要或容量状态变化。
第一次读取中途失败仍可输出明确错误的终帧和原始前缀；第二遍失败或变化不能宣告图像完整。
取消、过期或 sink 失败可能没有终帧；主机保留 `.incomplete`、已收字节和 wire，不提升为完整材料。
期限在读取/帧/让出 CPU 之间协作检查，不能抢占已阻塞的 SDK 或 sink 调用。
未来实体传输必须自行限制写入等待，提供逐帧校验、背压、取消和交付 ACK。

终帧的源复读声明和 host 图像完整性不代表设备操作已获交付确认。
例如 sink 实际送出终帧后返回错误，host 可拥有完整且自洽的图像，但设备 API 仍报告交付失败。
主机清单固定 `transport_delivery_acknowledged=false`，不由图像摘要推断 ACK。

## 仅处理已保存 wire 的主机入口

```sh
python3 scripts/raw_flash_evidence_receive.py receive \
  --input /tmp/raw-wire.bin --output /tmp/raw-bundle \
  --capture-id <32hex> --mac <12hex> --firmware-elf-sha256 <64hex> \
  --sensitive-main-flash
python3 scripts/raw_flash_evidence_receive.py verify \
  --bundle /tmp/raw-bundle --expected-manifest-sha256 <receive输出摘要> \
  --sensitive-main-flash
```

`--serial-log` 可读取已保存的 `WQN_EVIDENCE <base64>` 行，仍按 WQR1 magic 严格验收。
工具不打开设备，不自动提取凭据，不把主 Flash 当作可直接安装候选。
输出目录必须新建，权限 0700，wire、图像和清单均为 0600；拒绝 symlink/FIFO、过大输入、
身份 pin 不匹配、错序、非连续地址、摘要错误、额外帧和不一致完成声明。

独立 verifier 从 wire 重建清单、逐块比较全部图像及 wire 字节，并复核外部清单 pin。
重新 pin 伪造的授权字段、把不完整源改为完整、篡改图像/协议或不确定落盘结果均不能通过。
源错误的有效终帧可封存“不完整证据”，保留 `.incomplete`；CLI 对这类结果仍退出 1，不能误作成功备份。
无论完整与否，`complete_device_backup`、`atomic_snapshot_proven`、`identity_authenticated`、
`installation_ready` 和 `repair_authorized` 均为 false。

## 本地证据与剩余工作

新增专项包含 37 个 SDK 运行情形和共 203 项断言，包括 16 MiB 全范围、加密标志、
预置条件、敏感范围、容量不一致、两遍读错误/变化、取消、慢调用、迟到 sink 错误、
私有落盘、wire/清单重算及不确定最终 fsync。测试全部为 SDK 端口和合成字节，不来自设备。
三个语义负对照分别移除敏感范围准入、容量匹配和复读一致检查，必须完整运行并出现目标断言失败。

SDK v5.5.6 ai-local 构建、M8/profile/UI 门禁完成。实际编译参数另测得采集入口静态帧 1936 B、
Writer::Send 静态帧 560 B；这不是总调用栈或运行时 HWM。原逻辑采集 49 项回归及其 mode 负对照
必须保留，确保共享启动维护态发布没有改变已有逻辑证据协议。

本分支综合清单为 108 项（44 runtime / 10 UI / 54 controls）；实际运行结果和提交记录在工作区
`doc/r08-raw-flash-20261107/`。当前合成示例不是设备备份，也不授权真实介质读取或维修。

下一批必须接入真实设备确认与有界实体传输，保持逻辑和原始路径的范围区分；随后完成可信来源、
源证据绑定的云端对账、受控安装/回滚及可恢复设备上的取消、断线、重启、断电和栈水位 HIL。
