# Note4 可靠性第一批实施记录

实施日期：2026-10-21 至 2026-10-22；分支：`fix/firmware-reliability`。
路线与问题定义见 [FIRMWARE_RELIABILITY_PLAN.md](FIRMWARE_RELIABILITY_PLAN.md)。

## 范围与状态

| 分组 | 状态 | 内容 |
|---|---|---|
| 文档基线 | 已本地提交 `6dcbd8a` | 12 个问题、B0–B5 路线与证据边界 |
| B0 工具链与配置基线 | 已本地提交 `0e81504` | CI 对齐 IDF 5.5.6；干净导出树从 defaults 构建 |
| B1 音频时间单位 | 已本地提交 `2cf5e97`，设备待测 | 修复 7 处调用及服务 API，补实际边界回归 |
| B2 启动存储恢复 | 已本地提交 `6a314bb`，行为未改 | 表征当前数据处置与中断重入；保全策略和 HIL 待后续批次 |

本记录中的 host/build 结果不代表设备 HIL、音质/停止延迟、实际功耗或远端 CI。

## B0：同版 SDK 与干净配置构建

本地 ESP-IDF 为 `v5.5.6`，revision
`880a203ae603d08a94d06aa01e0d6761958e5e9e`。`dependencies.lock` 已是 5.5.6，
本批没有手改锁文件。CI 原容器为 5.5.4，改为 `espressif/idf:v5.5.6`，并在构建
日志打印 `idf.py --version`。

已通过 `docker manifest inspect espressif/idf:v5.5.6` 核验官方 tag：
Linux amd64 manifest digest 为
`sha256:851f136a2c09cf17c13e952b9975606d7d1050be1480b1fd82c7ea4bce78cfb3`。
此处只核验 registry 元数据，没有声称已运行该容器或远端 Actions。
沙箱阻止 registry 网络访问，核验在获准的沙箱外只读执行。

干净配置基线从提交 `6dcbd8a` 用 `git archive` 导出到独立临时树；不复制原本地
`sdkconfig` 或 build。使用该树自己的 `build-wsl` 执行：

```sh
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl set-target esp32s3 build
```

原本地构建与干净 defaults 构建均 EXIT=0、0 warnings，M8 和编译后 UI 栈检查通过。
干净导出没有 `.git`，构建中的 Git 身份回退为 unknown；下面的 SHA 不能用来
推断配置差异的性能影响。

| 项目 | 原本地有效配置 | 干净 defaults 配置 |
|---|---|---|
| ESP-IDF / target | 5.5.6 / esp32s3 | 5.5.6 / esp32s3 |
| `FREERTOS_HZ` | 100 | 100 |
| 普通分配 internal 阈值 / internal reserve | 0 / 98304 B | 0 / 98304 B |
| WiFi STA / Agent / EPD UI | y / y / y | y / y / y |
| deep sleep / PM | y / y | y / y |
| RAM diagnostics / sample interval | y / 30 s | y / 30 s |
| RAM history | y，24 h | n；不是完全关闭诊断 |
| app 大小 | 3,638,784 B | 3,636,784 B |
| app SHA-256 | `92b1418640584f686b39e3ed5d9a0e1c80f882b614c7dff30cb0897d66f5c320` | `8aae4f26898b5beb947715c81c69f1aa09bb9a9a62de061eb01a1c89e8e0c07c` |

该对比表仅列审查过的非敏感配置。没有导出设备 token、WiFi 凭据或整个本地配置。
新建配置已保留关键 PM/功能/PSRAM 策略；历史记录开关的差异需要在后续候选身份
中明确，不自动改成本机历史开关，也不当作关闭诊断的功耗对照。

本机证据：`/tmp/note4-batch01-baseline-build.log`、
`/tmp/note4-batch01-fresh-build.log`、`/tmp/note4-batch01-idf-manifest.json`。
临时目录可能清理；上面的身份、差异和结论保存在此跟踪文档中。

## B1：I2S 超时毫秒契约

`ReadAudioChannel` / `WriteAudioChannel` 改为显式 `uint32_t timeout_ms`，与 IDF
新 I2S API 一致。STD/Agent 共用采集的两个调用、Flash 的四个读写调用和音频自测
的一个调用直接传入毫秒。保持原来标称的 100/200/250/1000 ms 预算，消除 100 Hz
下提前换成 tick 的错误；不改变底层资源 owner、DMA 布局或录音停止状态机。

注释明确这是驱动各等待点的预算，不能推算整个调用或停止响应一定低于 200 ms。
设备 PCM 连续性、调度抖动和停止响应仍按原音频验收矩阵补测。

新增 `scripts/test_audio_channel_timeout.py`：编译生产服务函数及真实公开头文件，
提取全部调用点的最后一个实参表达式，在 100/1000 Hz 下检查驱动实际收到的单位。
driver 与 capability 调度端口是 host 替身；额外验证错误码、部分完成字节数及
调用期间 operation 的持有/释放，不将其扩大为真实 DriverOperation 并发或 PCM 验证。
新增 CI job 在 PR 等既有 workflow 触发条件下运行此回归。

| 验证 | 结果 |
|---|---|
| 未修复导出基线运行新回归 | 预期 FAIL，100 Hz 的 `driver_timeout_ms == expected_ms` 断言捕获错误 |
| 修复后新回归 | 100/1000 Hz 各 1,222 检查 PASS，ASan/UBSan |
| 调用端重新换成 tick 的变体 | 预期 FAIL，100 Hz 捕获 |
| 驱动边界再次换成 tick 的变体 | 预期 FAIL，100 Hz 捕获 |
| 既有 audio runtime / RX DMA / recording indicator | 74 / 98 / 67 检查 PASS，ASan/UBSan |
| Agent disabled | 22 个公开入口链接及拒绝/no-op 行为 PASS |
| 当前默认 IDF 构建 | EXIT=0、0 warnings，M8 与 UI 栈检查 PASS |
| 音频设备 HIL | 未执行，未刷机 |

ASan/LeakSanitizer 测试在沙箱 ptrace 下出现环境错误；以上最终结果来自获准的
沙箱外主机测试，未接触设备。原始代码的失败断言也在导出基线上重新运行确认。
本机记录为 `/tmp/note4-batch01-audio-*.log`。

REL-002 的 6 个短延时调用位于 ADC、wake assembly 和 EPD 路径。本批只定位并
保留它们，不统一改成一个 tick；其硬件/睡眠影响需逐调用点判断和相应 HIL。

## B2：真实 schema 实现回归与策略决策边界

新增 `scripts/test_storage_schema.py` 直接编译并链接生产
`main/runtime/storage_schema.cpp` 和真实公开头文件。NVS、SPIFFS 和 SleepLease
为故障可控的 host 端口；不再用 `EnsureStorageSchema() { return {}; }` 代替待测决策。
原有 startup/journal 回归继续保留，两个测试负责不同层。

这次运行原有 startup 回归还发现 fixture 漏掉了生产启动新增的
`InitMemoryDiagnostics()` 端口，导致它无法编译。本批补齐无行为替身，恢复原来的
启动门测试（`dfc1b78`）；没有改变生产启动顺序或诊断初始化。

| 验证 | 结果与边界 |
|---|---|
| 真实 schema 回归 | ASan/UBSan，39 场景 / 1,229 检查 PASS |
| 跳过 current-schema 检查的变体 | 预期 FAIL；新设备/旧 generation 不能被误判为 Ready |
| 忽略重置失败的变体 | 预期 FAIL；失败不能发布 RestartRequired 成功结果 |
| 开启挂载失败自动格式化的变体 | 预期 FAIL；校验挂载必须保持非破坏策略 |
| 既有 sync boot recovery | 三配置共 27 PASS / 0 FAIL，UBSan；校验 journal/启动顺序 |
| 既有 startup 两种故障变体 | 延后 journal 门、忽略服务启动失败均被原断言拒绝 |
| M8 gate fixtures | 11 PASS / 0 FAIL |
| NOR 部分写入/擦除、真实断电及设备并发 | 未验证；host 状态重入不等同于真实掉电恢复 |

39 个场景覆盖当前有效 schema、新设备、旧/未来/零 generation、reset_id 缺失或
长度异常、marker 读取故障、NVS 两类特殊错误、挂载/容量查询/卸载故障、重置各
步骤失败、部分已完成步骤后的重试、主动恢复出厂设置及 storage lease 拒绝/释放。
测试模拟 staging/commit 的可见性，不证明 NVS 或 SPIFFS 自身的介质原子性。

### 已观察的当前决策

| 条件 | 真实实现当前结果 | 本批处理 |
|---|---|---|
| 当前 generation、16 B reset_id、校验成功 | Ready；不 erase/format/commit | 保留此保护回归 |
| 缺 namespace/marker、旧/未来/零 generation、缺/短 reset_id | erase NVS、format SPIFFS、提交 marker 后请求重启 | 明确记录会清除模拟离线记录，不将其称为保全 PASS |
| NVS NO_FREE_PAGES / NEW_VERSION_FOUND、marker 读取错误 | 同样进入全量重置 | 作为 REL-003 的策略复审输入 |
| 当前 schema 挂载/info/unmount 失败 | RecoveryRequired；不进行全量重置 | 验证错误原样返回与业务门控边界 |
| 重置过程失败 | RecoveryRequired；已完成的操作不会自动回滚 | 记录具体破坏进度及再次启动决策 |
| 恢复出厂 invalidation 的 commit 失败 | 返回失败；模拟持久 generation 保持原值 | 保留主动重置提交失败回归 |

### 下一行为批次的决策要求

当前重置顺序是先擦 NVS，再格式化 SPIFFS，最后提交 schema marker。失败后再次
启动能凭 marker 缺失重做，是现有中断重入机制的一部分。因此只把“marker 缺失”
改成永久恢复错误，会同时改变新设备初始化和授权重置/迁移被打断后的恢复行为。

下一批需先把以下情形区别开，再编写最小保全补丁：

1. 全新且明确可初始化的介质；
2. 明确授权的 factory reset 或旧 generation 迁移；
3. 未知原因造成 marker 缺失、不可读或与数据不一致；
4. 授权重置已擦 NVS、但 SPIFFS/最终 marker 尚未完成的中断状态；
5. 新于当前固件的 generation，避免回退固件把未知格式当作可擦旧数据。

应评审重置意图在上述中断阶段的保留方式、可重复步骤、错误返回和未上传记录
的处置，然后用新策略替换本测试中明确标注的 characterization 用例。第一批没有
修改 schema 决策、擦除/格式化顺序、generation 或文件格式，REL-003 未关闭。
存储行为补丁依 AGENTS.md §7 完成相应 HIL 后再提交；本测试提交不含该行为变更。

schema 与既有 startup 回归也已接入 CI。最终默认构建零警告、M8/UI 栈门通过；远端 CI、刷机和
设备 HIL 均未执行。本机记录为 `/tmp/note4-batch01-storage-schema.log`、
`/tmp/note4-batch01-schema-mutation-*.log`、`/tmp/note4-batch01-sync-boot*.log`、
`/tmp/note4-batch01-m8-fixtures.log` 和 `/tmp/note4-batch01-final-build.log`。

## 本批提交与最终构建身份

| 提交 | 内容 |
|---|---|
| `6dcbd8a` | 可靠性问题清单、路线与文档入口 |
| `0e81504` | CI 对齐 IDF 5.5.6；干净 defaults 构建与配置差异基线 |
| `2cf5e97` | I2S 毫秒契约、7 处调用修复、100/1000 Hz 实际 API 边界回归 |
| `dfc1b78` | 修复既有 startup/journal fixture 缺少诊断初始化端口的问题 |
| `6a314bb` | 真实 schema 故障表征、三个错误变体与 CI 接线 |

实现提交完成后，在干净的 `6a314bb` 上执行
`idf.py -B build-wsl reconfigure build`，修正旧配置留下的
`WQN_GIT_COMMIT=f9367716-dirty` 显示身份。重配置后的宏为 `6a314bb1`，没有 dirty
后缀；此后的验收记录更新属于文档提交。运行时代码与 `2cf5e97` 一致，随后两个
实现提交只改测试/CI。

| 字段 | 本次最终本地构建 |
|---|---|
| 源码提交 | `6a314bb`，配置时工作区干净 |
| SDK | IDF v5.5.6，revision `880a203ae603d08a94d06aa01e0d6761958e5e9e` |
| 配置 | 原本地配置；100 Hz，RAM diagnostics + 24 h history 开启 |
| app | `build-wsl/wqn-zectrix-note4.bin`，3,638,784 B |
| app SHA-256 | `de6c02791fc6fc8586c2c6f15d24bacab287d5f9719a94c905e09a4ddc65a8fe` |
| ELF SHA-256 | `19b8177fa63e5ddf6e75d467374df281b2c80e4741120ae4dc3dd0f3b9bab37b` |
| 构建门 | EXIT=0，0 warnings，M8 ownership / 编译后 UI 栈检查 PASS |
| 日志 | `/tmp/note4-batch01-identity-build.log` |

本批交付是 B0 本地工具链/配置基线、B1 音频代码修复，以及 B2 测试与决策输入。
REL-001 的设备效果、REL-002 硬件短延时、REL-003 保全行为及后续 B3–B5 仍按路线
继续验收或实施。没有执行推送、PR、刷机、设备 HIL 或远端 CI。
