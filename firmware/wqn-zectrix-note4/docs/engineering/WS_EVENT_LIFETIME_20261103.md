# R-25 WebSocket 回调通知的连接归属

R-25 原为候选。本批在当前基线 `b2e67f5` 的受控主机时序中确认：旧 client 已经
完成回调、stop/destroy 并被替换，其遗留的任务通知仍可取消新 client 的握手。
这是 STD/PRO transport 的连接归属问题，不是 Flash 提供商配置 ACK 的问题。

## 合法触发顺序

1. transport owner 读取本轮任务通知后，开始处理新的 Connect 命令。
2. 旧连接在这一读取之后发布故障；或者 stop 到达正在读帧的旧 worker，recv 的
   REQUESTED_STOP 分支返回失败，SDK task 的失败分支调用 abort_connection，发布
   DISCONNECTED，再进入 FINISH/STOPPED。
3. owner 等到旧 worker 停止，destroy 旧 client，创建并启动新 client。
4. 下一轮任务通知同时含旧 DISCONNECTED 和新 CONNECTED。原来的通知没有连接
   身份，owner 会销毁当前的新 client，并将其 Connect 操作报告为 Cancelled。

空闲 worker 正常 stop 可以只发布 FINISH；测试保留了这一正对照，没有把所有
stop 一概当作 DISCONNECTED，也没有在 destroy 之后凭空调用旧回调。

仅在回调时比较 `event->client` 不足以修复这个窗口：旧回调执行时的 client 本来
就合法，丢失的是回调到 owner 消费之间的归属信息。

## 实现

[`ws_event_mailbox.h`](../../main/ws_event_mailbox.h) 保存当前连接代次及待消费事件。
owner 为每个 client lifetime 分配非零递增代次；用尽后拒绝新建归属，避免回绕。
回调上下文保存 client 与代次，注册到实际 SDK handler argument。

| 路径 | 行为 |
|---|---|
| CONNECTED / DISCONNECTED / ERROR / DATA | 验证上下文、client、代次，再把事件内容写入受保护的 mailbox |
| 任务通知 | 仅发送一个 callback-pending 唤醒位，不用这些位直接携带事件内容 |
| owner 消费 | 从当前 mailbox 取出事件，忽略没有当前 payload 的裸 socket 位 |
| destroy | 在 stop/join 前退休 mailbox、清除旧 backlog；旧唤醒位晚到只产生一次空唤醒 |
| 上下文复用 | SDK stop/destroy 完成回调生命周期屏障后才允许覆盖；stop 等待期间上下文字段保持只读 |
| text | copy 在短临界区外执行；先前已通过校验的旧 copy 由 stop/join 后的既有 stream reset 清除，新 client 随后才启动 |
| PCM reset / emergency / control | 保留原通知语义，不用清空整个任务通知的方式丢掉它们 |

没有新增队列、任务或逐帧动态分配。当前连接的 text-before-fault、FINAL/释放期限、
PCM 故障及握手状态机保留。事件注册失败也会关闭本次 client 并立即返回对应错误，
不会启动一个没有回调的握手然后等待超时。注册失败/代次耗尽有独立诊断阶段名。

## 验证

```sh
python3 scripts/test_ws_event_lifetime.py
python3 scripts/test_ws_event_lifetime.py --before b2e67f5
python3 scripts/test_ws_event_lifetime.py --mutation keep-backlog
```

回归提取当前生产 Connect、callback、notification consumer、teardown，以及真实
owner loop 中“读通知 → 排空控制队列”的顺序。SDK dispatch/register/abort/stop/destroy
函数、recv stop guard、task 接收失败分支和退出片段也来自仓内实际组件。RTOS、
事件 loop、网络、分配/释放及业务 text parser 为端口；不声称执行了完整网络 SDK。

旧基线在适用的 32 项检查中有 6 项失败，四种读取/故障排列中有三种误取消新
握手。修复后 86 项检查通过，新增身份/启动失败检查只适用于新实现，不把数量差
当作覆盖率。另有 20 轮并发 Post/Retire（合计 100,020 次 post 尝试），主机 mutex
对应生产临界区；不把它当作设备跨核调度或中断延迟证明。

四个语义负例保留旧 backlog、忽略 client、忽略代次、信任裸通知位；它们必须完成
行为断言并返回失败，编译失败/异常/sanitizer 不算有效检出。当前故障、text 与
fault 合并、stream overflow、PCM reset、emergency disconnect、注册/start 失败均有
正对照。已有 R-12/R-22/R-24/R-26 回归一并验证。

本地开发配置启用了开发菜单。显式 `ai-local-dev-menu` 契约检查其 17 个配置项；
独立 `build-robustness-prod-20261103` 使用自己的 sdkconfig 验证 `ai-local` 产品契约。
产品/CI 仍要求开发菜单关闭，不修改原设备配置来制造通过。构建目录及理由见
[BUILD_HERE](../development/BUILD_HERE.md)。

本批未做网络/设备 HIL、push、部署或刷机。SDK stop/destroy 非返回阻塞的总上界、
现场发生概率、实际资源/栈水位、物理 PCM 连续性及历史 STD cleanup 事件仍不能
由这些主机结果直接关闭；新归属协议依赖当前 SDK 的 stop/join 屏障。
