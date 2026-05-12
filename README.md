# wqn-zectrix-note4-firmware

WQN ZecTrix Note4 Firmware 是“错题本”（WQN）面向 ZecTrix Note4 设备的专用固件项目。目标是把这块 4.2 英寸墨水屏学习设备接入 WQN Web 后端，让学生可以在设备上同步待复习错题、离线完成复习标记，并在联网后把复习结果上传回 WQN。

## 项目目标

- 支持 ZecTrix Note4 / `zectrix-s3-epaper-4.2` 硬件。
- 通过 WQN ESP32 API 完成账号配对、token 保存、错题同步和复习结果上传。
- 第一阶段以 headless 串口固件为主，优先验证网络和数据闭环。
- 数据闭环稳定后，再接入 400 x 300 墨水屏 UI、实体按键、配网和低功耗。
- 保持固件中不包含 Supabase service role key、WiFi 密码、真实 token 或用户数据。

## 当前状态

当前固件位于 [`firmware/wqn-zectrix-note4/`](firmware/wqn-zectrix-note4/)。

已经具备：

- ESP-IDF 项目骨架，目标芯片按公开 ZecTrix 示例暂定为 ESP32-S3。
- 16 MB Flash 分区表，贴近官方 `zectrix` 示例固件基线。
- ZecTrix Note4 安全引脚初始化，默认关闭墨水屏、音频、功放和 NFC 电源。
- 启动诊断日志，包括固件版本、板卡标识、Flash、MAC 和 reset reason。
- NVS access token 存取和日志脱敏。
- WQN ESP32 API JSON fixture 自检。

还未完成：

- WiFi station / 配网流程。
- `/api/esp32/poll` 真实配对请求。
- Bearer token 认证请求、`/sync`、`/problems` 和 `/review-complete`。
- 本地题目缓存、pending 复习结果队列和墨水屏 UI。

## 开发顺序

建议按这个顺序推进，避免把硬件驱动、网络和业务协议混在一起调试：

1. 通过 `idf.py build` 验证 ESP-IDF 构建环境。
2. 实现 WiFi station 最小连接。
3. 实现 `/poll` 配对，保存 64 位十六进制 access token。
4. 实现 `/sync` 获取待复习题目 ID，401 时清 token 并回到配对。
5. 实现 `/problems` 拉取题目详情，并缓存到本地。
6. 实现串口复习流程和 pending 队列。
7. 实现 `/review-complete` 上传结果，成功后再删除 pending。
8. 接入墨水屏显示和实体按键。
9. 最后再做 AP 配网、低功耗和 OTA。

## 构建

先激活 ESP-IDF 环境，然后运行：

```powershell
cd D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4
idf.py set-target esp32s3
idf.py build
```

开发环境需要显式指定 WQN API 地址。ESP32 不能访问电脑上的 `localhost`，请使用局域网 IP、域名或隧道地址：

```powershell
idf.py -DWQN_API_BASE=https://your-host.example.com/api/esp32 build
```

## 安全提醒

不要提交或烧录前依赖以下内容：

- 官方备份固件 `zectrix_note4_backup.bin`。
- WiFi SSID / 密码。
- WQN access token。
- Supabase service role key。
- 真实用户错题数据。

在刷写实机前，必须先确认 `CMOS5` 的真实含义，并验证 16 MB 官方备份镜像可以完整恢复设备。
