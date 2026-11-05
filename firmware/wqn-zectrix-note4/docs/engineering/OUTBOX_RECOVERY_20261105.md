# R-08：原始记录保全与维修流程（进行中）

R-08 尚未关闭。本批增加离线原始文件保全、当前 Word/Note 日志解码和证据包验证；
后续还要完成按 actor/session/request 对账、明确维修计划、设备端受控交付及失败回滚。
保全工具不会修改设备、数据库或源文件，也不会生成、安装或授权“修复后”日志。

## 已确认的维修边界

完整长度 CRC 坏记录可能是 observation、ACK 或 suspend。即使坏字节看起来仍像一个
request-id，也不能把它当作可信身份。跳过坏 ACK 会复活已确认事件，跳过 suspend
会恢复被明确挂起的上传；跳过 observation 会破坏会话序号链。坏记录后的有效块可以
供维修调查，不能直接接续为可信的当前队列状态。

`ScanOutbox` 的现行候选优先级是 primary → recovery → backup。`.tmp` 可能是未发布
事务，始终原样保全，不能因为 CRC 全部有效就自动选择为恢复来源。空的有效主文件
也有实际语义，不能用非空旧备份擅自覆盖。

云端已有 session `next_sequence` 和 observation 行，但这些不是完整的本地持久化
回执接口。web session 查询属于 web actor；设备维修需要核对 user/device/domain
归属。服务端序号也不能单独证明某条记录的接收情况，或恢复本地 next-position /
next-phase / suspend 原因。证据不足时保留并停止，不能推测出“无损”修复结果。

## 离线保全入口

在 Linux/WSL 本地文件系统上，对已停止写入的导出目录执行：

```sh
python3 scripts/outbox_forensics.py capture --domain word \
  --source-dir /tmp/note4-offline-export --output /tmp/wqn-word-evidence --offline
python3 scripts/outbox_forensics.py verify --bundle /tmp/wqn-word-evidence \
  --expected-manifest-sha256 <capture输出的摘要>
```

Note 使用 `--domain note`。输入是已经从测试介质/离线镜像中导出的平铺文件；本批
没有设备串口导出、SPIFFS 镜像解包或在线抓取功能。`--offline` 是操作方对导出状态
的声明，不能证明跨文件原子快照。读取期间检查每个源文件的 inode、长度和修改标记，
不把活动目录当作一致快照。发现后目录库存或已复制源文件变化也拒绝提交成功包，
但这些检查仍不能证明跨文件原子快照。实际介质故障试验仍须先保存完整镜像。

工具按当前源码中的 49 个 Word / 10 个 Note 固定文件名保全日志、临时/恢复/备份副本、
session、cursor 和 rejected journal，记录缺失项。Word 另外发现并保全
`wl<24hex>.v1/.tmp/.bak` 本地会话描述文件；任意一个副本出现时也列出该族缺失的
其他副本。异常或未来 `wl*` 名称作为不解析的原始证据保留，不据此还原 SID 或选择
恢复来源。目录、符号链接、FIFO、路径分隔符及超出 4096 个已发现描述文件的清单
使保全失败并保留 incomplete 状态；不会静默丢弃多余文件。可选 `--flash-image <本地镜像>`
额外原样复制镜像；该镜像的设备归属、分区范围、完整性和可恢复性仍未自动验证。
manifest 始终声明 `complete_device_backup=false`，不能把这些文件当作完整设备备份。

新建目录使用 0700，文件使用 0600；不复用已有目录、不覆盖源文件、不接受符号链接
或 FIFO。每份原文流式复制、SHA256、flush/fsync 后才提交 manifest 并清除 incomplete
标记。提交前复制/写入/同步报错或源文件改变时，保留不完整输出，不发布成功回执。
移除标记后的最终目录同步返回错误会报告不确定，并尽力恢复本地 incomplete 标记；
不能保证错误前未提交或再次 I/O 失败后的掉电持久性。现有源文件不变。私有目录及
完整性依赖本地文件系统和操作系统访问控制。

## 检查内容与证据强度

Word v1 记录是 200 B，Note v1 是 206 B；工具核对当前 magic、version、CRC、字段
终止、kind 和 observation 枚举，并模拟实际 reader 的前缀计数。CRC 对齐
`esp_rom_crc32_le(UINT32_MAX, ...) ^ UINT32_MAX`；`123456789` 的结果是 `0x2dfd2d88`，
与默认 zlib CRC 约定不同。SDK Linux CRC 实现作为独立的 C oracle 编译验证。

损坏完整记录只给出偏移、长度、摘要和错误类型，不把其中的 kind/request-id 当真。
有效 CRC 但含不透明文本、嵌入非零 padding、不可用 UUID 或超出 JSON 安全整数范围
的记录也不获得身份查找资格。v2 对可用 observation 另保留本地 `next_position`、
Word `next_phase` / Note `next_sequence`，独立于云端事实字段；回执和云端序号
不能替代这些续学证据。CRC 坏块不解释这些字段。未知版本/类型、冲突重复记录及容量错误继续阻止
完整重放。尾部短记录与错误后的有效块保留在原文和诊断中，不自动删除或接纳。

新包使用 `wqn-outbox-forensics-v2`，固定声明存储配置 `word8192-note1000-v1`：
Word 活跃/挂起身份合计上限为 8192，Note 仍为 1000，不把 Word 扩容套用到 Note。
`verify` 继续接受旧 v1 包，以旧 1000/1000 上限和 8192 块预算重新计算分析，
不会用新规则使旧报告失效。配置未知、与格式不匹配、或用新分析冒充旧报告均拒绝。
这些标识固定了解读规则，不证明设备实际上运行了哪份固件。

v2 每文件最多分析 16448 个块、展示前 256 个块详情；所有原始字节均完整复制。
预算覆盖 8192 个挂起观察及其 8192 个 suspend 标记，而不只覆盖观察条数。
超出预算明确标记 `analysis_complete=false`，不能给出完整扫描的成功结论。
manifest 独立记录原文摘要、字节数、候选顺序、前缀计数、坏块统计和分析完整性。

`verify` 重新计算每份原文 SHA256，并从保全字节重新运行日志分析；拒绝不完整标记、
缺失/多余文件、篡改报告、重复 JSON 键、不支持的格式或文件条目中的未知字段，
不接受嵌在单文件条目中的额外维修声明。未提供独立固定的 manifest
摘要时只能证明 self-consistent，不能证明来源真实性。无论哪种验证成功，都保持
`repair_authorized=false`。

## 当前验证和后续实现

`test_outbox_forensics.py` 使用当前生产编码器、固定布局、扫描器和实际 SDK CRC C
实现生成 38 组原生样例，并核对 Python 解码、错误码、域容量和计数。额外使用真实
描述文件路径派生器验证大小写 UUID 的文件名。19 个主机场景另覆盖
原文/副本/session 保全、权限、已存在目录、坏 observation/ACK/suspend、opaque
identity、未来版本、源文件变化、读取/写入/fsync 失败、不完整包、报告/字节篡改、
符号链接/FIFO、分析预算与大文件完整复制；新增 Word 8192/8193、Note 1000/1001、
16384 块挂起日志、描述文件族与异常名称、库存变化及 v1/v2 配置兼容检查。
它们不是物理 Flash/HIL 证明。

四个语义负例分别改用错误 CRC 参数、信任未重算的报告、将新 Word 容量降回 1000、
忽略动态描述文件；要求完成测试且只有行为
断言失败，不能把编译错误、异常、sanitizer 或超时当作成功检出。工具接入统一门禁，
当前清单 65 项（30 运行时入口 / 10 UI / 25 负例）。

接下来按以下顺序推进完整维修流程：

1. 提供受现有设备认证和 actor/domain/session 归属约束的只读逐记录回执对账；不通过
   推进序号、skip observation 或创建新会话来伪装修复。
2. 将 manifest 摘要、原始记录身份、已核对的回执和可恢复字节绑定到显式维修计划；
   对未知 observation/ACK/suspend、缺失 cursor 与冲突证据明确保留未解决状态。
3. 提供设备维护态的可靠导出/交付入口和 StorageService 事务，保留原始 bytes，
   先验证暂存候选，再按可重入的 intent 发布；重启、中断与失败均有明确回滚路径。
4. 在可恢复测试介质上执行设备、SPIFFS/NVS、断电和序号对账验收，最后关闭 R-08。
