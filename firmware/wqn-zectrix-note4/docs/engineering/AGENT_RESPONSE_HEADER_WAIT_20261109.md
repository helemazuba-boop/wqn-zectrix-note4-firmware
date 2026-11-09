# Agent 响应头等待（2026-11-09）

用户报告 Agent 屏幕显示头不完整。客户端原错误串为 Response headers are incomplete，
指服务器响应头，不足以证明设备漏传请求头。当前设备日志没有记录 fetch_headers 的
原始返回值，因此不能断言用户的每次失败都属于临时等待；断连接仍须视为失败。

ESP-IDF fetch_headers 的 TCP timeout 分支返回 -ESP_ERR_HTTP_EAGAIN，且保留部分
响应头的 parser。原客户端把所有负数立即当作失败。新 helper 仅对这个临时返回继续
读取同一个 client；不重新 open、不重新提交音频、session 或 run。永久断连接立即失败。
JSON 与 ASR 的重试使用从请求创建时计算的剩余预算；SSE 头阶段从头读取开始计算既有
预算。每次 fetch 之间检查网络、剩余预算和 timeout 设置，临时等待会让出一个 tick。

失败提示区分网络断开、等待超时和响应中断；日志仅记录返回值、轮询次数，不记录 token、
完整音频或业务响应。SSE 的业务进度/等待用户/取消与 outbound reply 归属策略保持现有逻辑。

验证：完整 production client + real SSE/cJSON、虚拟 HTTP/时间 ports 共 199 项通过。
临时等待后恢复、120 秒等待耗尽、永久断连均验证音频只提交一次与 client cleanup；
run header 等待恢复后只提交一次运行。旧 client 与禁用 header waiting 负对照都完成
199 项，分别失败 4 项。实际 installed SDK fetch、headers-complete callback、HTTP parser
另有 7 项，覆盖跨 timeout 的拆分 header、content-length、chunked 与永久断连接。

这些证据不等于用户网络/代理或提供商实测。原日志没有原始头失败返回，实机仍需确认；
SDK 单次调用内部的慢速持续输入也不由虚拟端口的截止测试证明硬截止。本批不重发已被
服务端接受的 POST，不改变后端策略，不读取临时提供商 key。
