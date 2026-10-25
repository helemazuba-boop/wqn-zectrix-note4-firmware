# 第十九批：Problem 离线记录编码失败时禁止成功确认

日期：2026-10-25。分支：fix/firmware-reliability。起点：4e7a5a8。

## 状态与范围

用户要求“推迟验收，继续推进代码修复”。第十八批及本批实机验收延期，继续按
已授权的逻辑批次本地提交、host/SDK 验证；不把延期记为验收通过。本批不处理词包
下载性能，不修改主目录其他会话的 Flash/audio 工作；未推送、未建 PR、未刷机。

| 提交 | 内容 |
|---|---|
| c4bc7ec | 检查 Problem 编码四字段结果、编码失败归类为 NO_MEM、180 个单次分配故障场景 |
| 本记录所在提交 | 验收延期、修复证据与后续代码任务队列 |

## 已确认问题及修复

EncodeObservationLine 原本只检查根对象和 Print；四个 AddStringToObject 的节点、
字符串值或 key 分配失败时，后续分配恢复仍能输出缺字段 JSON。CommitTransaction
只检查编码非空/长度，可能追加该行并返回 ESP_OK；随后生产解码器拒绝它。

生产函数回归复现：首次追加时拒绝一次 request_id 节点分配，提交返回成功、产生
一次写打开，而落盘行 decodable=0。该故障会破坏本地成功确认所承诺的记录完整性；
主机复现不表示已发生现场事故。

本批逐个检查 request_id/problem_id/action/occurred_at 字段；失败清理整个 JSON
树并返回空编码。CommitTransaction 将空编码返回 ESP_ERR_NO_MEM，超长编码仍返回
INVALID_SIZE，均在尾部修复和追加之前结束。正常字节格式、原持久写顺序、request_id
幂等语义保持。没有更换 outbox 格式或改变故障记录自动清理策略。

## 验证

- test_problem_outbox_recovery.py：1634 PASS / 0 FAIL，生产编码/解码、事务、真实
  libc 文件及 ESP-IDF 5.5.6 cJSON；ASan/UBSan/LSan。四种动作 × 15 个分配点 ×
  三种 journal 状态（不存在、已有有效记录、有效记录后有残尾）共 180 场景。
- 每次仅拒绝一个编码分配，后续分配恢复。提交失败、不打开写文件，原主/备份
  保留，残尾不被提前改写；原记录可查询，恢复后重试成功且幂等，无遗留 cJSON 节点。
- 旧源码及 5 个错误变体均被行为断言拒绝：忽略四字段结果分别复现 false ACK；
  失败树泄漏由存活分配断言拒绝，并得到 LSan 留证。
- test_problem_empty_cache.py：58 PASS / 0 FAIL，保留既有 I/O/空缓存覆盖；其 codec
  为桩，不能替代上一条真实 JSON 回归。生产 problem_store.cpp 的 CRLF 保持。
- idf.py -B build-wsl reconfigure build：零警告，M8 与编译后 UI 栈门通过。显式
  reconfigure 后候选身份为 c4bc7ec2。文档链接与 CRLF-aware diff 检查通过。

测试 allocator hook 仅存在于 host fixture，不安装到设备或影响其他任务。测试不证明
设备物理掉电、真实内存压力、Flash 原子性或调用者实际栈高水位。

## 构建与归档

本批按验收延期后的代码推进节奏，只更新默认诊断开启构建；没有重建诊断关闭对照。
之前第十八批构建对作为历史保留。后续电气验收时需为联合候选生成同源构建对，
不能用不同运行时代码的两份镜像归因诊断开销。

源提交：`c4bc7ec2d39af5beb99cb703289e3cda07b16234`。候选：`c4bc7ec2`。SDK：v5.5.6。
应用描述符：`Oct 24 2026 23:59:21` UTC。
独立工作树固件目录的 `dist/reliability-b19-20261025/manifest.json` 保存配套 Flash 文件、
配置指纹、ELF/map、host/变体日志和下一项解码探针。完整私有配置未入 Git。

| 项目 | 诊断开启 |
|---|---|
| app 大小 | 3651264 B |
| app SHA256 | `62e39bf60443657760c974068f9acfce83a46869436c01274f3c175725d8439c` |
| ELF SHA256 | `75707c334a0a8c53f669859f71ff0b53c73e746a80cb91b627c77e46328bfb5b` |
| sdkconfig SHA256 | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

## 下一代码批次及延期设备项

1. **优先：Problem 解码资源失败保全。** 额外生产函数探针已复现：记录 A 提交成功；
   读取 A 时拒绝一次 JSON 分配，再提交 B 返回成功，查询只剩 B，A 已不在主/备份。
   原因是 DecodeObservationLine 的 false 同时表示坏数据和解析资源不足，读取层据此
   修复 journal。本批编码修复未覆盖此路径，单列下一批；需定义错误分类和坏帧
   恢复政策，保持有效记录保全，并建立独立回归。
2. **随后：Note 跨 lane 清单安装。** 全量同步与单本下载分别读取/合并再保存清单；
   第十六批只保护活动临时路径，尚无整轮 manifest 隔离。先回放丢更新/淘汰交错，
   再落实 owner 内合并发布及版本检查，避免用网络期间的永久等待解决。
3. **再后：旧适配层审计。** Note/Problem 分开检查 operation/generation 绑定、
   取消/失败后的 lease/busy 归还、隐藏页面资源和会话恢复；候选问题确认后才修复。
4. **独立设计：OTA 启动健康门。** 明确业务启动、迁移意图重启、回滚和离线数据兼容
   的判定后再改确认时机；不将现有双 OTA 分区当作完整升级产品流程。

以上是代码任务顺序。第十八/十九批实机矩阵、低电/电流及既有精确故障窗口保留延期，
后续按候选身份补测，不阻塞当前已授权的代码修复；未执行的项保持未覆盖状态。
