# Word 上传组批边界与无进展退避（2026-11-09）

实机日志 serial-COM5-261109-123722-022-20040.log 包含 60 次同步调度，中位间隔 1160 ms；
每轮第 12 次 word-outbox-peek-batch 返回 INVALID_ARG，没有 Word 排空记录。
pending=20 的同会话连续积压触发此路径，设备 UI/EPD 尚可工作。
完整源码与脱敏计数证据在工作区 doc/word-outbox-upload-boundary-20261109。

根因有两层：上传协议允许 64 条，排除列表却误用 10 条的 journal append 限制；
服务端确认 64 条后，原先还会把全部 ID 一次交给同样只允许 10 条的 ACK 写接口。
此外，组批读取错误无退避地返回 Pending，调度器将其视作可立即处理的后续工作。

公共存储契约现区分 64 个只读排除 ID 与 10 条 journal 写入。
上传仍可发送 64 条连续记录，持久化 ACK 按最多 10 条分段；只有成功持久化的前缀从 RAM ACK 列表移除。
后段写入失败时，已成功的前缀保持有效，退避绑定第一个尚未确认的 request_id。
读取失败会记录 selected/error 并走既有 local-storage 指数退避；自通知不越过该期限。
不改 outbox 二进制版本、记录身份/原时间、服务端协议，不清空或重置已有积压。

新增 test_word_upload_store.py 将实际 UploadPendingWordObservations、完整 word_study_store.cpp、
实际协议回执验证、重试函数和 OutboxWaitDelay 串在一起，使用真实 libc 文件。
StorageService 调度、NVS/RTC、网络服务与时间为明确端口，不构成 SPIFFS/RTOS/网络 HIL。
覆盖 10/11/20/64/65 条、完整及部分回执、后段 ACK 失败、journal 重载，以及自通知期间的退避。
三个行为负对照分别恢复 10 条 peek 限制、把 ACK 放大为整批、移除读取错误退避；编译失败/超时不能计作负对照通过。
既有上传 fixture 现遵守真实读/写容量，避免无边界的假接口再次掩盖问题。

待实机：在新镜像上验证既有 pending=20 原样上传、durable ACK 与 drained，
重复回执/ACK 写入失败与断电重载，以及无进展时的调度/WiFi demand 频率和功耗。
只含软件计数与主机文件证据时，不宣布设备问题已经消除。
