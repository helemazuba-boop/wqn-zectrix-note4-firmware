# Note4 可靠性第一批实施记录

日期：2026-10-21；分支：`fix/firmware-reliability`。
路线与问题定义见 [FIRMWARE_RELIABILITY_PLAN.md](FIRMWARE_RELIABILITY_PLAN.md)。

## 范围与状态

| 分组 | 状态 | 内容 |
|---|---|---|
| 文档基线 | 已本地提交 `6dcbd8a` | 12 个问题、B0–B5 路线与证据边界 |
| B0 工具链与配置基线 | 已本地提交 `0e81504` | CI 对齐 IDF 5.5.6；干净导出树从 defaults 构建 |
| B1 音频时间单位 | 代码/host/build 通过，设备待测 | 修复 7 处调用及服务 API，补实际边界回归 |
| B2 启动存储恢复 | 待实施 | 先执行真实 schema 决策；行为补丁须另有决策与 HIL |

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
