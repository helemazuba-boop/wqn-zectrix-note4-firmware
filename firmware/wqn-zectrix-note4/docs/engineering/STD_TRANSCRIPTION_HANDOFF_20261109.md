# STD 转写结束交接与输入归属（2026-11-09）

## 设备证据

回归日志 `serial-COM5-261109-130347-163-40012.log` 中，第四次 HTTP 回退转写
`71212274d93a6e6f` 已收到 ASR 完成与 final；出现 callback 内 close_return 后，直到
日志末尾仍没有 SDK read_return、cleanup 或转写任务返回。日志并不证明提供商未完成。
用户确认屏幕长时间停在转写中。另有转写时进入状态栏、切换档位和点击清空的事件。
HTTP 回退的 gen=0 不能作为上下文被清空的证据。

## 修复

ESP-IDF 的实际 esp_http_client_read 会在 ON_DATA 返回后继续填充调用者的缓冲区。
原超时 helper 在回调中把外层选择的 500 ms 读取轮询恢复成整轮剩余预算。
读取阶段现在取 min(500 ms, 剩余预算)；连接、上传、响应头仍沿用既有整轮预算。
成功 terminal 的关闭由 SDK read 返回后的 owner 执行；fatal/溢出/绝对超时仍立即
中断，不能让连续流绕开绝对期限。失败的 timeout 配置必须进入显式错误。

完整 SSE 交换可以带 ASR error 帧。STD 转写 owner 检查 response.error_code，失败不会
进入确认发送状态；保留 HTTP API 对完整交换的既有返回契约。新增 settled 日志仅记
状态、待确认字节数和错误名，不输出转写内容。

STD 转写、待确认和交互决策统一拥有模态输入；状态栏不能抢先消费对应手势。转写快照
立即关闭旧状态栏编辑，转写按键清除旧的确认时间。ASR 完成后的确认需要重新按下，
录音松键尾事件不能提交刚完成的转写。空闲时状态栏原操作仍然可用。

## 验证范围

- installed SDK read 函数 + production helper：8 项；旧 helper 完成 8 项但失败 3 项；
  恢复整轮读取超时的语义负对照失败 2 项。
- production UI dispatch、snapshot 与 STD adapter：175 项；绕过转写输入归属及保留旧
  编辑器两个负对照均完成断言并被拒绝。
- STD Transcribe/Confirm/Cancel/interaction 原生回归含服务端 ASR error。
- 受影响传输、Agent、UI/navigation、回归门禁及零 warning SDK 构建随本批证据记录。

主机 I/O、时钟、设备状态和 panel ports 不等于实机或网络验收。仍需实机验证短/长录音、
HTTP 回退、成功后确认/重新输入、ASR 失败、连续手势、模式/清空隔离及取消路径。
未改变存储格式、现有 outbox 身份或学习时间规则。本批不含 R-15 草稿。
