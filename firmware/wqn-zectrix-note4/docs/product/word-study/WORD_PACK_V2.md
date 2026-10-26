# Word Pack v2

Word Pack 只携带不可变内容，不包含用户进度或会话 cursor。

## 确定性

- pack schema version 固定为 2，格式为 JSONL，当前 compression 为 `none`。
- 输出不写入实时 `generated_at`；相同内容 revision 必须生成相同字节和 SHA-256。
- manifest 按 deck 提供 content revision、pack revision、entry count、byte size、SHA 和 change sequence，
  不使用跨词库的全局最大 revision。

## 有界资源

- 每包最多 10,000 词，未压缩最多 4 MiB，单行最多 8 KiB。
- 固件流式下载到临时文件，同时增量计算 SHA；不把整包放入 `std::string`。
- 字节数、行长、词数或 SHA 不匹配时拒绝安装，保留当前已验证包。
- 本地仅保留当前包和至多一个回滚包；容量不足时先停止下载，不覆盖原错误。

## 活动会话

session snapshot 固定 deck id、content revision、pack revision 和 SHA。后台同步只能把新包标记为
“下轮可用”，不能替换正在进行的会话索引。

