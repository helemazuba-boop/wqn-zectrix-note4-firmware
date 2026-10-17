# 设置 UI 重写：B 左对齐

2026-10-18，本地分支 `feat/ui-rewrite`。依据用户确认的 `/home/unknow/projects/doc/note4_settings_wqn_brand.html` 与随后“全部移除常驻底部操作说明栏”的决定。当前已接入固件，用户已批准分批本地提交；尚未推送或由本轮刷写，修正后的真机验收仍待完成。

## 页面与视觉

- 根目录保留 11 项常规设置、可选开发者入口开启后 12 项；开发者仍为六项。两级菜单均使用五行滚动窗口、细线、反白编号、右侧当前位置。2026-10-18 真机反馈后，列表主文字提升为原生 20px、值为 16px，编号列收窄；详情与列表外边距统一为 10px，状态栏使用全宽 y=27 的公共坐标。
- 旧中央弹框改为全页。自动同步、音量、图片渲染、AI 翻页与默认词库分别显示“已保存勾选”和“当前候选粗线”，浏览不会立即改变已保存值。
- 一级电量打开 `kBatterySummary`；原始 ADC、电压与公式仍在开发者 `kBattery`。关于设备替代原固件版本 notice，品牌为浏览器同源 W 图形 + `rongNoteBook`，型号 Note4，显示实际固件与 ESP-IDF 版本。
- WiFi 显示真实主 / 备用 SSID，提供“开始／重新配网”和“设备配对”两项；配对页区分已配对、未联网、等待配对码与实际配对码。无原型配对码或示例 SSID进入生产代码。
- 设备端配网引导显示 owner 发布的真实 AP 名和 `192.168.4.1`；覆盖启动、等待手机、保存完成与失败重试。成功文案是“配置已保存”，不将凭据保存等同于联网成功。手机端配网页未在此任务重写。

活动配网仍通过现有手机网页的完成 / 退出流程结束；本轮没有新增设备按键停止 AP 的服务接口。失败结果页支持确认重试或长按返回 WiFi 管理。
- 电量与容量增加有效性标记，未知值显示 `--`，不以零值冒充读数。满电文案需同时有外部供电证据，避免拔线后仅凭 STDBY 引脚误判。NVS 条目、Flash 容量、PSRAM 内存和内容 revision 分别表达。
- 所有设置画面取消常驻底部操作说明栏。保存 / 失败等反馈位于标题下的说明区。诊断页底部的公式、最小堆与 revision 是实际数据，不是操作提示栏。

主实现是 `main/ui/page_settings.cpp`，几何集中在 `ui_layout.h` 的 Settings 段。全页合成后仍调用原 `RefreshStableRegion` / `RefreshFrame`，不直接访问 SPI、BUSY、EPD 电源或面板波形。完整帧合成不等于每次强制全刷；快捷菜单仍由既有合成路径处理。

## 输入、保存与后台状态

- 继续复用五类设置的 persist worker 与 operation-id ACK。提交成功先显示保存中，匹配 ACK 才更新已保存值。失败保留旧值和重试候选；长按取消清除未提交候选；在途 ACK 不会被重复确认重置。
- 快速双击上下按逐次移动处理。跨设置页切换沿用全局导航 reducer 的 held / blocked / double-confirm 隔离，长按松开或尾随确认不执行下一页操作。
- 所有 `OpenSettingsDialog` 路径统一检查 worker busy、RAM 暂存与三个域的 `kPersisting` 窗口，再读取设置快照，包含 WiFi 与配对页之间的返回。
- 60 秒状态重载只更新已保存值，不改正在浏览的自动同步、音量或图片选项焦点。
- 词库来自 mounted catalog。没有可用清单时显示未同步 / 同步中 / 失败；确认可通过现有异步内容同步服务请求重试。后台安装新清单后按 ID 保留候选，已删除的候选退回全部词库。刷新签名只遍历可见五条，避免每次渲染签名随整个目录增长。
- 同步诊断单击确认等待 300ms 双击窗口后返回；双击请求原词库包重下动作，长按立即返回。超时在输入队列排空后处理，按生产时间识别已排队的第二击。重下动作保留持久化窗口保护。
- `DispatchSettingsTick` 复用现有 UI 循环，观察词库任务与配网 owner 状态，并处理本地确认窗口。无新增任务、网络轮询、用户活动发布或深睡唤醒源。配网 active 期间不执行被遮住的根目录按键动作；结束后按真实结果恢复设置画面。
- 手动同步仍由 SyncService 的 demand / readiness 机制处理。没有照搬 HTML 中“WiFi 当前离线则拒绝”的演示判断，以免设备的正常无线空闲关闭状态阻止手动联网。
- 恢复出厂与关机继续调用原有路径，短按取消、长按确认，保存与暂存保护保留；未修改电源锁存、睡眠、NVS / SPIFFS 协议或关机提示生成器。

## 原生字库

`main/ui/ui_fonts.c` 是离线生成、未压缩 1bpp 数据；不引入 LVGL widget/runtime 字体引擎。常规 16px 继续用已有通用字库的原生 glyph；设置页不调用字号缩放函数。20px 离线字模补齐设置列表 / 单选项，12px 仅用于必要辅助信息。

| 资源 | 覆盖 | 纯位图字节 |
| --- | --- | ---: |
| 思源黑体 Normal 12px | 7542 字符，包含完整 GB2312、ASCII 与补充符号 | 112483 |
| 思源黑体 Normal 20px | 7542 字符，同上 | 309436 |
| 思源黑体 Normal 24px | 同上 | 446623 |
| Noto Sans Bold 24px | 95 个 ASCII，品牌 | 2188 |
| Noto Sans Bold 32px | 95 个 ASCII，型号与配对码 | 3903 |
| Noto Sans Bold 48px | 95 个 ASCII，电量数字 | 8737 |

纯位图共 883370 B，另有字形描述符与映射表；最终 Flash 以链接产物为准。W 图形由本地 SVG 的相同八条浏览器路径生成 28px 掩码，不依赖在线资源。源字体已在工作区存在，本轮未下载新字体，Medium 仍是未来字重对照候选。12 / 20 / 24px 字库已为主页迁移准备；主页的原生字号与上移布局单独提交。动态音标和单词页迁移仍为待办；随固件提交的资源清单见 [UI_FONT_RESOURCES.md](UI_FONT_RESOURCES.md)。

再生成要求 Pillow 11.3.0、fonttools 4.67.0。生成器支持指定源文件，验证 SHA-256，拒绝误用不同字重或版本；源许可证随 `tools/ui/font-licenses` 保留。

```sh
/tmp/note4-ui-font-builder/bin/python tools/ui/generate_ui_fonts.py --check
python3 scripts/test_ui_settings.py --render-dir /home/unknow/projects/doc/assets/ui-layout-device-feedback/settings
/tmp/note4-ui-font-builder/bin/python tools/ui/render_settings_preview.py --input /home/unknow/projects/doc/assets/ui-layout-device-feedback/settings
```

## 验证与可视化

共享产物：`/home/unknow/projects/doc/assets/ui-layout-device-feedback/settings/`。`overview.png`、`details.png`、`states.png` 来自实际生产绘制函数与真实字模，输入和 I/O 是 host 替身，**不是设备截图**。原生画面为 400×300、1bpp，导出时未缩放设备像素。

- `scripts/test_ui_settings.py`：Dev 关闭 202、开启 217，共 **419 项断言通过**；覆盖原生字模、与其他页面一致的全宽状态栏、字号 / 边距、动态单行文本、根目录 / 二级滚动、已保存值与焦点、全部详情、未知电量、外部电源与满电、词库状态 / 目录更新、真实输入桥接、取消 / ACK / 失败 / 忙碌、跨页尾随、单 / 双击、重载候选、持久化与危险动作保护。
- 临时源码变体分别破坏取消、已保存勾选和电量有效性签名，三者均被断言检出。正式源码保持完整，未将故障变体写入仓库。该变体检查属于最初设置重写轮次，记录保留在此前 V1 验证报告中。
- 快捷菜单 1308、主页 88、display submit 96、栈检查 17、单词 start / cursor / RAM batch 26 / 34 / 32，通过；M8 fixture 实际运行 1 项并通过。
- ESP-IDF Dev 开 / 关两配置构建均需零警告、M8 与编译 UI 栈门禁通过；结果、产物大小和 SHA-256 见 `/home/unknow/projects/doc/assets/ui-layout-device-feedback/verification.json`。临时切换配置后恢复原 `sdkconfig`，最终产物为原 Dev 开启配置。
- `git -c core.whitespace=cr-at-eol diff --check` 干净。构建日志位于 `/tmp/note4-ui-feedback-dev-off-build.log` 与 `/tmp/note4-ui-feedback-dev-on-build.log`。

设备反馈与前后对照记录见 `/home/unknow/projects/doc/1018-ui-device-layout-feedback.md`；最初 V1 回放及验证报告保留为历史。

上述 host / build 证据不证明 EPD 物理响应、RTOS 并发、真实配网或设备待机功耗。没有刷机、HIL、推送、PR 或发布结论。

## 待设备验收

1. 两级菜单逐项进入、跨五行滚动、快速双击上下；原尺寸字形、截断、省略与焦点可读，无底部操作栏。
2. 所有选择项的取消、保存中重复确认、失败后重开 / 重试；保存中开启全局菜单并返回，确认尾随不触发其他动作。
3. 选择过程中驻留超过 60 秒、切换充电 / 满电 / 拔线、容量未知与变化，检查刷新去重、候选保持、残影与面板延迟。
4. 使用真实主 / 备用 SSID、空网络、不同配对状态与真实配对码；配网启动失败 / 完成、AP 名发布及 portal 结束后的页面恢复，确认不会操作遮住的设置列表。
5. 词库未同步、下载中、失败重试、多目录 / 长名、云端增删与当前候选保持；同步诊断单双击边界及排队输入。
6. 单词 RAM 暂存、三域 Prepare / reserve / apply 窗口中尝试打开详情、切词库、重下、重置与关机，确认原保护保持。真机重置 / 关机仍属于独立、明确安排的设备验收。
7. 普通待机、深睡 / 唤醒、快捷菜单与设置详情的共存；记录实际运行时栈余量、刷新与功耗，不把静态栈预算视为运行时 HWM。
