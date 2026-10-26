# 云端与固件 release 集成验证

日期：2026-10-27（Asia/Shanghai）。工作开始于 2026-10-26，分支/归档目录保留开始日期。
本轮目标是将当前更新整理为可审阅的 release PR。未直接更新 release、发布、迁移生产数据库或刷机。

## 集成范围

| 仓库 | release 基线 | 纳入更新 | 集成分支 |
|---|---|---|---|
| WQN cloud | `73d0d2f` | `fix/cross-repo-security-20261023@af73247` 的 13 个提交；账号/筛选授权、取消与回执、分页/并发/摘要、STD 确认 | `merge/cloud-release-20261026`，见 [PR #108](https://github.com/helemazuba-boop/Wrong-Question-Notebook/pull/108) |
| Note4 firmware | `3ad5747` | 可靠性 `b8f49aa` 与 Flash/STD/诊断 `becd306`，包含第二十批 Problem 解码保全 | `merge/firmware-release-20261026` |

独立工作树中的 merge 保留原提交历史。主开发工作区及其未跟踪文件不入本次提交；
其他独立特性分支沿各自发布工作继续。

## 合并与测试适配

固件出现两份文件的内容冲突：`word_pack.cpp` 同时保留提前准备路径/活动路径 lease
与下载分项计时；`storage_temp_lease.cpp` fixture 同时保留路径与计时类型。计时起点
在路径准备前设置，Begin/Append/Commit/Abort 的路径所有权和无额外 C++ 分配约束保留。

生产代码合并后，为 host 环境补齐 allocation fixture 的计时/SafeKey 端口，profile
fixture 按新上下文准备路径，endpoint fixture 提供诊断端口及 request ID。首轮全面
回归中的两个失败日志保留，修正后定向重跑通过；不将未编译或旧 fixture 的结果算作 PASS。

CI 增加 pack stream allocation/path/profile、STD 输入/流处理、Flash 上行及 RX overflow
的自动入口。它们执行生产函数与可控端口，不代替设备电气、网络或 RTOS 验收。

## 已完成的本地验证

| 项目 | 结果 |
|---|---|
| 固件 host | 全部 63 个 `scripts/test_*.py` 通过；ASan/UBSan/LSan 按各脚本应用 |
| 固件默认构建 | 从空 build 目录和 defaults 配置构建 ESP32-S3；IDF 5.5.6，零警告，M8/UI 栈门通过；UI owner entry frame 1840 B <= 2048 B，仅为编译帧门 |
| Agent off 构建 | 真实 SDK 条件编译通过，零警告，M8/UI 栈门通过；不改变默认配置，不作为诊断关闭功耗对照 |
| 端云协议 | `ai-interaction-v1` 的 11 份文件逐字节一致 |
| Cloud quality | Node 24，`npm ci`、type-check、lint、Prettier 通过；lint 0 errors / 164 warnings |
| Cloud Vitest | 常规套件 1753 passed / 11 conditional skips；另外启用真实本地 PostgreSQL 的 voice-write 与 STD 执行器，10 passed |
| Cloud build | Next.js 生产构建和客户端密钥扫描通过；保留 2 条动态文件追踪警告，不将其写成零警告 |
| Cloud DB clean | 独立 Supabase 从空库应用 83 个迁移，SQL/权限与账号生命周期回归通过；从 release 基线升级的迁移及数据保留回放亦通过 |
| 部署/CI 隔离保护 | 部署脚本回归通过；CI 栈分配和清理保护 7 项通过 |

实际数据库执行器测试使用新建的 `wqn_voice_test_*` 库和业务 schema；复制时排除只允许
原数据库安装的后台扩展，不接触生产库。测试关闭了外部网络/提供商依赖的边界与真实
执行 SQL 的边界分别保留。普通测试中的条件跳过不冒充实际运行通过。

## 固件产物身份

默认运行时源码：`d01acf59411e59e61227cd9f2b83ebba12266281`；镜像内 build：`d01acf59`。
Agent off build：`d38d0827`。两者之间仅有 host 端口提交；运行时 main/components 源码相同，
配置差异与两个实际构建身份分别保留。归档位于集成工作树固件目录：
`dist/release-validation-20261026/manifest.json`、`host-results.json` 及各脚本日志。

| 产物 | 默认构建 |
|---|---|
| app 大小 | 3678160 B |
| app SHA256 | `c7067c00e29c6d03b46d92886feb5ee8c2ac2f1a0155ff8174ac0b5d28b51e98` |

私有 sdkconfig、ELF/map、镜像及原始日志不入 Git。此镜像仍使用开发版本号，PR 不等于生产固件发布。

## 尚未完成的发布条件

[统一验收清单](../engineering/FIRMWARE_RELIABILITY_ACCEPTANCE.md)仍是唯一执行入口。
本轮开始 A 类联合验证，S/P 的真实设备项目待执行；没有因开始验证而撤销历史用户节点确认。

- P00 仍未满足：Note 全量/单本清单安装交错待回放/处置，旧 STD 卡顿观察需在联合候选复核。
- S01–S07 按影响合并操作：离线保存/重启/上传 ACK、同步与前台衔接、STD/PRO/Flash 终态恢复、输入显示及睡眠恢复；词包需同包分项计时。
- P01–P04 按条件补真实掉电、介质、低/高占用、实际任务栈余量、弱网/上行背压与电池电流/低电持久化。实际 Agent-off 构建不能替代诊断开/关对照。
- Cloud 两个新增迁移需先于 App reader 与 STD 功能部署；App/realtime-proxy/固件部署顺序和真实提供商验收留在发布窗口。远端 CI 以 PR 页面为准。

固件 PR 先以草稿供代码审阅；满足对应 S/P 与缺陷处置后再进入发布合入。没有自动合并或部署动作。
