# 第九批：SSE 期限、溢出终止与语音地址配置

日期：2026-10-23（Asia/Shanghai）。分支：`fix/firmware-reliability`。起点：`44c7c73`。

> 当前需要执行的验收见[统一验收清单](FIRMWARE_RELIABILITY_ACCEPTANCE.md)；本文保留历史范围与证据，不要求逐批重复设备矩阵。

## 状态与范围

用户确认第八批“通过，继续”。记为用户确认设备节点验收，未附新串口日志，不补录
逐项故障注入或精确时序。本批处理审查 R-02、R-03、R-08，代码与 host 回归已完成，
新设备/网络故障节点仍单列。

2026-10-23 用户回复“通过 继续”，第九批记为用户确认节点验收。没有附新日志，
不补录逐项后端故障注入、真实 SDK 收尾时序或独立地址的网络验证。后续进入 B4。

| 提交 | 内容 |
|---|---|
| `2d960eb` | REST/WS 同源推导、完整 WS 地址覆盖、两个生产入口与 CMake 配置回归 |
| `4bfefcd` | HTTP 共享期限、HTTP/WS 溢出终止、正常终帧收尾与生产消费链路回归 |
| 本记录所在提交 | 前批验收、候选身份、SDK 边界与设备验证矩阵 |

## R-02：HTTP 整轮共享预算

此前 `request.timeout_ms` 只交给 SDK 作为单次等待参数；每次收到小包后，下一次等待
可以重新使用完整预算。代码注释称有“outer timeout”，实际没有相应的单调截止时间。

现在在初始化客户端之前记录绝对 deadline，连接、分段上传、响应头和读取共享余额。
每次进入 SDK I/O 前重新设置剩余毫秒；ON_HEADER 和 ON_DATA 同样消耗预算，并压缩
SDK 下一次等待参数。一次 `esp_http_client_read()` 可以内部循环并连续触发 ON_DATA，
所以在事件入口与帧派发循环内也检查时间，不只在外层 read 循环检查。

期限耗尽记录 `chat_timeout`，返回 `ESP_ERR_TIMEOUT`。以整毫秒向下取整设置等待，
不足 1 ms 时拒绝开始新操作。默认预算仍为 600000 ms，没有缩短产品默认值。
已经观察到的故障不会被后续响应状态覆盖。

收到 `final` 或远端 `error` 后结束本轮，禁止继续派发同包中的后续业务帧；不再等服务端
长连接自然断开。此前没有终帧的 EOF 可能返回 ESP_OK，现在明确失败。远端 error 的
既有返回契约仍是 ESP_OK，并在 response 中携带业务错误；HTTP 4xx/5xx 和传输失败
返回失败。响应头读取期间可能已经收到 body，因此在 ON_DATA 先捕获完成的 HTTP
状态，避免把 401/错误响应中的 SSE 体当作成功事件交给业务层。ON_HEADER 时状态
仍未定，继续禁止在那里锁存 -1。

**SDK 边界：**核对本地 ESP-IDF v5.5.6 的 `esp_http_client.c` 后，采用同一 HTTP owner
在事件回调中 `esp_http_client_close()` 关闭 transport；先置 close 标志，承受同步
DISCONNECTED 重入。客户端、parser 和回调 context 保留到 SDK 调用返回，只有外层
执行 cleanup。SDK 解析路径不使用事件回调返回值中断读取，因此不能仅 return error。
没有使用会关闭后再次连接的 cancel_request，也没有新增后台任务或重发。

这是应用层协作式期限。单次 SDK connect/write/read 内部等待、尚未产生事件的残缺
响应头、用户 callback 以及最终清理可能超过预算；取回控制权后拒绝晚到的非终帧结果。
它修复了持续 SSE 小包不断续期的路径，不能证明任意字节滴流下的硬实时完成上界。

## R-03：累计帧溢出必须结束当前流

生产 parser 的累计 `data:` 上限为 16 KiB，extract 溢出时返回 partial 并置 sticky 标志。
旧 HTTP 与 STD/PRO WS 只检查 feed 返回值，漏掉了这种“每行都小、累计过大”的失败。
WS 的行溢出路径还会通知稍后重置并继续消费，可能先派发后面的正常帧或 turn.released。

两个消费者现在每次 extract 后、任何事件派发之前检查 sticky 标志；行溢出和累计溢出
都立即结束当前流。HTTP 记录 frame_overflow 并关闭。WS 在 transport owner 内调用
真实 TeardownFailedTurn 并立即返回，不把损坏之后的帧交给业务层。

WS 沿用已有 FINAL 账本语义：Recording 为 DefinitelyNotSent / PreFinalFailed；
FinalInFlight 为 Ambiguous / HandoffAmbiguous；RemoteHandoff 为 TransportLost。
没有把不确定的 FINAL 重新判成可以 HTTP 重传，也没有伪造 Released。

`SseFrameBuffer::clear()` 仍保留 sticky，维护原契约；新增 reset 只用于已结束的连接
清理和新 turn 边界，避免前一轮失败永久污染后续会话。Agent/OpenCode 消费者已有
溢出检查，本批没有重写其协议。

## R-08：REST 地址覆盖同步到 WS

STD/PRO 与 Flash v2 现在共用 BuildVoiceWebSocketUrl：

- 默认从 WQN_API_BASE 取 scheme/authority：HTTPS 对应 WSS，HTTP 对应 WS，保留
  显式端口和带方括号的 IPv6。WS 路由继续使用 WQN_FLASH_WS_PATH；REST 路径前缀
  不会被猜测为 WS 路由前缀。
- 新增 CMake cache 项 WQN_VOICE_WS_URL；非空时指定完整 ws(s) URL，可覆盖独立
  voice origin 或代理路径。旧的硬编码 WQN_API_BASE_HOST 已移除。
- 不支持的 scheme、空 authority、凭据、空白/控制字符等基本非法配置被拒绝，剩余
  URI 解析由 SDK 完成；不回退到生产主机。Flash 已开始音频后的拒绝走已有清理入口。

默认地址仍为 `wss://wqn.helema.cn/api/esp32/realtime`。SDK 初始化会复制 URI，
局部 std::string 覆盖 init 期间的有效生命周期。Flash v1 保留其原配置分支。

示例：

```sh
idf.py -B build-wsl -DWQN_API_BASE=http://192.0.2.10:3000/api/esp32 build
idf.py -B build-wsl -DWQN_API_BASE=https://rest.example/api/esp32 \
  -DWQN_VOICE_WS_URL=wss://voice.example/custom/realtime build
```

这些是配置示例，不代表已连接上述地址。切回默认时应清空两个 cache 项。

## 验证与证据边界

| 检查 | 本轮结果 |
|---|---|
| 完整生产 HTTP 源文件 + SSE parser/JSON decoder | 562 项通过，ASan/UBSan/LeakSanitizer |
| 生产 WS ProcessTextStream、DoDestroyWs、TeardownFailedTurn | 215 项通过，同上 |
| 生产 CMake 定义与两个 WS URI 配置块 | 6 个配置变体通过，UBSan |
| 旧基线 `44c7c73` | 因终帧后继续派发而被行为断言拒绝，非编译失败 |
| 10 个错误变体 | 均被行为断言拒绝，非编译/环境失败 |
| 既有 ADC/UI guard | 100/1000 Hz 各 535 项通过，保留第五批回退 |
| 默认 SDK 构建 | 见下方候选身份 |
| 新设备 HIL、真实后端故障注入、远端 CI | 未执行 |

HTTP 测试替换 SDK I/O、单调时钟、MAC 和 token admission 端口，执行完整生产上传函数、
真实 public 类型、真实 SSE decoder 与 SDK cJSON。模拟一次 read 内多次事件回调、
partial write、响应头内 body、同步 DISCONNECTED、超时/溢出之后仍到达的正常帧。
WS 使用真实状态 enum、生产消费和 teardown 决策；RTOS 流/队列/信号、SDK stop/destroy、
账本发布和内存日志为端口。这些结果不证明真实 TLS/WS close 时序、ESP32 调度或网络概率。

错误变体包括：忽略 HTTP/WS sticky、WS 仅通知重置后继续、生命周期只 clear、移除期限、
每阶段重建预算、事件派发不检查期限、终帧后继续、漏掉头部读取期间的 HTTP 状态、恢复
硬编码生产 host。源码提取锚点与变体边界发生漂移会直接报错，不作为成功的反向控制。

CI 在现有 IDF v5.5.6 容器 job 增加两个脚本，以使用同版本 cJSON；远端未运行。
主日志为 `/tmp/note4-b09-{stream-final,endpoint,power,negative-final,build}.log`；错误变体
使用 `/tmp/note4-b09-mutation-<name>-final.log`。临时文件可能被系统清理。

## 候选身份

运行时提交：`4bfefcdf6b5806d4deb0c5e7ab13dbf7becf4bfa`，候选短标识 `4bfefcdf`。
从干净跟踪工作区执行 `idf.py -B build-wsl reconfigure build`，随后仅更新文档。

ESP-IDF v5.5.6 构建 EXIT=0，零编译警告，M8 ownership 与编译后 UI 栈门通过。
编译数据库确认烘焙标识无 dirty 后缀；REST/WS 覆盖 cache 均为空，保持默认配置。

| 对象 | 大小 / SHA-256 |
|---|---|
| build-wsl/wqn-zectrix-note4.bin | 3,647,728 B；`3e90d138e1266271353a6915b11905ddd4cd0f87c7a8f8e5a829a2b7dc08f330` |
| build-wsl/wqn-zectrix-note4.elf | 41,808,588 B；`49ea6b7200eb6028274da150ecba2077c24e8ec919588e311f82aa12120d6af3` |
| sdkconfig | 76,758 B；`70da51aea6e12c47566c6753b60f0c63221a038e7e9bf5751fbe152dd00cc7e3` |

源码/产物清单：`/tmp/note4-b09-identity.json`。构建日志：`/tmp/note4-b09-build.log`。
配置摘要与第八批一致；后续重构建可能改变构建时间和镜像摘要，需核对设备实际身份。

## 设备与网络验证矩阵

1. 核对候选身份，走现有 STD/PRO、Flash v2 的正常录音/识别/回复；确认结束后可立即
   再开一轮，页面交互与分钟刷新保持原节奏。已有连续录音证据继续保留，本批重点是新收尾。
2. 在可控后端验证 HTTP fallback：持续 heartbeat 跨越请求预算、终帧后连接不主动关、
   无终帧断线，确认及时收尾、错误码和下一轮可用。当前产品默认预算是十分钟；短预算
   实验须单独记录测试入口与镜像身份，不能冒充默认候选结果。
3. 后端分别发送单行超限和多行累计超限，再尾随合法 final/turn.released，确认失败后
   没有后续事件/工具动作，新连接恢复正常。host 证据不能代替真实 SDK 关闭交界。
4. WS 在 FINAL 前、FINAL 交接中、RemoteHandoff 阶段断开，核对账本/界面结果；交接
   不确定时禁止 HTTP 重发，重连后的正常新请求应成功。保留相同 request_id 的日志便于核对。
5. 默认配置完成正常验证；开发地址的 REST 与两个 WS 入口再用可控服务器核对实际
   scheme/host/port/path。Flash v1 的兼容分支未做新设备复测。

用户常规验收与受控网络故障验证分别记录。只有“通过”回复时记用户确认，不补写
未收到的逐项故障日志。本批节点之后，下一主线为 B4 输入采集/ring/睡眠交界回放；
B5 高占用存储、诊断开关对照、低电量/功耗和最终整机收口继续保留。
