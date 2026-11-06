# R-21 鲁棒性工程门禁

本批保护已落地的故障处理性质，不改变运行时协议或恢复政策。冻结审计的历史
脚本计数保持原样；脚本没有被工作流直接引用，不自动代表没有间接覆盖。

## 同一入口用于本地与 CI

激活项目 ESP-IDF v5.5.6 环境，在固件目录执行：

```sh
python3 -m venv /tmp/wqn-robustness-gate-venv
/tmp/wqn-robustness-gate-venv/bin/python -m pip install -r scripts/robustness-requirements.txt
/tmp/wqn-robustness-gate-venv/bin/python scripts/run_robustness_gate.py --selftest
/tmp/wqn-robustness-gate-venv/bin/python scripts/run_robustness_gate.py --output-dir /tmp/wqn-robustness-gate-results
idf.py -B build-wsl -DWQN_BUILD_PROFILE=ai-local build
git -c core.whitespace=cr-at-eol diff --check
```

[`run_robustness_gate.py`](../../scripts/run_robustness_gate.py) 的 `--list` 输出命令与
故障模型映射，可用 `--group runtime|ui|controls` 运行受影响分组。完整门禁共
83 个执行项：37 个运行时回归、10 个 UI 检查（9 个功能脚本、1 个静态栈门禁自测）、
36 个选定的语义变异。R-21 初始清单为 44 项；R-04 复核、R-25 修复各增加
1 个运行时入口与 4 个负例；R-18 增加 schema、两种 tick 频率恢复链路和 3 个负例。
R-08 进行中的离线保全工具增加原生 codec/SDK CRC 检查和四个语义负例，
涵盖新旧容量配置、动态描述文件、CRC 与报告重算；不代表 R-08 已关闭。
其后增加原文绑定对账/审查计划入口与三个负例，保护完整 payload、suspend 和派生报告。
本地 checkpoint 材料入口另增加两个负例，保护候选窗口约束和 NVS 证据边界。
原生候选入口再增加两个负例，保护独立重算和原始 session 的精确字节往返。
维护 NVS 证据、游标保护、session 回放和 pack prune 纳入统一门禁，再增加四个负例，
保护存在性、原始长度、只读模式及读失败向调用者传播。
已有 outbox CRC/恢复副本、配额、会话重放、prune、M8 与
HIL 解析器 fixture 仍由工作流原有步骤执行，不把它们写成原本缺失。

| 故障模型 | 生产行为与检查入口 |
|---|---|
| 分配失败与完整 wire 契约 | `test_protocol_request_oom.py --validate-schema`、`test_problem_request_contract.py` |
| Note 安装交错、加载失败、跨 lane 旧结果 | `test_note_install_serialization.py`、`test_manifest_load_preservation.py`、`test_note_index_delivery.py` |
| 外部/持久化 JSON NUL 与 Unicode 身份 | `test_json_string_boundaries.py`、`test_pack_json_boundaries.py`、`test_sync_journal.py`；原有 outbox 恢复 CI |
| 传输终态、体积预算、无进展、preroll、release 期限 | `test_agent_voice_finalize.py`、`test_stream_transport.py --validate-pages`、`test_agent_run_progress.py`、`test_pcm_preroll.py`、`test_ws_release_deadline.py` |
| Flash 超大帧后的后续分片 | `test_flash_frame_assembly.py` 的真实有界组装器 |
| EPD 失败预算、AI 强制全刷与 UI | `test_display_recovery.py`、`test_ai_force_full.py`、10 个 `test_ui_*` 检查 |
| 结果容量与 receive-before-ACK 前提 | `test_display_result_capacity.py`；[R-04 当前结论与边界](DISPLAY_RESULT_CAPACITY_20261030.md) |
| WS 旧连接回调及待处理通知 | `test_ws_event_lifetime.py`；[R-25 生命周期与验证边界](WS_EVENT_LIFETIME_20261103.md) |
| 启动恢复、显式重置与不确定写入 | `test_storage_schema.py`、`test_storage_recovery.py`；[R-18 恢复入口与边界](STORAGE_RECOVERY_20261105.md) |
| R-08 进行中的原始保全与离线检查 | `test_outbox_forensics.py`；[保全和后续维修步骤](OUTBOX_RECOVERY_20261105.md) |
| R-08 原文绑定对账与审查计划 | `test_outbox_reconcile.py`；原生枚举、7014 事实、本地 HTTP 故障与三个语义负例 |
| R-08 本地 checkpoint 材料 | `test_outbox_checkpoint.py`；生产 session/descriptor/cursor 编解码与回放对照，窗口、暂停证据和两个语义负例 |
| R-08 原生合并候选 | `test_outbox_candidate.py`；完整 journal 前缀、跨分组回放、多模式 checkpoint、篡改拒绝和两个语义负例 |
| R-08 NVS/暂停与 pin 证据 | `test_storage_maintenance.py`、`test_word_cursor_guards.py`、`test_word_session_replay.py`、`test_pack_prune.py`；49/40/198 项读路径检查、pack 原生故障及四个负例 |
| UUID/count 边界、独立 storage owner 与期限 | `test_v3_sync_response.py`、`test_storage_prepare_deadline.py` |
| Word 错误、检查点与启动恢复联动 | `test_sync_control.py`、`test_sync_retry.py`、`test_word_ack_upload.py`；原有真实 Word 持久化/重放 CI |
| 功能关闭 | `test_agent_disabled.py` 的真实关闭分支入口；不宣称这是 Agent-off SDK 构建 |

## Word SEQUENCE_GAP 回归

历史 ACK fixture 的错误分类器返回可设枚举、code 固定为 `fixture`，只可证明上传
编排行为。本批扩展同步控制集成 fixture：上传循环、原始 code 分类、真实重试函数、
检查点构造/序列化/解析、启动恢复均取自当前生产源码；网络、outbox、文件写入为端口。

测试在第 1～5 次 `SEQUENCE_GAP` 后分别清除 RAM 并从已写出的检查点字节恢复，
到期前不请求、不递增，到阈值后隔离同会话后续记录，新会话从独立阶梯开始。
另测阈值前恢复成功、传输歧义与 retryable server error 不误隔离/挂起，以及首条
隔离后中断并重启。退休会话提示仅在 RAM：后一种重启会重新走有限阶梯，不伪称
提示已持久化，也不将隔离端口当作物理 flash 证明。

四个对应负例删除阈值升级、遗忘退休会话、丢恢复计数、误匹配其他会话。其他六个
负例保护显示预算、storage 绝对期限、强制全刷、UUID、Note 旧结果、Unicode guard。
每个变异必须 exit 1、输出行为断言失败并完成带失败计数的结果汇总；编译失败、
Python 异常、sanitizer 错误、信号终止、超时或变异边界漂移均令门禁失败。

## 最终配置契约

[`check_firmware_profile.py`](../../scripts/check_firmware_profile.py) 检查 IDF 生成的
`build-*/config/sdkconfig.json`，不从 defaults 推断最终镜像配置。显式传入
`-DWQN_BUILD_PROFILE=ai-local` 后，CMake ALL 目标检查 ESP32-S3、PSRAM、联网、控制
协议、AI/Agent、EPD、PM/睡眠、空闲预算与关闭自测/开发菜单等 17 项。`ai-local-agent-off`
显式允许 Agent 关闭，`ai-local-dev-menu` 明确要求开发菜单开启；三种契约都检查全部
17 项，默认产品契约不变。未选择契约的其他开发配置照常构建。检查器只输出差异键名，
不输出完整配置或未知值。CI 默认构建选择 ai-local，构建日志出现 warning 即失败。

## 证据边界

工作流已接入上述入口；本地运行和 SDK 构建不能充当远端 CI 执行结果。合成 HIL
smoke 保持解析器测试身份，动态栈水位、真实 EPD/BUSY、SPIFFS/掉电、网络阻塞、
PCM 连续性、功耗与发布验收仍按统一清单执行。R-18 已增加本地恢复入口，设备验收
后置；R-08 无副本无损维修仍待实现。R-04 普通背压触发链当前未证实；异常
owner/queue 故障来源仍需新证据，其他候选/政策项继续单列。
