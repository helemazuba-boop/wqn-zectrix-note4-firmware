# WQN ZecTrix Note4 固件

本目录是 WQN ZecTrix Note4 伴随设备的 ESP-IDF 固件工程。目标硬件是一台基于 ESP32-S3 的电子纸学习终端，板级配置对应 `zectrix-s3-epaper-4.2`。

固件负责把实体设备接入 WQN 云端，同时避免把模型服务密钥、Supabase 权限、用户私密数据放进微控制器。设备侧处理本地按键、电子纸渲染、音频采集、轻量缓存和 WQN ESP32 API 通信；ASR、LLM、Supabase 访问、笔记本权限和 AI function call 执行都属于云端职责。

## 硬件目标

- 主控：ESP32-S3。
- Flash：16 MiB，分区布局对齐 ZecTrix Note4 基线。
- 屏幕：400 x 300 电子纸屏，使用 Note4 控制器命令集。
- 输入：实体按键，用于页面导航、选择、计时器控制和 AI 按住说话。
- 音频：ES8311/I2S 麦克风链路用于录音；识别和模型处理由 WQN 服务器完成。
- 电源域：固件显式控制板级保持、电纸屏电源轨、音频电源、功放、NFC 和低功耗空闲行为。

## 固件职责

- 板级启动和诊断：安全 GPIO 初始化、芯片/Flash/PSRAM 诊断、复位原因、WiFi MAC、电池诊断和串口日志。
- WQN 云端连接：可选 WiFi STA、配对 token 存储、脱敏 token 日志、待复习题目同步、题目索引刷新、复习结果重试上传、Todo 同步和 AI 请求上传。
- 电子纸 UI：首页/时间、倒计时、番茄钟、缓存题目、Todo、单词/笔记学习流程，以及 AI 会话状态展示。
- 电子纸刷新控制：全刷、本地局部窗口刷新、刷新冷却和空闲断电。
- AI 音频路径：长按确认键开始录音，松手上传 16 kHz mono PCM 到 WQN 服务器，然后显示转写、回复和动作摘要。
- 本地存储：使用 NVS 保存配对 token 和少量设备缓存，用于离线展示和失败重试。


默认 WQN ESP32 API 地址是：

```txt
https://wqn.helema.cn/api/esp32
```

开发构建可以覆盖 API 地址：

```powershell
idf.py -B build-ai-local-s3 -DWQN_API_BASE=https://your-host.example.com/api/esp32 build
```

## 主要配置开关

通过 `idf.py menuconfig` 进入 `WQN firmware` 配置：

- `CONFIG_WQN_WIFI_STA_ENABLE`：启用 WiFi STA 和 WQN API 调用。
- `CONFIG_WQN_WIFI_SSID` / `CONFIG_WQN_WIFI_PASSWORD`：本地开发 WiFi 凭据，写入被忽略的 `sdkconfig`。
- `CONFIG_WQN_EPD_UI_ENABLE`：启用电子纸 UI 和按键驱动的设备应用。
- `CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE`：启用 Note4 本地局部窗口刷新。
- `CONFIG_WQN_EPD_IDLE_POWER_OFF_MS`：UI 空闲后关闭电纸屏电源轨以节省电量。
- `CONFIG_WQN_DEEP_SLEEP_ENABLE`：可选的实验性深睡路径。
- `CONFIG_WQN_AI_ENABLE`：启用 AI 固件模块；provider 密钥仍只存在云端。
- `CONFIG_WQN_AUDIO_SELFTEST_ENABLE`：启动时采集并打印音频统计，不上传音频。
