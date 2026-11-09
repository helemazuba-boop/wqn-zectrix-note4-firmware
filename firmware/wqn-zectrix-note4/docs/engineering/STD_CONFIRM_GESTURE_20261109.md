# STD 确认短按范围（2026-11-09）

生产 button_input 在 200 ms 发 HoldPress，650 ms 才进入 LongPress；200–649 ms 松开
仍发 ShortPress，再于下一轮发原始 Release。旧 STD 决策适配器在 HoldPress 时清掉
fresh-press 标记，导致这段合法短按不能发送确认或回答问题。独立只读探针在 178 项中
复现 3 项失败；该问题独立于 ASR terminal 的交接阻塞。

决策模式保留新按下标记经过 HoldPress，在 LongPress/LongRelease/Release 时清除。
转写模式仍消费完整手势并清除标记；转写期间开始的 PTT 没有完成后新按下，不能发送。
没有改动录音阈值、全局按钮生产、状态栏或 Agent 后端。

原生回归 181 项覆盖 200/400/649 ms 释放、问题回答、跨转写边界旧 PTT 尾事件、完整
长按及其尾事件和原有转写模态回归。恢复旧 HoldPress 清标记的负对照必须完成断言并
失败；不能用编译错误/abort 当作检出。受影响 UI/navigation 门禁与 S3 SDK、M8、profile
和 UI 栈结果随本批证据记录。物理按键、去抖、电子纸与真实网络 HIL 仍需单独验证。

val-20261109.2 保留此前两批提交，此后续修正尚需独立提交/汇入；旧固定包保持原样。
