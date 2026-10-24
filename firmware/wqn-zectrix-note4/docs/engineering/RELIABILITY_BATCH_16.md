# 第十六批：同一路径包流排他与活动期间缓存重置暂缓

日期：2026-10-24（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`96753a4`。

## 状态与交付

用户回复“通过，继续”，第十五批记为用户确认节点验收。本轮检查两条 cloud lane
之间的活动文件所有权，修复重复 Begin 删除同一路径临时文件，以及活动包流期间
缓存重置仍可删除文件的问题。新的普通运行片段没有覆盖该竞争窗口。

| 提交 | 内容 |
|---|---|
| `80b3929` | 有界路径占用表、三域下载准入、破坏性缓存重置门、真实回调交错与单元回归 |
| 本记录所在提交 | 第十五批用户验收、触发证据、资源成本、当前构建对及设备节点 |

继续在 `/home/unknow/projects/firmware/.worktrees/reliability-20261024` 工作，保留
a565923 安全基线和此前可靠性补丁；主目录的其他工作未纳入本批。沿用先本地提交、
关键节点集中验证的授权。未推送、未创建 PR、未刷机，远端 CI 未执行。

## 明确触发路径

`CloudDomain::kNoteBulk` 与普通 `kNote` 分别拥有结果槽；全量 `kPackSync` 走 bulk
lane，`kFetchNotebookPack` 走 interactive lane。前者调用 SyncNotePacks，后者调用
SyncSingleNotebookPack，最终都可调用 DownloadNotePackToStorage。

二者在包尚未落盘时都可能判断需要下载。同一个包使用相同生产 temp path；两次
Begin 都执行 remove(temp) 后 fopen。StorageService 只串行化单笔事务，因此当第一个
FILE 跨事务等待网络数据时，第二个 Begin 可以删除它。第十三批的通用容量回收保护
没有覆盖另一个下载者自己的 Begin。三域采用同样的流生命周期，统一补此准入。

还确认 ResetWord/Note/ProblemPackStorageCacheRaw 会先删 manifest，再枚举删除
域内最终包与 `.tmp`。例如全量同步发现本地清单异常后，重置可与单本下载交错；
只挡第二个 Begin 仍不足以保护活动文件，因此本批同时在 reset 的首个文件操作前加门。

这是源码与主机交错复现确认的生命周期问题，没有根据本轮普通日志报告现场事故或
学习记录丢失。SPIFFS 删除打开文件会释放相关 fd 的 SDK 依据沿用第十三批；本轮
libc 文件测试没有模拟 ESP32 VFS 的全部句柄复用语义。

## 最小实现与返回契约

新增任务上下文使用的 PackStreamPathLease：最多 8 个路径，长度上限为 96 字节
（含终止符预算），按完整临时路径字符串比较。空/过长、重复路径或表已满时不取得
占用。不是按字符串指针地址判断，同内容的不同字符串对象也会冲突。

三域在路径准备后、Begin 入队前取得占用；失败立即返回 ESP_ERR_INVALID_STATE，
不执行 Begin、Abort 或删除竞争者的文件。同路径请求不会等待另一条完整网络传输，
不同路径可以同时持有占用，原存储 owner 仍串行执行各笔文件事务。

占用覆盖 Begin、Append、Commit/Abort。对象禁止复制/移动，并声明在拥有路径字符串
的 context 之后，析构先注销路径再释放 context 缓冲。表只借用不可变字符串指针，
调用者须保持其有效至注销；不能在占用期间移动或修改路径缓冲。

ResetRaw 在 StorageService 内先检查有无活动包路径；非空则返回 INVALID_STATE，
在删除任何 manifest/temp/final 文件之前退让。该规则对任一域的活动包流都生效，
因此某域的破坏性恢复可能暂时等待另一域下载结束。全部退出后，后续重试按原 reset
流程执行；没有把“没有实际重置”伪装成成功。

既有 full-sync CompleteContentRefresh 对失败使用 backoff；单本拉取按原错误结果
收尾，用户可再次确认重试。未新增产品菜单、永久等待或配对状态改变。已有命中本地
缓存的判断保持，不提供多个请求共享一次流/进度的 coalescing 协议。

这里处理活动流的临时文件所有权与 reset 入场，不是整轮 manifest 安装的事务隔离，
也不新增缓存发布/掉电原子性保证。

## 同步顺序与成本

路径表在专用 portMUX 保护下比较/登记/注销；临界区内不分配、不做文件 I/O，
不在网络传输期间持锁。字符串长度在入锁前有界检查，比较最多 8 个有界 key。

reset 的零占用检查在 owner 事务内：若新请求之后才登记，其 Begin 只能排队，
FILE 会在 reset 结束后才打开；若 FILE 已经打开，其占用应已在 Begin 入队前发布，
reset 必须退让。临时文件通用回收仍由原 StorageTempFileLease 保护，两个机制配合。

本候选 ELF/map：8 个指针为 32 B BSS，锁为 8 B data，共 40 B 普通内部 RAM，
未计段对齐。没有复制路径 key、动态表、worker 或周期采样，也没有新增 RTC/Flash
持久化。三个下载入口单帧仍为 384 B，三个 stream transaction 单帧为 176 B；新增
构造/析构和锁调用仍有调用链成本，不能由入口帧相同推断实际高水位不变。

## 回归结果

- test_pack_stream_path_lease.py：1026 检查通过，ASan/UBSan/LSan。覆盖同内容不同
  地址、不同路径、失败占用者析构不影响原占用者、8 槽满/释放/复用、长度拒绝、
  1000 次复用、八线程同路径只有一个成功。锁是主机任务锁端口，未验证 ESP32 SMP。
- test_pack_stream_allocations.py：3826 检查通过，执行三域真实回调/下载调度和
  cache-reset 源函数。HTTP 等待中嵌套第二个同路径请求，第二个在 Begin 前返回，
  不执行 Abort，原 FILE 仍打开且能写完发布；不同路径两者都完成，退出后同路径
  可再取得。活动期间 reset 保留全部三种 manifest，结束后 reset 才真正清理。
- 保留第十五批普通 C++ 分配拒绝、摘要/错误/关闭路径；Metadata 路径、RTOS、SHA
  与 HTTP 部分为端口，不是完整 ESP32/SPIFFS 集成测试。
- 旧源码及 10 个错误变体均被行为断言拒绝：三域任一漏接、过早释放、漏 reset 门、
  删除 manifest 后才检查、按指针地址比较、全域串行化、槽泄漏、释放错误槽。
- 原 391 项临时文件回归和 148 项 caller/owner 回归通过；Note UI 15639 断言通过，
  未据此宣称两条真实 lane 的全链路设备验收已完成。
- 两份 ESP-IDF 5.5.6 镜像零警告、M8 与编译后 UI 栈门通过。新增路径回归进入 CI；
  CRLF-aware diff 和本批文档相对链接通过。未重复无关 ADC/录音长测。

## 新日志边界

原输入 `logs/serial-COM7-261024-224124-005-53832.log`，冻结副本 SHA256：
`20817a31dce007364f419e7aa3c024818cf26f737fa3157c71a74473964d70d7`。
14 条完成事务缺 build/boot/mem_diag 身份：11 OK、3 正常 NOT_FOUND，无已观察到的
异常存储结果或低栈/放弃事件。最大 queue=9 ms、exec=2455 ms；一次容量 27.112%。
保持未知镜像，不追认为当前候选；该片段没有补齐同包竞争、重置或压力注入矩阵。
也不以这个较短片段未再出现接口错误，就宣布上一批 500/503 外部依赖已经恢复。

## 当前镜像

候选 `80b3929a`，完整源提交 `80b3929a8d4771e100bb0392fc83064b71b0ffd7`，SDK `v5.5.6`。
应用编译时间 `Oct 24 2026 15:28:31` UTC，WQN_BUILD_TIME `2026-10-24 15:28` UTC。

本独立工作树的固件目录中，清单为
`dist/b5-diagnostic-pair-b16-20261024/manifest.json`；关闭版使用
`build-b5-diag-off-b16-20261024`，本工作树 build-wsl 与 sdkconfig 保持诊断开启。
两版配置仅差 RAM 诊断开关及三个依赖项，应用描述符时间一致，bootloader/分区表/
OTA 初始数据相同。完整私有配置未入 Git，历史构建对不与本批混配做诊断归因。

| 项目 | RAM 诊断开启 | RAM 诊断关闭 |
|---|---|---|
| sdkconfig SHA256 | `70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` | `0d3492e21dda7471b3753544bf5fd85b29273d1dfaa6f62b3c24eb2ce15162cc` |
| app 大小 | 3651184 B | 3646736 B |
| app SHA256 | `83c696f2e99d85e30ae1db112430132c415beff2e0931a8819aad7015ae2acc1` | `7f20af58daab63e53a50d42c1f9a855665348adb87f0eec37d463a916d575689` |
| ELF SHA256 | `fc399a5e725ea74a3e66115c4baf59c9917ffbd64db627b857ac4cda7e0ae099` | `c52493992e912e20fea5d63912dc7f5a07641d0ff935804ccbca679d862df27e` |

## 设备节点

1. 核对镜像身份，在可控数据/设备上交错全量 Note 同步与同一 notebook 的正文拉取；
   确认仅一个实际流进入 Begin，另一个按失败状态收尾，原下载继续、内容完整。
2. 换为不同 notebook/路径，确认允许并发，未被全域锁住；占用者完成/传输中断后
   同路径可重试，没有长期 busy 或槽泄漏，既有确认重试和 backoff 可达。
3. 活动包流期间触发可控 cache reset，确认先返回忙、清单/临时文件未被删除；退出
   后确认 reset 能恢复执行。生产 UI 没有注入入口的精确场景继续留待测试设施。
4. 在真实双核/Flash 并行场景观察临界区、heap 与任务栈；继续 B5 高占用回收、时间
   跳变、诊断电流与低电持久化。主机嵌套交错和线程锁不是设备调度/电气证明。
