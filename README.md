# WQN for ZecTrix -note4-firmware` 是 WQN 面向 **ZecTrix Note4 / ESP32-S3** 的专用固件。

它让 Note4 成为 WQN 的一个实体学习终端：同步需要学习的内容，在墨水屏设备上完成错题复习、单词学习、Todo 和 AI 交互，并将学习结果重新同步回 WQN。

复杂的数据、权限、模型与学习调度留在 WQN 服务端；设备只负责把学习带到一个更简单、更专注的界面上。

**设备没有也不会有任何娱乐性质的内容。**

> **Cloud decides what to study.
> Note4 makes studying it frictionless.**

---

## WQN × Note4

```text
                  WQN
        Web · API · AI · Data
        Scheduling · Insights
                   │
                   │ Sync
                   ▼
          ZecTrix Note4
     E-paper · Buttons · Audio
         Cache · Offline Study
                   │
                   │ Study evidence
                   ▼
                  WQN
```

WQN Web 负责：

* 题目、单词、Todo 与学习数据管理
* 学习内容与复习计划生成
* AI / ASR / LLM 服务
* 权限、用户数据与服务端凭据
* 多设备同步与长期学习数据

Note4 固件负责：

* 本地输入与学习交互
* 400 × 300 电子纸渲染
* 内容缓存与离线访问
* 音频采集
* 设备配对与同步
* 学习结果可靠回传
* 电源与硬件资源管理

服务端可以持续改变推荐和调度算法。

---

## 当前能力

### Study

* 错题同步、缓存与设备端复习
* 复习结果持久化与失败重试
* Word Study 学习流程
* Todo 同步与设备端查看
* 本地 Study Session 状态管理
* 离线内容与 durable outbox

### E-paper

* 400 × 300 Note4 电子纸 UI
* 全局刷新
* 局部窗口刷新
* 刷新冷却与调度
* 空闲显示电源管理


### AI

* 按住说话的语音交互
* Realtime模式
* 16 kHz mono PCM 音频采集
* 音频上传至 WQN
* 服务端 ASR /  LLM 处理
* 转写、模型回复与动作结果展示
* 支持设备端定义思考等级
* 支持设备端选择预设模型

模型密钥与 AI tool execution **不会进入设备固件**。

### Sync & Storage

* WQN 设备配对
* Wi-Fi 连接与重连
* WQN ESP32 API
* 同步 revision / cursor 管理
* NVS 控制状态
* SPIFFS 持久内容
* PSRAM 临时解码与 UI 数据
* 原子写入与可恢复缓存

### Runtime

固件按照明确的资源所有权组织硬件：

```text
Features
UI · Problem · Todo · Word · AI
                │
                ▼
          Service Layer
                │
                ▼
Display · Power · Connectivity
 Storage · Audio · Sync
                │
                ▼
       Runtime Primitives
                │
                ▼
      Note4 / ESP-IDF
```

显示、存储、Wi-Fi、音频和电源都有独立 runtime owner，业务功能通过稳定接口访问硬件资源。

详见 [`ARCHITECTURE.md`](firmware/wqn-zectrix-note4/ARCHITECTURE.md)。

---

## Hardware

| 项目         | 当前目标             |
| ---------- | ---------------- |
| Device     | ZecTrix Note4    |
| MCU        | ESP32-S3         |
| Display    | 4.2" E-paper     |
| Resolution | 400 × 300        |
| Flash      | 16 MiB           |
| Audio      | ES8311 / I2S     |
| Input      | Physical buttons |
| Framework  | ESP-IDF 5.5.4    |

当前固件针对 `zectrix-s3-epaper-4.2` 板级配置开发。

---

## Security Boundary

Note4 是 WQN 的受限客户端。

固件没有以下信息：

* Supabase service role key
* LLM / ASR provider key
* 服务端数据库权限
* Notebook 权限判断逻辑
* AI function-call 执行逻辑
* 不必要的长期用户私密数据

设备通过配对获得自己的设备凭据，并只调用 WQN ESP32 API。

```text
Note4
   │
   │ Device credential
   ▼
WQN ESP32 API
   │
   ├── Database / Permissions
   ├── ASR
   ├── LLM
   └── Tools
```

---

## Build

固件工程位于：

```text
firmware/wqn-zectrix-note4/
```

环境：

* ESP-IDF `5.5.4`
* ESP32-S3 toolchain

克隆仓库：

```bash
git clone https://github.com/helemazuba-boop/wqn-zectrix-note4-firmware.git
cd wqn-zectrix-note4-firmware/firmware/wqn-zectrix-note4
```

加载 ESP-IDF：

```bash
source "$IDF_PATH/export.sh"
```

然后：

```bash
idf.py set-target esp32s3
idf.py build
```

开发环境可以覆盖 WQN API：

```bash
idf.py -B build-ai-local-s3 \
  -DWQN_API_BASE=https://your-host.example.com/api/esp32 \
  build
```

默认服务端：

```text
https://wqn.helema.cn/api/esp32
```

在烧录真实 Note4 前，请先保留完整官方 Flash 备份，并确认目标串口和恢复路径。

更完整的构建、烧录和恢复说明见：

[`firmware/wqn-zectrix-note4/README.md`](firmware/wqn-zectrix-note4/README.md)

---

## Documentation

核心文档集中在 [`firmware/wqn-zectrix-note4/`](firmware/wqn-zectrix-note4/)：

* [`ARCHITECTURE.md`](firmware/wqn-zectrix-note4/ARCHITECTURE.md) — Runtime 架构与资源所有权
* [`RELEASE_CHECKLIST.md`](firmware/wqn-zectrix-note4/RELEASE_CHECKLIST.md) — 固件 / 云端联合发布检查
* [`TROUBLESHOOTING.md`](firmware/wqn-zectrix-note4/TROUBLESHOOTING.md) — 调试与故障排查
* [`STUDY_SESSION_PROTOCOL.md`](firmware/wqn-zectrix-note4/STUDY_SESSION_PROTOCOL.md) — Study Session 生命周期
* [`WORD_PACK_V2.md`](firmware/wqn-zectrix-note4/WORD_PACK_V2.md) — Word Pack 格式
* [`WORD_OFFLINE_OUTBOX.md`](firmware/wqn-zectrix-note4/WORD_OFFLINE_OUTBOX.md) — 单词离线提交队列
* [`STUDY_RUNTIME_REUSE.md`](firmware/wqn-zectrix-note4/STUDY_RUNTIME_REUSE.md) — Study Runtime 复用设计


协议、架构、恢复流程和实现细节以对应专项文档为准。

---

## WQN Ecosystem

本仓库只包含 **设备侧**。

WQN 的 Web、API、AI、数据与学习系统位于：

**[helemazuba-boop/Wrong-Question-Notebook](https://github.com/helemazuba-boop/Wrong-Question-Notebook)**

```text
WQN
│
├── Wrong-Question-Notebook
│   Web · API · AI · Data · Insights
│
└── wqn-zectrix-note4-firmware
    E-paper · Audio · Cache · Offline Study
```

两个项目共同构成 WQN 的完整学习闭环。

---

## Project Status

WQN for ZecTrix Note4 仍处于积极开发阶段。

当前重点已经从早期硬件 bring-up 转向：

* 稳定设备端学习体验
* 更可靠的离线与同步机制
* 电子纸刷新与交互体验
* Study Runtime 的统一
* 更清晰的端云职责边界
* 与 WQN 学习系统持续集成

部分能力，尤其深度睡眠和新的学习流程，仍可能发生协议或实现变化。

---

## License

This project is licensed under the **GNU General Public License v3.0**.

See [`LICENSE`](LICENSE) for details.
