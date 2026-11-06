# R-08：原始记录保全与维修流程（进行中）

R-08 尚未关闭。本地已增加离线原始保全、Word/Note 解码、设备认证回执对账与
审查计划与原生候选工具；后续还要完成真实介质身份核验、设备候选复验/交付、失败回滚和 HIL。
保全工具只复制原文；候选工具额外生成供离线审查的 journal/checkpoint 文件。
设备安装、数据库变更与维修授权仍未实现，源文件保持不变。

## 已确认的维修边界

完整长度 CRC 坏记录可能是 observation、ACK 或 suspend。即使坏字节看起来仍像一个
request-id，也不能把它当作可信身份。跳过坏 ACK 会复活已确认事件，跳过 suspend
会恢复被明确挂起的上传；跳过 observation 会破坏会话序号链。坏记录后的有效块可以
供维修调查，不能直接接续为可信的当前队列状态。

`ScanOutbox` 的现行候选优先级是 primary → recovery → backup。`.tmp` 可能是未发布
事务，始终原样保全，不能因为 CRC 全部有效就自动选择为恢复来源。空的有效主文件
也有实际语义，不能用非空旧备份擅自覆盖。

云端已有 session `next_sequence` 和 observation 行。现新增独立的只读回执入口，
本地实现与协议见下文；尚未部署或完成真实设备联调。web session 查询属于 web actor；
设备维修需要核对 user/device/domain 归属。服务端序号不能单独证明某条记录的接收情况，或恢复本地 next-position /
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
当前清单 83 项（37 运行时入口 / 10 UI / 36 负例），包含对账客户端、本地 checkpoint
材料校验、原生合并候选、维护 NVS/游标/pin 保护及十一个相关语义负例。

## 设备认证的只读回执（2026-11-06，本地实现）

云端新增 `POST /api/esp32/v3/study/observations/receipts`，协议独立镜像为
[`outbox-reconciliation-v1`](../../contracts/outbox-reconciliation-v1/README.md)。
现有 Word/Note schema、散列与正常上传代码未改动。

从设备 token 得到 user/device 后，按 domain/session 和最多 64 个原 request ID
查询规范 `study_observations`。数据库在同一 SELECT 快照中再次核对设备归属、账户
active 与 session 归属，过期/退休 session 的保留事实也可读取。仅 service_role 可调用；
STABLE 函数不写事实、投影、session、设备元数据或控制幂等表。

`found` 返回原规范事实；同 actor/device 的 request ID 被其他 session 使用时返回
`conflict`，不暴露其他会话内容；`not_found` 仅描述该快照未找到保留行，不证明从未
接收过。数据库故障、畸形结果、归属/顺序不符均失败，不能当成缺失。成功回执固定
`repair_authorized=false`，不会产生 ACK、skip、重放或维修指令。

现有 Note `loadEquivalentAppliedObservation` 按序号等字段补偿，不比较原 request ID
和 occurred_at，不能代替此逐请求证据。回执接口兼容 PostgreSQL/固件保留的 UUID
文本，包括 `00000000-0000-0000-0000-000000000001` 用户和历史非 RFC variant 会话。

## 保全包对账与审查计划（本地实现）

使用安装了 `scripts/robustness-requirements.txt` 的 Python 环境。
`scripts/outbox_reconcile.py` 提供以下入口，所有输出目录必须新建且位于输入包之外：

```sh
python scripts/outbox_reconcile.py collect \
  --source-bundle /tmp/wqn-word-evidence --source-manifest-sha256 <独立固定的原文摘要> \
  --endpoint https://wqn.helema.cn/api/esp32/v3/study/observations/receipts \
  --device-token-file /tmp/device-token.txt \
  --expected-user-id <用户UUID> --expected-device-id <设备UUID> \
  --output /tmp/wqn-receipt-evidence
python scripts/outbox_reconcile.py verify \
  --source-bundle /tmp/wqn-word-evidence --source-manifest-sha256 <原文摘要> \
  --evidence /tmp/wqn-receipt-evidence --evidence-manifest-sha256 <collect输出摘要>
python scripts/outbox_reconcile.py plan \
  --source-bundle /tmp/wqn-word-evidence --source-manifest-sha256 <原文摘要> \
  --evidence /tmp/wqn-receipt-evidence --evidence-manifest-sha256 <回执摘要> \
  --output /tmp/wqn-repair-review
python scripts/outbox_reconcile.py verify-plan \
  --source-bundle /tmp/wqn-word-evidence --source-manifest-sha256 <原文摘要> \
  --evidence /tmp/wqn-receipt-evidence --evidence-manifest-sha256 <回执摘要> \
  --plan /tmp/wqn-repair-review --plan-sha256 <plan输出摘要>
```

设备 token 是配对所得的 64 位小写十六进制凭证。token 只经私有 stdin 管道进入
独立网络进程，不在命令参数、输出或证据包中保存；反射原文或转义 token 的响应也拒绝。
默认只允许固定路径的 HTTPS，不跟随跳转；HTTP 只在显式
`--allow-insecure-loopback-test` 下接受字面 `127.0.0.1` / `::1`，其证据标记为测试用途。
网络子进程总期限覆盖 DNS/TLS/读取，默认每批 10 秒、网络查询总预算 300 秒，无自动重试。
本地原文验证与写同步不计入网络预算。
401、429、数据库/传输错误和畸形回执停止后续请求，未查询项继续登记为未知。
返回码 2 表示已保全带缺口的证据，不能当成完整对账。

工具重扫全部预算内原始记录，支持完整 7014 条事实，未受 manifest 的 256 条预览
限制。每批原请求/响应、文件/偏移/记录 SHA、本地 checkpoint 与独立固定的源
manifest 绑定。除了原 request ID，还比较 session、sequence、item、action、mode、
occurred_at；时区按精确微秒比较，非零超微秒精度暂列 unresolved_timestamp。
`next_sequence` 不用于补猜事实，ACK/suspend 不靠表面 ID 反推损坏角色。

原文 actor 归属目前仍是操作方声明，不等于已验证的设备导出身份。各批回执来自各自
快照，不构成全局原子快照；安装前必须维护态重新核验。离线验证重算原文索引、查询
覆盖与全部判断，拒绝自行改写的 assessment；独立固定的摘要保护保留字节，不是
可离线重新证明 TLS 来源的服务器签名。

计划列出原生候选顺序和具体 `acknowledge_after_checkpoint` 提案，仅用于已匹配、
未挂起且无本地字节版本冲突的 pending。已挂起/已 ACK 的事实保持原处置；未知坏块、
原文预算不足、部分尾部与冲突继续保全。有效空 primary 不被旧备份覆盖，tmp 不晋升。
每个提案绑定原记录 SHA/偏移和本地 checkpoint。计划始终
`installation_ready=false`、`repair_authorized=false`，明确列出尚未完成的设备身份、
原生 checkpoint、受控发布和显式维修授权前置条件。`verify-plan` 不能认可篡改的授权标记。

30 个对账专项场景使用真实原生记录/枚举和本地 HTTP 故障服务器，取消会回收使用凭证的
网络子进程；三个语义负例保护
完整 payload 比较、suspend 意图和派生报告重算。真实设备导出、线上认证对账、
受控发布与 HIL 仍未完成。

### 审查计划 v2：本地 checkpoint 材料

`plan` 现在通过 `outbox_checkpoint.py` 核对 Word v4 / Note v1 session、Word v1
descriptor 和 legacy cursor。原文 SHA、严格长度、SDK CRC 约定、枚举和计数均须
有效，再应用更严格的维修策略核对 UUID、pin、唯一候选和序号范围。session 仅在
primary 解码失败时考虑 backup；有效 primary 的 mode 不符不改选 backup，tmp 不晋升。
读取后的源 manifest 再次按独立固定摘要核验。

事实匹配但无可用 session、SID/mode 不符、item 不在当前窗口、Word ordinal 不能
由候选窗口表示、Note next-position 越界或本地 next-sequence 与事实序号矛盾时，
不再提出 ACK。已持久化更高序号的 session 不回退；Note opened 的 last-opened pin
仍独立列为 checkpoint 更新，不能由更高 next-sequence 代替。存在 descriptor 家族时
还要核对完整 SID、header/payload CRC、pins、start-index、scope-generation 及候选
子集；损坏或只有 tmp 的描述文件继续保留。

每个 `local_checkpoint_checks` 是单条事实相对保留 session 的结构检查，候选状态不是
可安装的合并 session 文件。跨全部 pending/ACK/suspend 的原生重放、候选序列化、
复验及发布仍受 `combined_native_candidate_not_validated` 阻止。新计划格式为
`wqn-outbox-repair-review-v2`；旧 v1 审查计划需以原始证据重新生成，不会静默升级旧
SHA 或授权标记。既有证据包和机器回执协议不变。

Word 暂停值当前优先位于 NVS，`.cur` 只是一次迁移后备，`.ctp/.cbk` 不自动晋升。
当前导出包没有 NVS 游标或已提交 deck-scope-generation，因此有效 `.cur` 也只作
legacy 证据；计划保持 `word_nvs_pause_not_exported` 等未验证前提。当前 pack pins
和 Word session 来源也尚未核验。不能把这些结构检查写成完整设备备份或安装许可。

18 个 checkpoint 场景使用生产 EncodeSession / DecodeSession / LoadSessionFile、
descriptor 解码、cursor 读取和 ReconcileSession 函数，与实际 SDK CRC 交叉对照；
原生函数由源码原样提取，ASan/UBSan 编译执行。测试中的 scope-generation 外部值
未被用来证明设备当前状态。两个语义负例分别放行窗口失配、把 legacy cursor 冒充
NVS 权威来源，要求由断言检出且无异常或 sanitizer 错误。

### Note 回放修复（2026-11-06）

原实现按 acknowledged → pending → suspended 三组逐次覆盖 last-opened pin。
这三个组是处置状态而不是时间顺序：较旧的挂起事实可能在最后覆盖较新的 pending，
而 next-sequence 已经推进到最新。`ReconcileSessionFromOutbox` 现在跨三个组按每项
最高 observation sequence 选一次 pin；cursor 推进、磁盘格式和持久化顺序不变。
不按 occurred-at 字符串排序，设备时钟回拨也按逻辑序号选择；已完成的 checkpoint
再次回放不会因中间旧 pin 请求重写。新增原生场景复现了修复前 00:00:03 被
00:00:02 覆盖的问题，并验证重复回放及回拨时钟。该纯内存选择逻辑没有新增堆分配。

### 原生合并候选（主机工具，不授权安装）

在审查计划生成后，可使用当前源码及实际 SDK CRC 编译并运行原生编解码、扫描与
完整回放。需要本地 `gcc`、`g++`、ESP-IDF（`IDF_PATH`；默认
`/home/unknow/esp/esp-idf-v5.5`）及前述 Python 依赖，不需要设备/提供商凭证。

```sh
python scripts/outbox_reconcile.py candidate \
  --source-bundle /tmp/wqn-word-evidence --source-manifest-sha256 <原文摘要> \
  --evidence /tmp/wqn-receipt-evidence --evidence-manifest-sha256 <回执摘要> \
  --plan /tmp/wqn-repair-review --plan-sha256 <审查计划摘要> \
  --output /tmp/wqn-native-candidate
python scripts/outbox_reconcile.py verify-candidate \
  --source-bundle /tmp/wqn-word-evidence --source-manifest-sha256 <原文摘要> \
  --evidence /tmp/wqn-receipt-evidence --evidence-manifest-sha256 <回执摘要> \
  --plan /tmp/wqn-repair-review --plan-sha256 <审查计划摘要> \
  --candidate /tmp/wqn-native-candidate --candidate-manifest-sha256 <候选摘要>
```

工具先重新计算计划并核对独立摘要，再把已核对的输入复制到私有临时目录。
原始导出路径不会进入原生写入入口。候选保留选中 journal 的完整原始字节前缀，只
追加原生编码器产生的 ACK 标记；不会 compact、删除坏块、恢复 suspend 或改写事实。
当前入口要求已有完整可解析源及可用提案，不能重构无法解释的坏 observation/ACK/suspend。

每个受提案影响的 session 都用同一份完整候选 journal，跨三种处置组进行原生回放；
核对 ACK 前后 checkpoint 字节等价、挂起处置/原因不变、ACK 覆盖及候选 session
原生重新加载。原 session 也必须精确 decode/encode 往返，不能悄悄归一化未解释字节。
输出包含 `journal.v1`、`checkpoint-XX.v1`、`proof.json` 和 manifest，绑定原文/回执/
计划摘要、生产源/头文件、生成单元、编译器与 SDK CRC/头文件摘要。候选包上限 8 MiB，
新增 ACK 后仍须满足原文配置的分析记录预算；不满足时保留未知，不能静默截断。

输出采用 0700/0600 权限与 fsync/incomplete 提交流程。原生编译/验证失败、来源改变
或提交未完成时不会发布成功包；编译器/sanitizer 诊断最多保留 16 KiB 于私有
`native-error.json`，保留 incomplete 标记。`verify-candidate` 会重新编译、生成及逐字节比较全部
输出，拒绝自造 SHA、授权标记、遗漏/增加文件和候选字节篡改。当前源码、编译器或
SDK 改变会使旧候选不能按当前规则通过复验，需要明确重新生成和审查。

11 个候选场景覆盖 Word/Note、跨模式 session、完整字节前缀、挂起保留、Note 最新
pin、未解释 session 字节、独立重算、原文变化、失败 incomplete 及 CLI。两个语义
负例分别信任篡改后的候选、跳过原 session 精确往返，要求断言检出且无运行异常。

原生验证完成只消除当前提案的合并候选检查缺口，不能证明运行设备使用了相同固件。
manifest 继续声明 `installation_ready=false`、`repair_authorized=false`，保留设备身份、
维护态重核验、NVS/scope/pack、受控发布和显式维修授权条件，并增加目标原生固件未
核验条件。它不是可直接交给设备执行的维修包。

### 只读 NVS 上下文与暂停读取保护（2026-11-06）

`InspectWordRecoveryNvsOnStorageOwner` 提供真实设备代码侧的有界只读采集原语。
调用者必须已在 StorageService 内部栈 owner 的事务中；不在 owner 时拒绝且不访问 NVS。
它只以 `NVS_READONLY` 打开 `wqn`，观测 7 个当前模式的游标原文、已提交的完整 u64
scope-generation、RAM generation 前后值，以及 scope-change marker 的存在性/长度。
不初始化 NVS、读凭证、迁移/清理字段，也不建立维护态冻结或跨文件快照。

每个游标分别保留 error、found、stored-bytes、copied-bytes 和最多 64 B 原文。53/64 B
等未知小布局原样保留，不当作有效暂停值；超过 64 B 明确失败且 copied-bytes=0，
不能把截断前缀冒充完整原文。类型/I/O/长度变化错误继续记录，并采集其余独立字段。
只有 error=ESP_OK 时 found=false 才证明缺失；read-complete 只证明这些已知字段读取
完成，不证明 CRC、运行时兼容、来源身份或维修许可。u64 generation 不截断为 u32，
也不把 RAM 缓存当成缺失 NVS 值的替代。API 未采集完整 NVS 分区或 pack pins。

NVS 输出先进入 owner 内部栈 scratch，再复制到调用者的 POD，即使输出位于 PSRAM
也不让 flash-backed NVS API 直接写它。49 项实际函数/主机端口检查覆盖缺失、未知
布局、超限、类型/读取错误、cache 漂移、只读/无凭证访问与原文保留；SDK 编译对象
的该函数入口帧为 720 B。此静态帧不证明完整调用栈或设备 HWM。

同时修复暂停读取：存在但长度不符的 NVS 值不再伪装成“没有值”而回退旧 `.cur`；
测量与数据读取后的长度都检查。读取错误、CRC/布局错误、legacy 短记录/尾随字节
和文件 I/O 错误通过独立 error 输出传给 `LoadSessionTransaction`，不再沿用旧快照
暂停值并返回恢复成功。真实缺失和上一 SID 的有效旧游标仍保持原有正常路径；有效
legacy 迁移写失败时继续使用已核对的 legacy 内存值，源文件保留。

现有 load 仍可能先执行原来的备份提升、checkpoint 或尾部修复，它不是取证导出 API。
新的只读 NVS 入口避免调用这些 load/migration 路径。pack prune 对任一 session
读取错误（除 NOT_FOUND）停止删除，不能把“pin 未知”当成“没有 pin”。故障注入已
复现旧实现误报恢复成功、以及错误下删除原词包；修复后分别通过 198 项回放、40 项
游标保护和 32518 项 publication/prune 检查（含 740 个单次 encoder 分配故障）。

四个新负例分别重新吞掉游标错误、把异常长度当缺失、截断原长度信息、使用 NVS
读写模式。这个入口尚未接到设备维护态导出通道；导出身份、冻结/传输、scope/pack
一致性及真机验收仍需实现或验证，既有候选的 NVS/身份安装前提保持未满足。

接下来按以下顺序推进完整维修流程：

1. 用已核验的设备导出身份和真实凭证联调现有对账工具；补齐设备来源证明及时间精度
   未解决项，不通过推进序号、skip observation 或创建新会话来伪装修复。
2. 已增加原生对照的 session/cursor/descriptor 结构检查及合并原生候选生成/复验；
   继续补齐可信 NVS/scope/pack 导出及设备维护态复验。未知 observation/ACK/suspend、
   缺失 cursor 与冲突证据保持未解决，不由主机候选验证授权写回。
3. 提供设备维护态的可靠导出/交付入口和 StorageService 事务，保留原始 bytes，
   先验证暂存候选，再按可重入的 intent 发布；重启、中断与失败均有明确回滚路径。
4. 在可恢复测试介质上执行设备、SPIFFS/NVS、断电和序号对账验收，最后关闭 R-08。
