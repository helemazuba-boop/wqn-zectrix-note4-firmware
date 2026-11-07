# 持续开发线 NVS 类型修正回移

本批从开发基线 `a49c33a` 独立回移验证线 `c1d14fd` 的共有读取修正。
验证候选 `val-20261107.5` 的源码/镜像与旧清单保持原样，不能把本批开发构建当作该候选。

## 修正范围

实际 ESP-IDF 的 typed lookup 可把页级 TYPE_MISMATCH 折叠为 NOT_FOUND。
`runtime/nvs_typed_value.h` 先查询当前可见键类型；仅查询本身 NOT_FOUND 可沿用未设置
政策，错型、查询错误或查询后读取失败必须保留错误。该查询不提供跨调用原子性，
也不证明历史或原始介质缺失；原有存储 owner、栈和写入授权约束继续适用。

以下七个函数与验证线的相应函数逐字一致：通用 string/u64/blob 读取，Word 游标
保存/读取/清除，以及音量读取。启动 schema/reset-id/reset-intent 读取及支持头也与
验证线一致。游标异常长度不再许可旧 `.cur` 回退或直接覆盖；字符串要求完整长度、
唯一末尾 NUL，真正空 BLOB 仍可读取。音量显式支持 i32/u8，保留夹取范围且不更新播放缓存。
正常新建/更新/清除仍可进入 SDK 路径，未将所有写操作关闭。

调用链复核又确认了必要依赖：旧 `ReadSessionCursorPaused` 把 NVS 错误压成 false，
旧 `LoadSessionTransaction` 会继续使用快照的未暂停值。仅回移底层会改变旧游标回退
行为却没有阻止错误恢复。本批同步回移实际暂停读取的错误输出与会话加载的错误传播；
任何已知读取错误都不能宣告成功加载。真缺失仍保留快照值，合法的未暂停 NVS 值仍可
覆盖旧暂停快照，合法 legacy 迁移失败仍保留已验证的内存暂停值和源文件。

开发线的 default-deck/marker 仍调用通用 `LoadStringFromNvs`，因此同样得到类型和
规范字符串保护。开发线没有 `LoadDeckScopeTextLocked` 或 owner 恢复观察接口，
没有为回移而添加空实现或在测试中默默跳过它们。严格 scope 恢复、ready 保护、
generation 上界及无效 marker 保全仍属于尚未完整回移的 R-08 依赖，不能因此关闭 R-08。

## 自动证据

`test_nvs_storage_core.py` 固定提取实际开发函数，覆盖 14 组 SDK 生成/构造的介质，
共 96 检查；scope 文本通过开发线实际使用的通用读取验证。它与验证线 101 项专项的
差别是不存在的 owner 恢复观察路径，不能混用两组数量或声明覆盖这些接口。

`nvs_sdk_fixture.py` 编译未修改的 SDK Page/Storage/PageManager/HashList/Item 与 ROM
CRC，固定源文件、host stubs 和对象摘要；public-handle 分发层是单线程 fixture，
不模拟全局 SDK 锁或 RTOS。媒体写入回调拒绝并计数；真实创建 namespace、Flash
写入成功、commit 持久性及断电恢复不在该测试证据范围内。ASan/UBSan/LeakSanitizer 启用。

回移前 13 个子进程完整执行 94 检查，出现 43 个行为失败；最后一组合法 SDK 布局但
零长度的字符串使旧 `std::string(nullptr)` 抛异常，整套基线未完成，不能计为负对照成功。
回移后 14 组全部完成、96/96 通过。六个语义负对照必须完整执行并因目标断言失败，
编译/异常/sanitizer 报错不算检出。开发门禁由 79 项扩至 89 项：37 runtime、10 UI、42 controls。
既有 schema ports 同步 SDK typed NOT_FOUND 语义，同时保留启动保全与显式重置故障回归。

`test_nvs_pause_pipeline.py` 在同一原生程序内串联实际 SDK、生产 NVS 游标读取、
`ReadSessionCursorPaused` 与 `LoadSessionTransaction`，其余 snapshot/journal ports
明确模拟健康介质。七种模式各覆盖错型、短记录、CRC 错误、暂停/未暂停、真缺失、
legacy 迁移、旧 SID、查询后消失与查询失败，共 140 检查。仅底层已修正而加载层仍旧时
为 140 检查 / 35 失败；补齐依赖后 140/140。另有吞掉读取错误和吞掉加载错误两项
语义负对照，不能用不同层各自通过的 mocks 冒充这条实际 SDK 调用链。

初始 86 项门禁在发现依赖后按已核实 PID/进程组主动停止，日志原样保留；后续完整
89 项另用新目录执行，不能把初始中断的部分结果拼成一次完整验证。

具体提交、SDK 构建、完整门禁和配置检查结果记录在工作区
`doc/nvs-type-backport-20261107/`。原开发 sdkconfig 含开发菜单，匹配 profile 为
`ai-local-dev-menu`；首次使用 `ai-local` 被检查正确拦下，未为通过而关闭配置检查。
验证候选 `.5` 的 `ai-local` /开发菜单关闭不受影响。

## 后续

本批是本地开发 checkpoint，未执行 push、刷机、部署、迁移或提供商请求。
设备实际 NVS、错误恢复、音量/Word、断电与栈水位仍待 HIL 和独立验收。
验证线已有的恢复观察/导出、严格 scope 组合、I-02/I-04、离线 NVS 维修计划衔接及
受控候选安装/回滚仍有各自的依赖与验收要求；本批不代表两条开发线完全同步。
