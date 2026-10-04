# Dev 诊断菜单规范（DEV_DIAGNOSTICS）

> 状态：已批准的实施规范。本文档是 Dev 菜单的**唯一结构定义**：新增调试信息必须进本文档列出的既有对话框；要加第四行菜单，先改本文档再改代码。

## 1. 目的与范围

固件仍在开发阶段。Dev 菜单是设置页「开发者选项」进入的**二级菜单**，用于回答高频问题：

1. **这台设备跑的是哪个构建、为什么重启的**（Dev 信息）；
2. **同步为什么失败、卡在哪个域**（同步诊断）；
3. **最近发生过哪些错误**（错误记录）；
4. **电量/存储的原始数字是多少**（电量原始、存储详情；两者原本平铺在一级）；
5. **睡眠为什么没进去**（电源睡眠诊断）。

**v1 边界**：除 §4.2 同步诊断的「双击确认重下词库包」外，全部只读。无其它写操作、无其它危险动作、无自动刷新、不做 `esp_log` vprintf 全量捕获、不触碰 persist-worker 事务协议。要加可写操作时先扩展本文档（该动作是 2026-10-04 首个例外，动因是 HIL 需要可复现的多 MB 包写负载）。

## 2. 配置与发布策略

- 新 Kconfig 开关：`CONFIG_WQN_DEV_MENU_ENABLE`（`main/Kconfig.projbuild`，`depends on WQN_EPD_UI_ENABLE`，**default n**）。
- 门控语义：开关**只门控设置页一级的「开发者选项」入口行**（`#if` 预处理器裁剪；二级六行随之不可达）；错误捕获（`RecordError`）**常开编译**，发布固件同样保留——失败路径才执行，代价约 1KB flash / 0.8KB RAM 静态环，换来现场设备的问题回溯能力。
- 发布保障：default n + `RELEASE_CHECKLIST.md` §1 校验步（记录的 sdkconfig 中 `# CONFIG_WQN_DEV_MENU_ENABLE is not set`）。开发机用 menuconfig 打开（写入 gitignored 的 `sdkconfig`，不进版本库）。
- `sdkconfig.ai-local.defaults` 中保留注释行 `# CONFIG_WQN_DEV_MENU_ENABLE is not set`（照 `WQN_AI_AUDIO_SELFTEST_ENABLE` 先例），显式文档化 off 态。

## 3. 入口结构：一级一行 + 二级六行（固定）

设置页一级在「固件版本」之后、「恢复出厂」之前插入**一行**入口（`ui/page_settings.cpp` 的 `root_titles`/`root_values`）：

| 一级行 | 标题 | 值列 | 动作 |
|---|---|---|---|
| +1 | 开发者选项 | `只读诊断` | 进入二级列表（`SettingsView::kDev`）；其中「同步诊断」有一个动作，见 §4.2 |

确认该行进入二级列表（`dev_titles`/`dev_values`，六行 = 面板 6 行窗口，不需要滚动）：

| 二级行 | 标题 | 值列 | 对话框 | 内容 |
|---|---|---|---|---|
| 1 | Dev 信息 | git hash 短码 | `kDevInfo` | 构建标识、复位原因、运行时长、堆/PSRAM（§4.1） |
| 2 | 同步诊断 | 上轮结果 | `kDevSync` | 同步计数、三内容域与三 outbox 的 phase/重试/last_error（§4.2） |
| 3 | 错误记录 | N 条 | `kDevErrors` | `error_recorder` 环形缓冲最近 6 条（§4.3） |
| 4 | 电量原始 | battery_mv | `kBattery` | adc_raw / adc_mv / battery_mv / percent / 充电标志 / 拟合公式（§4.4，原一级「电量详情」对话框） |
| 5 | 存储详情 | NVS used/total | `kStorage` | Flash、内容同步 revision、NVS 条目、PSRAM（§4.5，原一级「存储详情」行） |
| 6 | 电源睡眠诊断 | N 条 | `kSleepDiag` | `sleep_diagnostics` RTC 环最近 6 条（§4.6） |

一级「电量」行保留用户可见的百分比，确认只写 notice（`电量 87%`，照「固件版本」行的做法）；原始数字移到二级。

**导航**：上下移动 `dev_selected`（环绕），确认打开对应的对话框，**长按确认返回一级**（一级长按确认仍是回首页）。对话框除 §4.2 外均为只读。

**为什么是二级而不是平铺**：一级已有 12 行（「AI 回复时翻页」加入后），而 dev 信息还在增长（电源睡眠诊断是第六项），平铺会把「恢复出厂」「关机」推得更远；二级把 dev 信息收敛到一屏，且门控只需挡住一行入口。

**实现形态**：二级是 `SettingsAppState.view` 切换的**同屏行列表**，不是新 `UiScreen` —— 避免改动顶级导航环、深睡屏幕恢复白名单、`DeepSleepUiPolicyForScreen` 以及 `ui_runtime.cpp` 里各处 `screen == kSettings` 判定。切换用 `kConfig` 局部刷：标题栏（y<32）不在 `kSettingsContentRect` 内，因此标题保持「设置」，方向感由底部提示行「上下选择，确认查看，长按确认返回设置」承担（要改标题就得用 `kCommit` 整屏全刷）。

**扩展规则**：新的调试信息一律加进这六个对话框之一（数据面进 `SettingsDiagnosticsSnapshot` + `UpdateSettingsDiagnostics`，登记 `FrameSignature`）。要加第七行，必须先修订本文档并说明为何六行装不下（超过 6 行需启用现成的 selection-following 窗口逻辑）。

五个对话框（`kBattery/kStorage/kDevInfo/kDevErrors/kSleepDiag`）沿用 **只读模式**（`ui_input.cpp` 的对应输入块）：确认（短按或长按）关闭，Up/Down 无操作，无滚动，快照在打开时取一次。`kDevSync` 单独一块：短按/长按确认同样关闭，另加**双击确认 = 重下词库包**（§4.2）。驻留设置页时由既有的 60s 状态重载（`device_ui.cpp` `kStatusRefreshDelayTicks` → `LoadUiState`）被动更新。

**新增可见状态必须登记**：`view` 与 `dev_selected` 已进 `FrameSignature`；以后再加二级状态同理，否则 dedup 会吞掉重绘。

## 4. 对话框字段清单（v1）

### 4.1 Dev 信息（`kDevInfo`，≤7 行，内容带 y88..218）

- 固件版本 `WQN_FIRMWARE_VERSION` + 提交 `WQN_GIT_COMMIT`（含 `-dirty` 后缀）+ 配置期构建时间；
- 上次复位原因（`esp_reset_reason()` + `wqn::ResetReasonToString`，从 `main/diagnostics.cpp` 暴露）；
- 运行时长（`esp_timer_get_time()` 格式化为 h/m）；
- 内部堆 free / 最小剩余（`heap_caps_get_free_size` / `heap_caps_get_minimum_free_size`，`MALLOC_CAP_INTERNAL`，照 `LogAiMemory` 惯例）；
- PSRAM free / total（`SettingsDiagnosticsSnapshot` 既有字段）。

### 4.2 同步诊断（`kDevSync`，7 行）

- 上轮结果 + 成功/部分/失败计数 + `SyncSnapshot.status`；
- 词包 / 笔记 / 错题：`SyncContentPhase` 标签 + 重试次数 + `last_error`；
- 词箱 / 笔箱 / 题箱 outbox：`SyncOutboxPhase` 标签 + 重试次数 + `last_error`。

phase 标签 helper 放 `ui/diagnostics.cpp`（照 `OnlineSyncStatusLabel` 风格）。超宽用 `DrawClippedText` 截断。

**本对话框是全菜单唯一能执行动作的对话框：双击确认 = 重下词库包。**

- 动作内容（`ui_input.cpp` `RequestWordPackRedownload`，三个既有调用，无测试专用分支）：
  1. `wqn::ResetWordPackStorageCache()` 删本地 manifest + 包文件 → `!had_local_manifest`；
  2. `wqn::services::RequestContentRefresh(kWordPacks)` 置内容相 pending 并叫醒 sync task；
  3. `QueueWordReviewRefresh()` claim 刷新票据并排 bulk lane 的 `kPackSync`——与词页进屏时同一个调用。
- **为什么需要它**：健康设备上没有任何生产路径能按需造出多 MB 包写负载。词页只在 `cloud_sync_requested`（本地无 manifest / 包错误 / 无词可学）为真时才同步；`RequestSyncNow` 走 full sync 且只在服务端 revision 更新时才真正收敛；`RequestContentRefresh` 也只能在 claim 层绕过 revision 门，manifest 不变时仍走「词库无变更」分支、不下载包体。C.2 HIL 判据（测量 foreground cursor 写在包写进行中的 `queue_wait_ms`）因此无法运行。
- **为什么是双击而非长按**：长按确认在这套设置树里是「离开/返回」的通用手势（一级回首页、二级回一级），拿它触发一个会丢弃已下载内容的动作既违背惯例又易误触。双击确认在设置树里空闲（仅 AI 页用于状态栏编辑），且是刻意手势，适合破坏性动作。
- **操作序列**：开发者选项 → 同步诊断 → 双击确认 → notice「已重下词库包，去词页看」→ 去词页开始答题，包在后台全量重下。
- 成功后 `ResetWordPackStorageCache` 会打 `cleared incompatible word pack cache` 警告，动作本身打 `dev: word pack redownload requested`，两者都是 HIL 的 grep 锚点。
- 只影响词包内容相；同步轮次的其它部分不变。

### 4.3 错误记录（`kDevErrors`，≤6 行）

`CopyRecentErrors` 拷出的最近 6 条，每条一行：`HH:MM [tag] 消息`；时间不可信（`< kMinReasonableUnixTime`，照 `ui/clock.cpp` 先例）时显示 `up+Xm`。空态显示「暂无错误记录」。

### 4.4 电量原始（`kBattery`，原一级「电量详情」对话框）

adc_raw、adc_mv、battery_mv、percent、charging/full 标志，以及电压拟合公式 `(-V^2+9016V-19189000)/10000`。一级「电量」行只显示百分比。

### 4.5 存储详情（`kStorage`，原一级「存储详情」行）

Flash 大小、三内容域同步 revision、NVS 条目（带进度条）、PSRAM used（带进度条）与 free。

### 4.6 电源睡眠诊断（`kSleepDiag`，≤6 行）

`wqn::runtime::CopySleepDiagnosticEntries` 从 RTC slow memory 环拷出的最近 6 条，每条一行：`HH:MM [kind] reason`；`wall_time_sec` 不可信时退回 `up+Xm`。kind 标签（启动/策略/准入受阻/唤醒计划/回滚/提交）由 `ui/diagnostics.cpp` 的 `SleepEventKindLabel` 给出。空态显示「暂无睡眠记录」。

打开这个对话框**同时**调用 `RequestSleepDiagnosticsDump()`：coordinator 下一个 tick 把完整 64 条打到日志。屏上 6 条是电池下的唯一读法；日志版只在插 USB 时有用，且必须用 `deploy.bat` / `listen_usb.py` —— `idf.py monitor` 的 `USB_UART_CHIP_RESET` 会毁掉 RTC slow memory。

## 5. error_recorder 模块契约

新模块 `main/error_recorder.h/.cpp`（namespace `wqn`，登记 `main/CMakeLists.txt` SRCS；**不放 `ui/`**——它被各服务调用，且 `ui/` 受 M8 禁 driver 头 glob 约束）。

```cpp
struct ErrorRecord {
    uint32_t seq = 0;        // 全局单调递增；0 表示空槽
    int64_t  unix_sec = 0;   // 记录时刻 time(nullptr)
    int64_t  uptime_ms = 0;  // 记录时刻 esp_timer_get_time()/1000
    char     tag[12] = {};   // 子系统短标签，见下表
    char     msg[64] = {};   // 一句话原因（esp_err_to_name / 服务端 error_code）
};
constexpr size_t kErrorRecordDepth = 8;

// 仅任务上下文调用（当前无 ISR 调用方）。不分配、不阻塞（自旋锁）、不递归打日志。
void RecordError(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// 拷出最近 min(cap, 已有条数) 条，按 seq 从旧到新排序。仅 UI 任务调用。
size_t CopyRecentErrors(ErrorRecord* out, size_t cap);
```

**并发模型**：专用 `portMUX_TYPE` 自旋锁 + 整环拷贝，**照抄 `GetSyncSnapshot` 先例**（`sync_service.cpp`）。格式化（`vsnprintf` 到任务栈缓冲）在锁外完成，锁内只做定长拷贝与 `++seq`。

**tag 约定**（≤11 字符）：`persist`、`sync`、`cloud`、`audio`、`audio_cap`、`audio_play`、`ai`、`provision`、`rtc`、`storage`、`ui`。

**v1 埋点清单**（各一行 `RecordError(...)` + `// [dev-diag]` 热点标签；同步的 content/outbox 路径**不埋**——`last_error` 已随快照发布）：

| 位置 | 捕获内容 |
|---|---|
| `ui/persist_worker.cpp` `PersistWorkerTask` 命令结果非 ESP_OK | 各域持久化保存失败（一处覆盖全部域） |
| `services/sync_service.cpp` 控制面整轮结束为失败分支 | 整轮同步失败原因（快照未覆盖的缺口） |
| `audio_capture.cpp` 初始化失败 / 640KiB PSRAM 预留失败 / 握手超时 | 录音链路故障 |
| `services/audio_service.cpp` I2C/ES8311/I2S 创建失败 | 音频硬件故障 |
| `audio_player.cpp` DAC 初始化失败 / I2S 写失败 | 播放链路故障 |
| `ai_session.cpp` 录音启动失败 / AI 请求失败 / worker 卡死上报 | AI 会话故障 |
| `provision_manager.cpp` 凭证保存失败 / portal 启动失败 | 配网故障（状态机原子量会丢原因细节） |
| `pcf8563.cpp` 时钟完整性丢失（VL 位）/ 写校验失败 | 墙钟不可信 |
| `device_ui.cpp` 按键初始化/任务启动/EPD 初始化失败 | UI 底座故障 |

## 6. 纪律基线（所有 Dev 菜单代码必须遵守）

1. **所有权**：不新增任务、队列、信号量；不动态分配。跨任务发布的数据一律定长 char 数组 POD（照 `SyncSnapshot` / `ConnectivitySnapshot.active_ssid` 先例）；`std::string` 仅限 UI 任务私有的 `SettingsAppState` / 快照结构（照 `SettingsDiagnosticsSnapshot` 既有 5 个 string 字段先例）。
2. **生命周期**：错误环与全部模块状态为静态存储期，无 init/teardown 顺序依赖；快照在对话框打开时按值拷贝，UI 状态不持有任何指向服务层对象的指针/引用。
3. **并发**：短临界区用 `portMUX_TYPE` 自旋锁（照 `GetSyncSnapshot`）；**不用**阻塞互斥锁；载荷先备好、锁内只拷贝（AGENTS.md §4.7）。
4. **墨水屏**：只用 `DrawSettingsDialogBox` / `DrawClippedText` / `DrawWrappedText` / `DrawCenteredText` / `DrawProgressBar` 与 `ESP_RETURN_ON_ERROR` 包装；刷新走既有 kConfig/kSelection 语义，**不新增 RefreshSchedule 值**；对话框内可见的新字段必须登记进 `FrameSignature`（`ui/ui_refresh.cpp` settings 段），否则 60s 驻留重载后 dedup 会吞掉重绘。
5. **构建门禁**：新 .cpp 登记 SRCS；每次构建必须 0 警告 + `wqn_architecture_check`（M8）通过；条件编译用裸 `#if CONFIG_WQN_...`（仓库主流风格），整文件门控照 `audio_selftest.cpp` 先例。
6. **行索引单源**：`kSettingsItemCount`、`kDevItemCount` 与各行索引常量（`kSettingsRow*`、`kDevRow*`）唯一存在于 `ui_model.h`（namespace `wqn`），`ui_internal.h` 引用之；任何代码不得再硬编码行号字面量（`ui_model.cpp` 的 `ClampUiSelection`/`HandleUiInput` 历史上硬编码 9 是本规范的由来）。`ClampUiSelection` 必须钳制 `dev_selected`，并在 flag 关闭时强制 `view = kRoot`。

## 7. 验证流程

1. **双配置构建**：default n（发布等价，Dev 行不可见）与 sdkconfig 置 y（三行可用）都要 0 警告 + M8 通过；对比 bin 大小并记录。
2. **上机冒烟**（需硬件）：逐个进入三个对话框并确认关闭；制造一次可观测失败（如断开 WiFi 触发同步失败）验证错误记录；驻留设置页 ≥60s 验证重载后无吞帧、无残影。
3. **发布演练**：按 `RELEASE_CHECKLIST.md` 走一遍 §1 的 flag 校验步。

## 8. 已知局限（有意为之）

- `WQN_GIT_COMMIT` / 构建时间为 **configure 期快照**：同一构建目录内 commit 后需重新 configure 才刷新；拿不到 git 时回退 `"unknown"`。
- 错误环不持久化：重启即失（崩溃类信息由「上次复位原因」兜底）；要做持久化先修订本文档并评估 NVS 写入频率。
- 电源睡眠诊断屏上只显示最近 6 条，完整 64 条要靠 USB 日志（见 §4.6）；环本身跨深睡保留，但 `USB_UART_CHIP_RESET` 会清空 RTC slow memory。
- `WQN_FIRMWARE_VERSION` 保持手写、**不掺 hash**：它进同步 `image_id`（`sync_service.cpp`）与刷新签名（`ui/ui_refresh.cpp`），每次提交变化会污染这两处。
