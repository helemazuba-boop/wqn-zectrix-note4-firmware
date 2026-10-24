# 第十五批：包流回调复用路径与固定长度摘要

日期：2026-10-24（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`6597bae`。

## 状态与交付

用户回复“通过，继续”，第十四批记为用户确认节点验收。本轮继续检查失败清理，
修复三域流式回调中的重复路径构造和提交时的动态 SHA 字符串分配。新日志包含
服务端接口失败，和本批内存路径的定向复现分别记录。

| 提交 | 内容 |
|---|---|
| `c5b340d` | 三域下载打开 FILE 前缓存路径、固定长度摘要编码、完整生产回调分配拒绝回归与 CI |
| 本记录所在提交 | 第十四批用户验收、新日志边界、资源取舍、当前构建对与设备节点 |

沿用先本地提交、关键节点集中验证的授权。工作仍在独立工作树
`/home/unknow/projects/firmware/.worktrees/reliability-20261024`；保留已有 a565923
安全基线。主目录仍在安全分支，安装时从本工作树取产物。未推送、未创建 PR、
未刷机、未运行远端 CI。

## 剩余问题与实际修改

第十四批移除了后台完成对象的分配依赖，但 Word/Note/Problem 的
`*PackStreamTransaction()` 在进入 switch 前仍每次调用路径构造函数，生成临时与最终
路径。Begin、每个 Append、Commit、Abort 都会重复进行；路径构造和 SHA reserve
可能请求 C++ 堆内存。内存不足导致传输失败时，Abort 自己还可能在关闭 FILE 之前
请求新分配。Commit 也使用 `std::string::reserve(64)` 编码 SHA。

现在每个下载 context 在 Begin 入队之前缓存 `temp_path`、`final_path`；后续回调只用
const 引用。新 guard 拒绝缺失路径的内部 context，避免不完整准备进入文件操作。
两个路径仍调用原生产路径函数，文件名、stem、后缀、包 ID/SHA 组成规则不变。

提交时使用固定 64 字节数组渲染 SHA 的小写十六进制字符，检查期望字符串长度后
比较全部 64 字节。保留原 SHA 不符、flush/sync/close/rename 失败的结果和清理分支。
没有改变存储 owner、三域协议、已有临时文件保护和后台完成等待生命周期。

这移除了所检查回调中的路径/摘要 C++ 分配。准备路径仍可能失败；底层 libc、RTOS、
mbedTLS、文件系统或日志也可能使用内存。不是“任意内存耗尽时下载都能完成”的保证，
也没有新增跨掉电文件发布保证。

## 执行证据与成本口径

`scripts/test_pack_stream_allocations.py` 编译生产的路径/stem 函数、真实 stream context、
Begin/Append/Commit/Abort 回调及完整下载调度。SHA 算法、HTTP 和 RTOS 调度为可控
端口；文件使用真实 libc。使用普通 C++ new 拒绝/计数钩子，未拦截全部 C malloc、
自定义或对齐分配，不将端口结果当作 ESP32 分配器测量。

在同一主机、三片数据传输用例中，每域都有以下观察：

| 项目 | 6597bae 旧源码 | 本批源码 |
|---|---|---|
| 临时/最终路径构造函数调用 | 10 次 | 2 次，均在打开 FILE 前准备 |
| 回调内观测到的普通 C++ new | 31 次 | 0 次 |

旧路径会在 Begin 和三次 Append、Commit 各构造两个路径。主机路径根目录、标准库
及字符串容量和设备不同，31 不是 ESP32 的绝对分配次数，更不是毫秒或电流结果。
重复路径构造来自实际被调用的生产代码，不是链接器已丢弃的死代码。

新增 context 的两个字符串对象及其路径缓冲保留至下载结束，替代回调中的临时对象。
因此可能增加两个回调间的保留内存，也使用更多调用者栈。没有从次数减少推断峰值
RAM 或实际响应延迟降低。候选三域下载入口单帧均为 384 B、stream transaction 单帧
均为 176 B，反汇编摘要随产物归档；它们不含完整调用链/ISR。实际栈高水位和 heap
仍需设备验证；任务栈大小、采样节奏、ADC 等待与 UI 分钟刷新保持。

## 回归结果

- 新入口 3292 检查通过，ASan/UBSan/LSan。Begin 后拒绝普通 C++ 分配，覆盖成功
  三片传输、网络失败后的 Abort、打开/起始/hash-update/hash-finish、CRC 不符、
  flush/sync/close/rename 失败；验证关闭计数、临时文件清理、结果、最终内容和租约释放。
- 16 组给定 digest 在三域上验证生产十六进制编码和比较；期望值用独立 printf 格式化。
  这不是 mbedTLS SHA 算法或 ESP32 加速器测试。
- 旧源码被分配拒绝用例捕获：Abort 在关闭 FILE 前申请路径内存，bad_alloc 逸出。
  恢复任一域重复路径构造、恢复堆摘要、忽略摘要、漏准备路径、错用最终路径的 7 种
  错误实现均被行为断言拒绝。ESP32 异常禁用配置下的终止行为未用主机异常捕获冒充。
- 第十三批 391 项生命周期/回收检查、第十四批 148 项 caller/owner 检查通过。前者
  fixture 补齐新的准备字段，StorageService 仍是端口，不称为完整 ESP32 集成测试。
- 新回归加入既有 storage-schema CI job；远端 CI 未运行。双 SDK 构建零警告、
  M8 与编译后的 UI 栈门通过；CRLF-aware diff 和本批文档相对链接检查通过。

## 新日志与外部失败

原日志：`logs/serial-COM7-261024-205608-856-43476.log`，冻结副本 SHA256：
`f3016c5e643589533ad7a297b4117e1df6a4797468d97a64384d68c286464eef`。
副本和分析结果在本批 dist 的 `device-snapshot.log` / `device-evidence.json`。

该片段缺当前 FAULT:boot 身份头，80 条完成事务保持 build/boot/mem_diag 未知，
68 ESP_OK、12 正常 NOT_FOUND，未观察到异常存储结果或存储低栈/放弃事件。
最大 queue_wait=9 ms、exec=2262 ms。三次容量观测为 27.083%、27.106%、27.112%，
没有高占用测量，不按时间文件名猜测它安装了哪份二进制。

同时，日志记录接口 503 / IDEMPOTENCY_UNAVAILABLE、AUTH_SERVICE_UNAVAILABLE，
以及 problem manifest 的 500 / ARTIFACT_STORAGE_ERROR，均标 retryable=1。
“存储事务未观察到异常”不代表整轮同步成功。这些接口响应作为服务端依赖另列，
本补丁来自源码和定向复现，不把路径分配优化说成对这些响应的修复；未修改云端仓库
或生产服务。也不以该日志证明本批新候选已经上机。

## 当前候选

候选 `c5b340d1`，完整源提交 `c5b340d137799af3758a23d2720d780fa2737dec`，SDK `v5.5.6`。
应用编译时间 `Oct 24 2026 13:26:33` UTC，WQN_BUILD_TIME `2026-10-24 13:26` UTC。

在独立工作树的固件目录内：`dist/b5-diagnostic-pair-b15-20261024/manifest.json`。
关闭版目录 `build-b5-diag-off-b15-20261024`；本工作树 build-wsl 与 sdkconfig 保持
诊断开启。两版完整配置仅差 RAM 诊断开关及三个依赖项，应用描述符时间一致，
bootloader、分区表、OTA 初始数据相同，私有配置未入 Git。

| 项目 | RAM 诊断开启 | RAM 诊断关闭 |
|---|---|---|
| sdkconfig SHA256 | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` | `0d3492e21dda7471b3753544bf5fd85b29273d1dfaa6f62b3c24eb2ce15162cc` |
| app 大小 | 3650848 B | 3646368 B |
| app SHA256 | `2cfcf0e4bfcb723e663764d655ccc948f77b364d3b516b77eaecc53d80a0e771` | `92a495ebcf9b8d559fb9b00b45aec8b0a5cb7be53dbd0f93436055bbcd4de365` |
| ELF SHA256 | `bc5023f9838f3cbbf42b896a83cb7c1177274ecc75225506c781333d580bfe2f` | `7015fafd9edaa8325eb24391ae2371dc6a7d9f757d1c31f792f86b4ed70b514d` |

产物保留主机测试、旧源码/错误变体结果、构造次数前后记录、新日志快照及静态栈帧。
不同代码版本的构建对不混用后只归因于 RAM 诊断开关。

## 设备节点

1. 从本工作树取候选并核对身份；对 Word/Note/Problem 分别验证实际多片下载、完整
   内容和 SHA 校验，覆盖传输中断/取消后重试。
2. 观察活动下载时路径缓冲的保留成本、调用者/存储任务实际栈余量、heap 最小值和
   分配失败记录；有故障注入设施时验证准备成功后的分配压力窗口。
3. 继续既有跨 lane/容量回收/时间跳变、高占用 outbox 与诊断电流/低电矩阵。普通
   低占用片段、主机 C++ 分配计数与零警告构建不代替这些硬件证据。
4. 对日志中的 retryable 500/503 保留请求/轮次与服务端错误码，单独确认依赖恢复；
   不通过清空 token、擦除本地数据或改变重试状态来掩盖接口失败。
