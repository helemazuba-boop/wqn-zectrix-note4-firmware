# ZecTrix Note4 固件 Code Review 计划 & 详细评审报告

## 执行者：Gemini (Antigravity AI)
## 评审日期：2026-06-13
## 对照官方固件版本：Release V1.0 (ESP32-S3 e-paper 4.2)

---

## 一、硬件/引脚冲突问题（高优先级 🔴）

### 1.1 I2C 引脚冲突
**问题**：
- 当前固件使用 `I2C_1`（GPIO 18/20），但这两引脚被用于按键和 USB
- 导致 USB 无法识别、I2C 总线被干扰

**正确配置应为**：
```
I2C_0: SDA=GPIO 47, SCL=GPIO 48
```

**[x] Gemini 评审笔记**：
* **官方固件对照**：官方逆向代码（`config.h` 与 `board_init`）显示，主板只有一条 I2C 总线（`I2C_NUM_0`），引脚分配为 **SDA=GPIO 47**，**SCL=GPIO 48**。此总线上并联挂载了 **ES8311 音频 Codec**、**PCF8563 RTC** 和 **NFC 芯片**。
* **当前代码问题**：WQN 固件在最近的一次提交中错误地在 `board_zectrix_note4.cpp` 中将电源与 RTC 绑在 `I2C_NUM_1`，并指定 SDA=GPIO 20，SCL=GPIO 18。
* **影响机理**：
  1. GPIO 20 是 ESP32-S3 的原生 USB D+ 引脚。配置为 I2C SDA 会强行夺取 USB-Serial-JTAG 控制器对该引脚的控制权，导致连接电脑时产生 `PermissionError`，设备完全无法连接 PC。
  2. GPIO 18 是物理按键 `KEY_DET/PGDN`（下翻页/电源键）。配置为 I2C SCL 导致 I2C 传输电平被按键直接拉低，按下按键即拉死 I2C 总线引发断言崩溃重启；同时，I2C 脉冲会导致按键中断高频误触发（中断暴风）。
* **建议修复**：
  - 彻底废除 `I2C_NUM_1` 在 GPIO 18/20 上的初始化。
  - 将所有 I2C 外设（PCF8563, NFC, ES8311）的初始化收拢至单一的 `I2C_NUM_0` 上，物理引脚统一为 SDA=GPIO 47，SCL=GPIO 48。

---

### 1.2 USB 引脚占用
**问题**：
- GPIO 19 → USB_DN (D-)
- GPIO 20 → USB_DP (D+)
- 当前代码可能将其配置为 GPIO 输出

**[x] Gemini 评审笔记**：
* **官方固件对照**：官方固件从未对 GPIO 19 和 20 进行任何 GPIO 属性的初始化，这两个引脚被严格保留给芯片内置的 USB 硬件接口使用。
* **当前代码问题**：WQN 固件在 `button_input.cpp` 和 `board_zectrix_note4.cpp` 中将翻页下键 `kPageDownAndPowerDetect` 修改为了 `GPIO_NUM_19`，并在 `board_zectrix_note4.cpp` 中将 `kI2cSda` 设为了 `GPIO_NUM_20`。
* **影响机理**：GPIO 19 被配置为带上拉的 GPIO 输入，GPIO 20 被配置为 I2C SDA。这种配置完全破坏了 USB D-/D+ 差分信号线，导致电脑无法识别开发板。
* **建议修复**：
  - 撤销对 GPIO 19 和 20 的所有手动 GPIO 硬件配置，将其彻底释放，交还给 ESP32-S3 内置的 USB 控制器。

---

### 1.3 按键引脚配置
**正确配置**：
| 按键 | GPIO |
|------|------|
| KEY_ENTER (确认) | GPIO 0 |
| KEY_DET (下翻/电源检测) | GPIO 18 |
| KEY_PGUP (上翻) | GPIO 39 |

**[x] Gemini 评审笔记**：
* **官方固件对照**：官方原理图与逆向结果均证实了以下配置：确认键（KEY_ENTER，对应 GPIO 0/BOOT）、上翻页键（KEY_PGUP，对应 GPIO 39）、下翻页与电源自锁检测复用键（KEY_DET，对应 GPIO 18）。
* **当前代码问题**：当前固件把 `kDownPowerPin` / `kPageDownAndPowerDetect` 错配为 GPIO 19，且 GPIO 18 被 I2C SCL 占用。
* **建议修复**：
  - 将 `button_input.cpp` 中的 `kDownPowerPin` 修改回 `GPIO_NUM_18`。
  - 将 `board_zectrix_note4.cpp` 中的 `kPageDownAndPowerDetect` 修改回 `GPIO_NUM_18`。
  - 确保 GPIO 18 仅配置为带上拉的输入模式。

---

## 二、电源管理逻辑审查

### 2.1 电池耗尽关机逻辑与“按键关机”Bug（🔴致命级别）
**当前问题**：未检测 USB 供电时可能误关机，且**按下下键会导致设备立即黑屏死机**。

**[x] Gemini 评审笔记**：
* **详细代码分析**：
  1. 在 `power_manager.cpp` 中，引脚错配为：
     * `kDownPowerWake = GPIO_NUM_19` （错误：GPIO 19 实际上是 USB D-）
     * `kUsbPowerDetect = GPIO_NUM_18` （错误：GPIO 18 实际上是下翻页/电源按键 `KEY_DET`）
  2. `IsUsbPowered()` 函数被实现为：
     ```cpp
     bool IsUsbPowered() { return gpio_get_level(kUsbPowerDetect) == 1; }
     ```
     即读取 GPIO 18 的电平。由于 GPIO 18 接按键且有外部上拉电阻，在**按键未按下时**电平为 1。此时 `IsUsbPowered()` 会误报为 `true`（系统误认为一直连着 USB）。
  3. **关键 Bug 触发链**：
     * 由于 WQN 的初始化流程中，`wqn::InitPowerHardware()` **从未在任何地方被调用**，因此 ADC 驱动（`g_adc_initialized`）根本没有初始化。
     * 这导致 `GetBatteryVoltageMv()` 永远返回 `0`，`IsBatteryVeryLow()` 永远返回 `true`（0V <= 3430mV）。
     * 当用户**没有按下下键**时，`IsUsbPowered()` 返回 `true`，`IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered()` 为 `false`，系统能够勉强运行。
     * 当用户**按下下翻页键**时，GPIO 18 被拉低，`IsUsbPowered()` 瞬间变成 `false`。
     * 此时，电量耗尽条件 `IsBatteryVeryLow() && !IsCharging() && !IsUsbPowered()` 立即变成 `true`！
     * 系统在按键按下的瞬间，立刻执行了 `ShutdownForBatteryDepleted()` 紧急关机程序：切断 EPD/音频供电，拉低自锁引脚 `kBoardPowerLatch`（GPIO 17），并使芯片进入 deep sleep！
* **导致后果**：
  - 用户按下下键切换页面时，设备直接被拉入深度睡眠。由于屏幕电轨（GPIO 6）被瞬间切断，电子纸像素无法完成偏转，导致屏幕画面卡死，并留下一片模糊的“鬼影”和“卡死”现象。
  - 在有 USB 供电时，释放自锁引脚不会断电，芯片进入 sleep 后会因为 USB 差分线上的状态变化被立刻重新唤醒复位，导致了无限重启（Boot Loop）。
* **建议修复**：
  - 在 `board_zectrix_note4.cpp` 的 `InitZectrixNote4SafePins()` 阶段**必须显式调用** `InitPowerHardware()`（使用 `I2C_NUM_0` 挂载在 47/48 引脚上），使 ADC 正常工作。
  - 在 `power_manager.cpp` 中将 `kUsbPowerDetect` 废除，因为开发板没有独立的 USB Power 检测 GPIO。USB 供电的判定应直接与充电引脚状态 `IsCharging()` 或 `IsFullyCharged()` 绑定。
  - 彻底更正 `kDownPowerWake` 为 `GPIO_NUM_18`。

---

### 2.2 GPIO 引脚定义一致性
**需检查**：
```cpp
kChargeDetect = GPIO_NUM_2   // CHRG_L (充电中，低有效)
kChargeFull   = GPIO_NUM_1   // STDBY_H (充满，高有效)
```

**[x] Gemini 评审笔记**：
* **官方固件对照**：官方引脚与逆向代码确认，GPIO 2 连接至充电芯片 IP2312 的 `CHRG` 脚，充电时被拉低（Low-active）；GPIO 1 连接至 IP2312 的 `STDBY` 脚，充满时输出高电平（High-active）。
* **当前代码问题**：WQN 代码中的引脚值和电平判断与原理图是一致的。但因为未调用 `InitPowerHardware()`，GPIO 1 和 2 的输入模式配置可能在某些流程中被跳过。
* **建议修复**：确保在板级初始化时，这两个状态脚被正确配为输入，且不与任何 I2C 逻辑混淆。

---

### 2.3 电源锁存引脚
**关键引脚**：
- `kBoardPowerLatch = GPIO_NUM_17` - 电源锁存控制

**[x] Gemini 评审笔记**：
* **官方固件对照**：开机后在初始化初期必须立刻将 GPIO 17 拉高锁死电源轨，松开电源键后系统即可持续运行。关机时将 GPIO 17 拉低即可切断主 3.3V 稳压器，达到彻底断电。
* **当前代码问题**：逻辑本身无误，但必须保证该逻辑不受误触发的 `ShutdownForBatteryDepleted()` 干扰。

---

## 三、I2C 总线与外设

### 3.1 I2C 总线配置与重复初始化 Panic（🔴高优先级）
**总线拓扑**（正确配置）：
```
I2C_0 (SDA=GPIO 47, SCL=GPIO 48)
├── PCF8563 RTC      (地址 0x51)
├── ST25DV NFC       (地址 0x55)
└── ES8311 Audio     (地址 0x18)
```

**[x] Gemini 评审笔记**：
* **当前代码问题**：
  1. `pcf8563.cpp` 中自行调用了 `i2c_new_master_bus` 初始化 `I2C_NUM_1` 或 `I2C_NUM_0` 端口。
  2. `audio_selftest.cpp` 和 `audio_capture.cpp` 也在各自调用 `i2c_new_master_bus` 独占创建 `I2C_NUM_0` 主线。
  3. 当系统同时启用音频自检（`CONFIG_WQN_AUDIO_SELFTEST_ENABLE=y`）和 RTC 配置时，第二个调用 `i2c_new_master_bus` 的组件会抛出 `ESP_ERR_INVALID_STATE`，被 `ESP_ERROR_CHECK` 捕获后直接引发系统 Panic 崩溃重启。
* **建议修复**：
  - 在 `board_zectrix_note4.cpp` 中全局创建唯一的 `i2c_master_bus_handle_t` 总线句柄。
  - 屏蔽所有外设组件文件中的 `i2c_new_master_bus` 总线配置代码。
  - 重构外设初始化函数，使其接受该全局总线句柄，并通过 `i2c_master_bus_add_device` 共享总线。

---

### 3.2 PCF8563 RTC
**[x] Gemini 评审笔记**：
* **驱动完整性**：新加入的 `pcf8563.cpp` 实现了 BCD 时间解码和 1Hz 定时唤醒脉冲逻辑，这与官方 `FUN_4203d3d0` 的寄存器写入时序是一致的。但需要将总线接入修复。

---

### 3.3 ST25DV NFC
**[x] Gemini 评审笔记**：
* **功耗与控制**：NFC (GT23SC6699) I2C 地址为 `0x55`。在休眠前，应当拉低 `kNfcPower`（GPIO 21）以切断 NFC 芯片的 VCC 供电，并在唤醒时重新拉高。当前 WQN 固件在此处缺少电源轨的动态控制。

---

## 四、GPIO 使用清单

### 4.1 所有 GPIO 定义

| GPIO | 当前用途 | 硬件连接 | 状态 | 备注 |
|------|---------|---------|------|------|
| 0 | KEY_ENTER | 确认键 / BOOT | ✓ OK | 支持深度睡眠 ext1 唤醒（低电平有效） |
| 1 | STDBY_H | 充满检测 | ✓ OK | 输入，充电芯片 IP2312 充满指示 |
| 2 | CHRG_L | 充电检测 | ✓ OK | 输入，充电芯片 IP2312 充电中指示 |
| 3 | LED | 系统红色指示灯 | ✓ OK | 低电平点亮，高电平熄灭 |
| 4 | BAT_ADC | 电池电压采样 | ✓ OK | 模拟通道，对应 ADC1 通道 3 |
| 5 | RTC_INT | RTC 中断唤醒 | ✓ OK | 输入，支持深度睡眠唤醒（带上拉） |
| 6 | EPD_POWER | 墨水屏供电开关 | ✓ OK | 高电平使能 3V3_EPD 供电轨道 |
| 7 | NFC_FD | NFC 字段检测 | ✓ OK | 输入，低电平有效 |
| 8 | EPD_BUSY | 墨水屏忙状态 | ✓ OK | 输入，低电平表示忙碌 |
| 9 | EPD_RESET | 墨水屏复位脚 | ✓ OK | 输出，低电平复位 |
| 10 | EPD_DC | 墨水屏数据/命令 | ✓ OK | 输出控制脚 |
| 11 | EPD_CS | 墨水屏 SPI 片选 | ✓ OK | 输出，低电平片选有效 |
| 12 | EPD_SCK | 墨水屏 SPI 时钟 | ✓ OK | SPI3 物理时钟线 |
| 13 | EPD_MOSI | 墨水屏 SPI 数据 | ✓ OK | SPI3 物理数据线 |
| 17 | kBoardPowerLatch | 电源自锁控制 | ✓ OK | 高电平锁定系统供电，低电平彻底切断关机 |
| 18 | KEY_DET | 下翻/电源键检测 | ✗ 严重冲突 | 被 `power_manager` 错配为 USB 电源检测，同时被 I2C_1 SCL 占用。必须恢复为按键检测输入 |
| 19 | USB_DN | USB 差分 D- | ✗ 严重冲突 | 被错误配成了下翻页按键，导致 USB 掉线 |
| 20 | USB_DP | USB 差分 D+ | ✗ 严重冲突 | 被错误配成了 I2C_1 SDA，导致 USB 掉线 |
| 21 | NFC_POWER | NFC 电源控制 | ✓ OK | 输出，高电平导通 NFC 供电 |
| 42 | AUDIO_POWER | 音频 DAC 电源 | ✓ OK | 输出，高电平导通 ES8311 供电 |
| 46 | AUDIO_AMP | 音频功放 PA 使能 | ✓ OK | 输出，高电平启用功放，播放停止时必须拉低静音 |
| 47 | I2C_SDA | 共享 I2C 总线 SDA | ✓ OK | 连接 RTC, NFC, Audio |
| 48 | I2C_SCL | 共享 I2C 总线 SCL | ✓ OK | 连接 RTC, NFC, Audio |

---

## 五、功耗管理审查

### 5.1 深度睡眠流程
**[x] Gemini 评审笔记**：
* 官方在进入睡眠前会通过 `gpio_hold_en` 锁定 GPIO 17 (`kBoardPowerLatch`) 的电平为高，保证芯片休眠时外围 DCDC 电路不掉电。同时需要将 GPIO 21 (NFC), GPIO 42 (Audio), GPIO 46 (AMP) 均拉低以节省待机功耗。

### 5.2 墨水屏功耗
**[x] Gemini 评审笔记**：
* 官方刷屏结束 1.5 秒后调用 `PowerOffEpd()`，拉低 GPIO 6 切断电轨。
* 漏电防护：切断 GPIO 6 前必须向墨水屏发送 Deep Sleep 指令（`0x07`，数据 `0xA5`），并在此后将 SPI 的片选（CS）、时钟（SCK）、数据（MOSI）等引脚配置为低电平或输入，防止电压通过信号线向断电的屏幕模组倒灌漏电。

---

## 结论与建议变更列表

### 问题 #1: 下翻页按键与“电量耗尽关机”逻辑产生致命级连锁反应
* **严重程度**: 🔴致命
* **影响**: 按下下键切换页面时，设备直接被关电并进入 deep sleep，导致墨水屏刷新中断卡死并留下严重“鬼影”；在插 USB 时表现为反复重启。
* **位置**: [power_manager.cpp:L46](file:///d:/projects/wqn-zectrix-note4-firmware/firmware/wqn-zectrix-note4/main/power_manager.cpp#L46)、[power_manager.cpp:L57](file:///d:/projects/wqn-zectrix-note4-firmware/firmware/wqn-zectrix-note4/main/power_manager.cpp#L57)、[board_zectrix_note4.cpp:L60](file:///d:/projects/wqn-zectrix-note4-firmware/firmware/wqn-zectrix-note4/main/board_zectrix_note4.cpp#L60)
* **问题描述**:
  由于未初始化 ADC 导致电量始终读为 0V，并且将下翻页键 GPIO 18 错配为 `kUsbPowerDetect`（USB 拔插检测），导致平时 `IsUsbPowered()` 恒为真，而按下按键时该函数返回假，瞬间触发低电量保护关机逻辑。
* **建议修复**:
  1. 废除 `kUsbPowerDetect` 这一无物理引脚对应的定义。
  2. 在 `IsUsbPowered()` 逻辑中直接通过读取充电引脚状态 `IsCharging() || IsFullyCharged()` 来识别是否有 USB 供电。
  3. 将 `kDownPowerWake` 正确修改为 `GPIO_NUM_18`。
  4. 在 `board_zectrix_note4.cpp` 中开机时，调用 `InitPowerHardware()`（基于 47/48 的 I2C_0 总线）以正常激活 ADC。

### 问题 #2: I2C_0 端口抢占冲突，导致程序在音频自检时 Panic 重启
* **严重程度**: 🔴高
* **影响**: 开机音频自检被激活时，系统抛出端口占用错误并死机重启。
* **位置**: [audio_selftest.cpp:L161](file:///d:/projects/wqn-zectrix-note4-firmware/firmware/wqn-zectrix-note4/main/audio_selftest.cpp#L161)、[audio_capture.cpp:L213](file:///d:/projects/wqn-zectrix-note4-firmware/firmware/wqn-zectrix-note4/main/audio_capture.cpp#L213)
* **问题描述**:
  音频与 RTC 组件各自试图以 `i2c_new_master_bus` 独占初始化同一个 `I2C_NUM_0` 端口。
* **建议修复**:
  1. 在 `board_zectrix_note4.cpp` 中实现全局的 I2C_0 总线初始化并对外提供获取总线句柄的接口。
  2. 修改音频驱动，将其作为设备挂载到共享的板级 I2C 总线，移除多余的 I2C 总线创建代码。
