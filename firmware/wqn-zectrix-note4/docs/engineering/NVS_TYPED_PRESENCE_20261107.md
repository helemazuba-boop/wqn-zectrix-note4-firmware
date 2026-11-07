# NVS typed NOT_FOUND 的固件修正

实际 ESP-IDF v5.5.6 `Storage::findItem` 搜索 page 时只返回成功匹配，页级类型不匹配
最终可折叠成 `ESP_ERR_NVS_NOT_FOUND`。所以 typed get 返回 NOT_FOUND 不足以证明键
尚未设置；它也不能作为回退旧 `.cur`、默认 generation 或覆盖旧值的依据。

本批先对现有固件函数运行实际 SDK NVS/CRC：原有 76 项检查有 34 项失败，随后用
同一组输入核对修正；再补齐空值、异常字符串、读写准入、范围与故障回归。

## 新的读取边界

`runtime/nvs_typed_value.h` 使用 `nvs_find_key` 查询当前可见键的实际类型。只有该
查询返回 NOT_FOUND 才按现有未设置政策处理；类型不符返回 TYPE_MISMATCH，查询失败
原样返回。已确认存在之后，typed get 的 NOT_FOUND 仍然是错误，不重新改成缺失。
原有调用者的栈、owner 与事务约束继续适用，工具函数本身不建立冻结或跨调用原子性。

| 路径 | 修正后的行为 |
| --- | --- |
| 通用 u64/string/blob | 先验存在性和类型，再读取；不将错型数据清成默认值。字符串还需完整长度、唯一末尾 NUL；真正空 BLOB 仍支持 |
| Word 七模式暂停游标 | 错型/异常长度读取报错，不能许可旧 `.cur` 回退；保存前拒绝错型与异常旧长度；清除前拒绝错型 |
| scope generation、deck、marker | 错型字段和查询后消失均阻止正常恢复，不把它们当成 generation 0 或无 marker |
| 恢复态 NVS 观察 | 分别保留存在性、错误和原始长度，`read_complete` 不掩盖类型折叠；NVS 输出仍先进入内部栈 scratch |
| `wqn_boot` schema/reset-id/reset-intent | 错型进入恢复错误，不能被当成新设备或无 reset intent；未修改格式化、显式授权和 marker 最后清除顺序 |
| 音量 | 显式支持当前 i32 和已存在的 u8 格式，保留 0–100 夹取；真缺失沿用默认值，异常类型/读取失败返回错误；不更新播放缓存 |

字符串错误保留调用者原输出，避免嵌入 NUL 被截成合法前缀。通用 u64 只在成功后复制
内部临时值。Word 保存仍跳过完全相同的 blob；支持的新建、更新、清除可进入既有 SDK
路径，并非把所有写操作关掉。写入与业务清除的原有授权/owner 规则不由此检查替代。

## 证据范围

`test_nvs_typed_presence.py` 原样提取生产函数，链接未修改的 SDK Page、Storage、
PageManager、HashList、Item 与 ROM CRC。官方 generator 生成 v1/v2、多 namespace
样例。单线程 public-handle 分发/故障层是 host bridge，不冒充 SDK 全局锁、真实 flash
或 RTOS；媒体回调拒绝写入并计数，输入文件前后重新散列。

回归覆盖错误类型、查询失败、查询后消失、七种游标、正常与异常写入准入、schema /
reset-intent、u8/i32 音量、夹取、真正空 BLOB，以及 CRC/布局合法但不规范的 NVS 字符串。
ASan/UBSan/LeakSanitizer 在该非隔离的合成介质测试中启用。它与独立离线 NVS 工具的
seccomp child 不同；后者为隔离约束关闭 LeakSanitizer，不能混淆两组结论。

六个语义负对照分别隐藏错型、吞掉查询后的 NOT_FOUND、跳过游标写入保护、忽略启动
intent、丢弃 u8 支持及接受嵌入 NUL。必须完整执行并由目标断言失败，编译、异常或
sanitizer 错误均不算检出成功。综合门禁增加为 138 项：51 runtime、10 UI、77 controls。

旧 maintenance/scope/schema host ports 原先直接返回 TYPE_MISMATCH，无法覆盖 SDK 的
折叠行为。本批让其错误类型路径返回 SDK 所见的 typed NOT_FOUND，再通过真实存在性
查询取得类型；同时把原来注入在已不存在数据读取上的故障移到实际查询边界。实际 SDK
套件独立核验这套端口模型，不能仅因 mocks 通过就宣称 SDK 契约成立。

## 验收与剩余问题

这是运行时代码变更，需要新的、从镜像读回版本的验证候选；不可覆盖 `val-20261107.4`。
具体提交、构建、门禁日志和新候选身份记录在工作区 `doc/r08-nvs-type-guard-20261107/`。

类型查询只说明当前 SDK 视图，不证明历史/物理介质缺失，也不阻止 SDK 自己在初始化或
读取时执行恢复。这里没有引入原始 NVS 备份、解密、可信硬件来源或数据维修授权。
CRC/schema/SID/pack admission、受控候选安装/回滚仍有各自的验证与实施要求。

实机仍需验证：支持类型的正常启动/音量/Word 行为，类型错误进入恢复且原始记录可
保全，查询/读取故障的错误呈现，NVS 与 SPIFFS 交互、断电和栈水位。SDK 构建、主机
测试和新候选不是这些物理结果。R-08 仍未关闭，也不代表全部鲁棒性问题完成验收。

## 2026-11-08：实际 SDK 暂停调用链纳入验证线

开发线 `eccfd7d` 已回移共有类型保护及 `b9dccbe` 的暂停错误传播。验证线原本已包含
相同生产修正；本批同步更强的调用链回归，未改变运行时代码或固定 `.5` 镜像。
严格 scope/恢复观察和 I-02/I-04 仍有依赖未完整回移，不能称两条线完全同步。

`test_nvs_pause_pipeline.py` 与同名 C++ fixture 同时执行实际 SDK NVS/CRC、生产游标
读取、`ReadSessionCursorPaused` 和完整 `LoadSessionTransaction`。复用验证线现有
typed-presence bridge 与 SDK builder，无需移入仅供开发线使用的整套核心夹具。
快照/健康 journal 和 scope 状态是显式 ports，SDK 全局锁、介质写入成功和物理调度
不在结论内。源 NVS 及 legacy 字节前后核对；ASan/UBSan/LeakSanitizer 启用。

七模式各覆盖错型、短记录、CRC、暂停/未暂停、真缺失、legacy、旧 SID、查询后消失、
查询失败，以及验证线特有的 scope 未就绪，共 154 检查。未就绪必须在读取快照及
打开 NVS 前拒绝；不能为了移植夹具而删除生产 guard。三个语义负例分别吞掉加载
错误、吞掉暂停读取错误，以及只在实际加载函数内绕过 scope guard；异常或不完整
执行不能算检出。门禁清单扩展至 142（52 runtime、10 UI、80 controls）。

本次专项、三个负例、SDK 构建和继承证据核验记录在工作区
`doc/validation-pause-pipeline-20261108/`；旧 138 项需核对定义、源码与非空日志后才能
继承。未将继承证据称为 142 项一次新跑，亦未生成新的实机包或作设备验收。
