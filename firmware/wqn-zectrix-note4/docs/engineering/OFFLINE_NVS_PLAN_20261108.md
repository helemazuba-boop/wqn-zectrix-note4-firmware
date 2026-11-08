# R-08：将离线 NVS 证据接入计划与原生候选

`outbox_reconcile.py` 的 `plan`、`verify-plan`、`candidate` 和 `verify-candidate`
现在可以消费同一原始镜像的 [NVS 补充包](OFFLINE_NVS_CONTEXT_20261107.md)。
它们只生成和复验主机文件，不连接设备、提供商或数据库；`installation_ready`、
`repair_authorized` 与来源身份认证仍为 false。R-08 尚未关闭。

## 固定输入

先完成原始镜像导入、NVS capture 与回执证据收集。建立一个普通 JSON 文件：

```json
{
  "format": "wqn-repair-nvs-inputs-v1",
  "source_bundle": "/private/raw-evidence",
  "source_manifest_sha256": "RAW_SHA256",
  "profile": "/private/profile.json",
  "profile_sha256": "PROFILE_SHA256",
  "spiffs_build": "/private/spiffs-reader",
  "spiffs_build_manifest_sha256": "SPIFFS_BUILD_SHA256",
  "import_bundle": "/private/raw-import",
  "import_manifest_sha256": "IMPORT_SHA256",
  "nvs_build": "/private/nvs-reader",
  "nvs_build_manifest_sha256": "NVS_BUILD_SHA256",
  "context_bundle": "/private/nvs-context",
  "context_manifest_sha256": "CONTEXT_SHA256"
}
```

以上摘要占位符必须替换为各步骤记录的真实 64 位十六进制摘要，再计算此 JSON
文件的 SHA-256。键集合严格固定；相对路径以该文件所在目录为基准。所有敏感镜像
与导出目录使用私有权限，不将它们或设备凭据提交到 Git。

描述文件的 `source_bundle` 是 WQR1 原始导出包。下面 CLI 的 `--source-bundle`
则是导入生成的 **Word forensic 子包**，两者不能互换。NVS 上下文绑定的 forensic
pin 必须与这次计划的输入完全相同，不能拼接另一设备或另一轮导出的会话。

使用 Linux/WSL、固定的 robustness Python 依赖环境、ESP-IDF 5.5.6 及 Linux C++
工具链；SDK reader/profile 必须对应导出镜像的布局与 ELF。每个命令都重新验证
固定输入、只读重放原始镜像，并在完成时再核对输入；派生 JSON 自洽不构成证据。

## 四个入口使用同一组参数

以下 Bash 示例中的摘要均需替换为操作者独立记录的值。`REVIEW_ARGS` 不包含密钥。
输出目录必须尚不存在，且位于原始包、SPIFFS reader、import 包、NVS reader、context
包五个输入目录之外，也不能落入既有 forensic、回执或计划输入目录。

```sh
REVIEW_ARGS=(
  --source-bundle /private/raw-import/forensics-word
  --source-manifest-sha256 FORENSIC_SHA256
  --evidence /private/receipts --evidence-manifest-sha256 RECEIPTS_SHA256
  --nvs-context-inputs /private/nvs-inputs.json
  --nvs-context-inputs-sha256 INPUTS_SHA256
  --sensitive-main-flash
)
python3 scripts/outbox_reconcile.py plan "${REVIEW_ARGS[@]}" --output /private/new-plan
python3 scripts/outbox_reconcile.py verify-plan "${REVIEW_ARGS[@]}" \
  --plan /private/new-plan --plan-sha256 PLAN_SHA256
python3 scripts/outbox_reconcile.py candidate "${REVIEW_ARGS[@]}" \
  --plan /private/new-plan --plan-sha256 PLAN_SHA256 --output /private/new-candidate
python3 scripts/outbox_reconcile.py verify-candidate "${REVIEW_ARGS[@]}" \
  --plan /private/new-plan --plan-sha256 PLAN_SHA256 \
  --candidate /private/new-candidate --candidate-manifest-sha256 CANDIDATE_SHA256
```

后续命令遗漏上下文参数会因计划不一致而拒绝，不能静默退回旧计划。原生阶段失败
或输入在运行期间变化时，输出保留 `.incomplete`，不发布成功 manifest。

## 暂停与 scope 的解释

| 固定镜像中的证据 | 计划与候选行为 |
| --- | --- |
| 有效 NVS 游标，mode/SID 匹配 | 在 journal 重放后覆盖 paused；false 与 true 同样有效 |
| 有效 NVS 游标，SID 已过期 | 不覆盖重放后的状态，也不回退 legacy 游标 |
| NVS 真实缺失，legacy 有效且 SID 匹配 | 重放后应用 legacy 暂停值 |
| 两者真实缺失或 legacy SID 已过期 | 保留重放后的快照状态 |
| 类型、长度、CRC、未知 legacy 格式或读取结果不明确 | 保留原文，暂停提案 |
| scope generation 不同、完整 u64 超出会话 u32 范围或 marker 未解决 | 暂停提案，不截断、不假定 RAM scope ready |

原生工具先验证原会话的精确字节往返，再分别重放原 journal 和追加 ACK 后的
journal，最后覆盖暂停值并复验候选字节。终止重放会清除顺序和随机学习的 active/
paused，因此覆盖顺序不能提前。本批同时修正 Python 计划遗漏随机学习终止状态的
问题，与固件 `ReconcileSession` 的 mode 0/5 行为一致；旧的终止随机计划需重新生成。

未传这些参数时沿用 v2 计划与 v1 候选格式，Note 行为保持兼容，以上随机终止修正
除外。带上下文的计划为 v3，候选为 v2，记录整条输入 pin 和解析器源码摘要。
工具升级也会改变原生构建指纹，不能将旧候选证据冒充新工具的复验结果。

## 验证与剩余工作

`test_outbox_nvs_plan.py` 使用官方 NVS generator、只读 SDK reader、生产 raw collector、
SPIFFS 导入及原生 codec/replay，覆盖七种模式、真假暂停、legacy 优先级、scope、
原文绑定、派生包篡改、输入隔离、运行中变化、四个 CLI 和终止顺序。介质和回执
服务都是合成夹具，未访问真实设备或真实认证回执。

统一门禁新增两个运行时入口和七个语义负对照，清单为 151 项。每个新增变异只运行
针对该变异的一项场景；必须观察断言失败且零执行错误，编译错误、异常和超时均不
计作成功检测。具体运行结果以该轮独立证据报告为准。

这批改动只涉及主机工具、测试和文档，不改变设备运行时代码或配置。固定的
`val-20261107.5` 实机包与离线工具包保持原样；当前工具需从相应新提交运行。
来源硬件身份、实时 scope/pack admission、认证联调、受控安装、回滚、断电及物理
NVS/SPIFFS/USB HIL 仍需单独完成。
