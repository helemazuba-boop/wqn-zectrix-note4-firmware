# 单词端云切换清单

## 候选冻结

- 记录 Firmware/WQN commit、ESP-IDF 5.5.4 revision、migration head、schema hash 和 pack manifest hash。
- 运行 Firmware 完整 build 与 M8 gate，WQN `npm run prepush`、数据库安全/并发测试和跨仓 fixtures。
- 确认 AI SSE 和 `wqn-flash-v2` 媒体帧格式未变。

## 云端前置门禁

- Web 和 AI 已全部改用 StudySession/CandidatePage/Observation RPC，不直写 `word_progress`。
- 错词投影与 observation 在同一事务中，幂等重试不产生重复错词。
- 自托管 `data.helema.cn` 目标已就位并通过 guarded migration dry-run。
- 在以上三项完成前，W7 仍是未完成，禁止删除仍有 AI/Web 调用者的云端旧函数或宣布 W8 完成。

## 设备门禁

- 固件不含 `/words/sync`、`/words/review`、daily target、整包 review indices 或单 pending slot。
- 顺序、随机和词典各自暂停/恢复；随机重启不重排，词典查看不改进度。
- 500 次混合操作无跳词、重复、错误归因；outbox 满、掉电、重试和深睡准备可恢复。
- 新 pack 仅下轮生效，实机校验 manifest/SHA/回滚包。

## 切换与回滚

1. 部署 schema/RPC/路由，生成确定性 pack v2。
2. 端云 smoke 通过后才停用旧写路径，不运行长期双栈。
3. 仅在已备份且可重建时清理旧词包；不删除云端用户业务数据。
4. 任一门禁失败时停止新写入，WQN 与固件按记录的成对 checkpoint 回滚。

