# 三模式音频修复实机验收

## 候选版与验证边界

保留单路 RX DMA 等容量分片和 Agent 工具边界思考文本修复，将 STD 7168 B、
Agent 9216 B、Flash lifecycle 8192 B 工作栈迁入 PSRAM。令牌读取通过
StorageService 前台队列执行，存储睡眠租约覆盖等待和执行。HTTP 401 清理
继续由存储任务执行。StorageService 栈、WiFi 配置、Flash 双向 DMA 容量和
10384 B 准入门槛保持原值。

主机回归和构建不能证明缓存关闭安全、RTOS 并发、录音质量或设备验收。
PR #9 在矩阵完成前保持 Draft。无需提交或推送即可测试本地候选版。

## 构建、刷机与证据

在 ESP-IDF v5.5.4 环境中，从本目录执行：

```sh
python3 scripts/test_audio_runtime.py
python3 scripts/test_audio_rx_dma.py
sh scripts/run_agent_round_policy_test.sh
python3 scripts/test_release_flash_identity.py
python3 tools/release/release.py build --build-dir build-ai-local-s3
python3 tools/release/release.py verify-build --build-dir build-ai-local-s3
git -c core.whitespace=cr-at-eol diff --check
```

Windows 运行本目录 `deploy.bat`，使用 COM7。刷机后对照
`dist/flash-history.jsonl`、候选版本的 `identity.json` 和串口版本信息，
记录版本、源码 fingerprint、app SHA256。三个工作栈的启动行必须有
`in_psram=1`；内部回退属于降级，不能计作通过。

验收版关闭 `CONFIG_WQN_AUDIO_RESOURCE_DIAGNOSTICS`。即使调试开启，完整
堆／任务账本也只允许在采集尚未启动时打印。默认版保留每次启动、结束、
网络失败和工作任务栈 HWM 摘要。

每个场景单独保存串口日志，记录模式、轮次、说出的句子及识别结果。
监听器重新附着不算设备重启；不要用旧镜像日志替代候选版证据。

## 正常场景矩阵

| 场景 | 操作 | 核验 |
| --- | --- | --- |
| Flash | 连续 20 轮，每轮至少 5 秒，正常轮次等待上轮响应结束 | 双向准入、上行音频、识别、播放成功，无初始化或发送失败 |
| STD | 连续 20 轮，每轮至少 5 秒 | 样本、FINAL 成功；稳定网络下无意外 HTTP 回退 |
| Agent | 连续 20 轮语音输入，每轮至少 5 秒 | 转写及确认提交正确，无重复提示提交 |
| 混合模式 | Flash → STD → Agent → Flash 循环 20 次，包含立即开录 | 旧通道释放，新回合不被旧闲置清理关闭 |
| 并行工作 | 录音期间触发同步并允许 EPD 刷新，重复 5 轮 | 无持续采样缺失、任务创建失败或内存不足 |
| 思考文本 | 同一 Agent 回合触发至少两次工具调用 | 每次工具边界清空旧思考，后续思考及回答正常 |

STD／Agent 查看 `capture stop`；Flash 查看 `Flash capture stop`。
`duration_ms` 由样本数计算，`wall_duration_ms` 从 ADC 预热丢弃结束后计时。
正常 5 秒以上录音每轮差值绝对值必须 ≤100 ms；取消前尚未采到样本属于
取消场景。样本时长符合也不能代替人工检查识别是否截断。Flash 样本数统计
实际读取的 PCM，发送是否成功须同时检查上行与响应。网络阻塞造成采样
缺口依然算未通过，不能通过延长发送超时掩盖。

## 取消、收尾与恢复

1. 三种模式各做 5 次：准备阶段释放按键、录音后取消、随后立即重录。
   取消不发送空 FINAL、不残留音频活动或网络 demand。
2. Agent 列表请求未完成时切到 Flash，有限请求完成后 demand 归还。
   长运行 Agent 离开页面仅 detach，云端任务继续，回来可重新观察。
3. STD／Agent 远端响应未结束时离开 AI 页面，活动流不被闲置清理中断，
   结束后闲置连接可释放；新连接或开录使旧清理请求失效。
4. 三种模式各做 3 次：断网、恢复、重新开录。保留现有 HTTP 回退及
   FINAL 不确定性规则，无重复请求或后台永久等待。
5. 冷启动、正常睡眠唤醒后分别重测三个模式各 3 轮，无缓存关闭异常、
   栈溢出、Guru Meditation 或受控重启。
6. 在测试网关验证 HTTP 401，清理对应设备授权后再配对恢复录音。
   其他网络错误保留授权，没有 PSRAM 栈触发 NVS 断言。

## 通过门槛与待测记录

- 无 `Flash duplex admission rejected`、`Error create websocket task`、
  `RX DMA exhausted`、I2S 初始化失败、栈溢出或异常重启。
- 正常网络矩阵无 PCM 发送失败；断网注入有正确回退、取消和恢复。
- 新工作任务、UI／EPD 实测 HWM 均 >2000 B；未覆盖调用链单独标待测。
- 工作完成、页面离开且后台请求收尾后 demand 归零，Flash 通道释放。
- 同一闲置状态比较启动、10 轮、20 轮、混合模式结束的内部／DMA free
  和 largest，须达到稳定区间，无逐轮持续下降。TLS 瞬时高峰或有限请求
  重叠不直接判为泄漏。
- 必测场景缺日志或失败即未验收通过；主机 PASS 和检查器 SKIP 不能替代。

| 记录 | 状态／证据 |
| --- | --- |
| 构建版本／fingerprint／app SHA256 | 待填 |
| Flash／STD／Agent 各 20 轮 | 待测 |
| 混合切换／并行工作／思考文本 | 待测 |
| 取消／断网／睡眠／401 | 待测 |
| 采样时长／HWM／闲置内存 | 待测 |
