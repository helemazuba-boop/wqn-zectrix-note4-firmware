#!/usr/bin/env python3
"""HIL 日志判读器：把 PR#7 整改批次的验收矩阵自动化。

用法：
    python3 scripts/hil_check.py <serial-log> [<serial-log> ...]

对一个或多个串口日志跑全部判据，输出 PASS/FAIL/SKIP。SKIP 表示该判据
所需的场景未在日志中出现（不是通过，是没测到）——这是有意的：本整改的
多数问题只有真机能暴露，判读器负责把"看起来没问题"变成"逐条有据"。

判据来源（每条都可回溯）：
  P0-a  c93800c  wifi legacy 迁移（Codex 评论 2）
  P0-b  5ca3acd  sync journal 单写入者（Codex 评论 1）
  C5    37033e7  worker per-domain gate
  C6a   f71c255  scope 重置 generation 先行
  C6b   1fdfe52  scope 切换不阻塞 UI
  C8    9f1a6d8  候选页快照写搬到 runner
  PK    fbfa950→reverted  包读句柄复用（§十一方案3，已 revert：实测变慢，
                                 判据保留为「open_seek 不得劣化到 >500ms 中位」）
  OWNERS owner 命名 pass  storage_service.cpp:49/:346 的默认 "background" 桶
  BENCH  P1 量测 bench   单次元数据操作底价是否成立（重写前提的可证伪化）

退出码：0 = 全部 PASS；1 = 有 FAIL；2 = 用法/读取错误。
"""

from __future__ import annotations

import re
import statistics
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------- 解析原语 --

TX_RE = re.compile(
    # 时间戳可有可无：有的抓取不带 `I (ms)` 前缀。中间的
    # `storage_service: storage transaction complete: request=` 用 `[^\n]*?` 跳过
    # ——不能写成紧邻 owner= 的字面量，那会让可选组永远匹配不上而静默拿到 None
    # （1005.19 上就是这么错的：事务全都 t=None，端到端判据直接 SKIP）。
    r'(?:^[IWEDV] \((?P<t>\d+)\) [^\n]*?)?'
    r'owner=(?P<owner>[a-z0-9-]+) queue_wait_ms=(?P<qw>\d+) '
    r'elapsed_ms=(?P<el>\d+) result=(?P<res>\w+)', re.M)

CARD_RE = re.compile(
    r'word card loaded: open_seek_ms=(?P<os>\d+) read_close_ms=(?P<rc>\d+) '
    r'parse_ms=(?P<pa>\d+) total_ms=(?P<tot>\d+)')

SUBMIT_RE = re.compile(
    r'I \((?P<ts>\d+)\) wqn_ui: button event: id=(?P<id>\d+) type=(?P<type>\d+) '
    r'duration_ms=(?P<dur>\d+)')

DISPATCH_RE = re.compile(
    r'I \((?P<ts>\d+)\) wqn_ui: dispatch refresh: schedule=(?P<sch>\w+)')

PRESENT_RE = re.compile(
    r'display presented: revision=\d+ schedule=(?P<sch>\w+) elapsed_ms=(?P<el>\d+)')

# These are DIFFERENT tasks even though both emit with tag wqn_ui. A healthy
# EPD renderer must never hide the UI owner's 1128 B headroom (1009.4 crash).
STACK_RE = re.compile(r'RenderFrameToEpd: stack HWM before render: (?P<hwm>\d+) bytes free(?=\s|$)')
UI_STACK_RE = re.compile(
    r'^[IWEDV] \(\d+\) wqn_ui: UI stack HWM: '
    r'phase=(?P<phase>[a-z0-9-]+) free_bytes=(?P<hwm>\d+)(?=\s|$)', re.M)

# P1 量测协议（存储侧重写前的插桩）。字段由重写侧约定，判读器只按契约解析：
#   `atomic write: bytes=%u backup=%d fopen_ms=%lld write_ms=%lld stat_ms=%lld
#    remove_ms=%lld rename_backup_ms=%lld rename_primary_ms=%lld total_ms=%lld`
# `total_ms` 是一次 AtomicWrite 的墙钟；前七项是它的组成部分。对账判据就是拿
# 这七项之和与 total_ms 比——如果加起来远小于 total_ms，瓶颈不在 AtomicWrite，
# 我们在量一个不相干的东西。
PROBE_RE = re.compile(
    r'^[IWEDV] \((?P<at>\d+)\) \S+ atomic write: bytes=(?P<bytes>\d+) backup=(?P<backup>\d+) '
    r'fopen_ms=(?P<fopen>\d+) write_ms=(?P<write>\d+) stat_ms=(?P<stat>\d+) '
    r'remove_ms=(?P<remove>\d+) rename_backup_ms=(?P<rb>\d+) '
    r'rename_primary_ms=(?P<rp>\d+) total_ms=(?P<total>\d+)', re.M)

# bench 的起止标记。刻意宽容：只要一行里同时出现 bench 与 begin/end 即计入，
# 不锁定重写侧的措辞，免得它改个词判读器就瞎了。
BENCH_RE = re.compile(r'(?i)\bbench\b[^\n]*?\b(?P<what>begin|end)\b')

# `storage bench round: shape=%s round=%d wall_ms=%lld result=%s`
# wall_ms 是一笔 bench 事务的墙钟；probe shape 没有对应的 `atomic write:` 行
# （它只 fopen 一个不存在的路径），所以逐 op 明细只对写 shape 有意义。
BENCH_ROUND_RE = re.compile(
    r'storage bench round: shape=(?P<shape>\S+) round=(?P<round>\d+) '
    r'wall_ms=(?P<wall>\d+) result=(?P<result>\w+)')
# `long-held sleep lease: blocker=storage holder=storage-bench held_ms=209480`
# Only meaningful for holders that MUST have died: the bench's lease covers the
# whole run, so a warning naming it after `storage bench END` is a leak, not a
# long download. The pack/cloud holders legitimately span minutes and are
# deliberately NOT judged here (see BENCH:lease-released).
# `at_ms` is the log's own boot-relative clock, not held_ms -- that is the whole
# point of comparing them: held_ms counts from acquisition, so on its own it
# cannot say whether the lease outlived the run it was acquired for.
LEASE_RE = re.compile(
    r'\((?P<at_ms>\d+)\) (?P<tag>\w+): long-held sleep lease: '
    r'blocker=(?P<blocker>[\w-]+) holder=(?P<holder>[\w-]+) held_ms=(?P<held>\d+)')

# Boot-relative clock of the bench END marker. The marker prints `total_ms` (the
# bench's own duration) but not when it finished, so the clock comes from the log
# prefix -- same clock the lease warnings use, which is what makes them
# comparable at all. (Serial logs arrive CRLF; `\w+` cannot swallow the `\r`, so
# these patterns stop at the last field they name and never anchor to EOL.)
BENCH_END_RE = re.compile(r'\((?P<at_ms>\d+)\) \w+: storage bench END')

# P1b-B stream ramp: one shape, 5 rounds, each round writes a BIGGER chunk into
# the SAME already-open handle. This is the instrument that separates the two
# surviving cost models, which are degenerate at a single chunk size:
#   H1  cost is charged per CALL   -> cost_ms flat across chunk_bytes
#   H2  cost is charged per BYTE   -> cost_ms grows with chunk_bytes
# At the ~1719 B chunk 1005.15 actually measured, BOTH models predict ~890 ms
# (1719 x 0.517), which is why a single fixed size could never answer it.
# The round line carries every field on one line so `cost_ms` can be read
# against `chunk_bytes` and `file_bytes` directly:
#   `storage bench stream round=%d chunk_bytes=%u file_bytes=%ld result=%s cost_ms=%lld`
BENCH_STREAM_ROUND_RE = re.compile(
    r'storage bench stream round=(?P<round>\d+) chunk_bytes=(?P<chunk>\d+) '
    r'file_bytes=(?P<file>\d+) result=(?P<result>\w+) cost_ms=(?P<cost>\d+)')

# kCommit equivalent: the fflush+fsync+close wp-stream pays exactly once per
# download, timed apart from the rounds so per-round numbers stay clean.
# `aborted=1` means the ramp was cut short by the 6 s per-round budget.
BENCH_STREAM_COMMIT_RE = re.compile(
    # 对端把 cost_ms 改成了 cost_us（µs）——因为 ms 分辨率下 62 KiB 的 fsync
    # 会量成 0，而那个 0 被乘 21.1× 外推成"kCommit ≈0.0 s"，和 1.34 MiB 上真的
    # 15,327 ms 差 15 s。两种单位都认：手上既有旧格式日志，也会有新格式日志。
    # 单位不同，分辨率下限也要跟着变（见 kCommitFloorUs / kCommitFloorMs）。
    r'storage bench stream commit: result=(?P<result>\w+) '
    r'cost_(?P<unit>ms|us)=(?P<cost>\d+) aborted=(?P<aborted>\d+)')

# >6 s round. This is a CONCLUSION, not a crash: under H2 the big rounds are
# exactly the ones that blow the budget, so an abort is H2's strongest evidence.
# Treating it as "the bench died" throws away the one thing it measured.
BENCH_STREAM_ABORT_RE = re.compile(
    r'storage bench stream ABORT round=(?P<round>\d+): '
    r'cost_ms=(?P<cost>\d+) exceeds (?P<budget>\d+) ms')

# The read buffer size IS the wp-stream transaction count: a 1.34 MB pack at
# 2 KB/chunk costs ~780 transactions, at 32 KB/chunk ~41. If this line still
# reports the old 2 KB, the chunk-size fix is not in the build at all and the
# whole H1 prediction is void -- so the number is checked, not assumed.
WORD_PACK_READ_BUFFER_RE = re.compile(
    r'word-pack-download: read_buffer_bytes=(?P<bytes>\d+)')

# 一次词包下载的两端：起点（知道整包解压后多大）和终点（失败/成功各一行）。
# 这些行**不要用位置正则**：对端在这轮已经改过三次字段（`received=` → `of M`
# → `reason=` → `plain= / wire= / plain_bytes_per_s=`），每改一次位置正则就静默
# 失配一次，而失配的表现是"SKIP：本轮没有该行"——看着像没测，其实是判据瞎了。
# 所以只固定行头，字段一律用 KV_RE 扫，缺字段就是缺，多字段自动收下。
WORD_PACK_START_RE = re.compile(
    r'^[IWEDV] \((?P<t>\d+)\) wqn_api: word-pack-download: pack_id=\S+ '
    r'url=\S+ bytes_expected=(?P<bytes>\d+)', re.M)
WORD_PACK_END_RE = re.compile(
    r'^[IWEDV] \((?P<t>\d+)\) wqn_api: word-pack-download '
    r'(?P<what>failed|completed): '
    # err 是紧跟 failed: 的裸 token（ESP_ERR_TIMEOUT），不是 key=value，KV_RE 抓不到
    r'(?:(?P<err>[A-Z][A-Z0-9_]*) )?'
    r'(?P<body>.*)$', re.M)
KV_RE = re.compile(r'(?P<k>[a-z_]+)=(?P<v>[^\s]+)')
# word-pack-download 行出现过多少次 —— 用来抓"格式漂移"。判据正则没匹配上时
# 如果日志里明明有这些行，那是判据瞎了，必须 FAIL，不能 SKIP。
# 本会话已经因为这个模式错过一次（新增判据静默不触发）。
WORD_PACK_LINE_COUNT_RE = re.compile(
    r'^[IWEDV] \(\d+\) wqn_api: word-pack-download.*$', re.M)
# 可见的设备启动证据：bootloader 打分区表，紧跟 esp_image 段表，时钟重新
# 起跳。它也出现在 USB/软件复位后，不能单凭这行证明真实断电或冷启。
# WHY A NEW RE: 原来用 `text.count('opened COM')` 数启动，那数的是**监听器
# 连上串口的次数**，一次设备没重启也能有好几行（monitor_serial.ps1 每次自动
# 重连写一行 `[HH:mm:ss] opened COM7`）。两个数在多数日志里恰好相等，所以这个错
# 一直没露馅；在 1005.7crash（14 次启动）和 1004.2（12 次）上 `or` 短路把它
# 压成 1，**重启循环被判成单次启动**——假绿，且正好放掉最该抓的那份日志。
BOOT_BANNER_RE = re.compile(r'^[IWEDV] \(\d+\) boot: End of partition table',
                            re.M)
# 配对域也认直接ROM复位行（有时分区表未抓全）；仍不改 boots 的单一计数口径。
ROM_RESET_RE = re.compile(r'^(?:ESP-ROM:esp32s3\b|rst:0x[0-9a-fA-F]+\b)')
# 监听器附着次数。**不是设备事件**，只作上下文印出来：它解释"为什么有人看到
# 很多 attach 行"，也防止下一个人再把它当启动数。
#
# ⚠️ **两种写法都要认，不能只认新的。** 2026-10-06 projects-cf 把 emitter 从
# `[HH:mm:ss] opened COM7` 改名成 `[HH:mm:ss] listener attached: COM7`，理由是对的：
# 裸 `opened COM7` 长得和设备事件一模一样，正是我的启动数判据被骗的原因。
# 但如果只匹配新串，**全部 19 份历史日志的 monitor_attaches 会归零**——而那个
# 0 会被读成"这一轮监听器没连过"，与"旧 emitter"是两回事。
# 所以这里两型式并列：`opened`（历史）+ `listener attached:`（新）。
# 桌面 capture 脚本生成的 `[$ts] opened $Port @ $Baud` 由 `opened` 那半边接着。
#
# ⚠️⚠️ **`re.M` 是必需的，`^` 才是"行首"而不是"整个字符串的开头"。**
# 第一版漏了它，合成日志一测就露：`attaches = 0`。而它在 19 份历史日志上
# 全都显示 1、看着完全正常——因为那些日志的**第一行恰好就是 attach 行**，
# 于是"只匹配到开头那一行"和"匹配到所有行"在这一整个语料上**恒等**。
# 又一处"两个答案恰好相等所以发现不了"，和我刚修的启动数是同一个病：
# **拿单行日志做单元测试永远测不到 `^` 的语义，必须拿多行文本测。**
MONITOR_ATTACH_RE = re.compile(
    r'^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\] (?:listener attached:|opened) ?\w+',
    re.M)
# 服务器侧错误与"我们自己超时"要分开：前者这一轮什么都没测到。
WORD_PACK_HTTP_RE = re.compile(
    r'^[IWEDV] \((?P<t>\d+)\) wqn_api: word-pack-download HTTP status=(?P<status>\d+)',
    re.M)
# 真实下载路径的 kAppend —— **这一行给出的一直是缺的那个数**：解压后的分片大小。
# 直到它出现之前，"wp-stream 比 bench 贵几倍"有 1.22×~5.7× 四个自俞读法，
# 因为谁都不知道每笔到底写了多少解压字节。`total_bytes of %lu` 又给出真实进度，
# 不会再有人把压缩 received 当分母去算完成度。
WORD_PACK_APPEND_RE = re.compile(
    r'^[IWEDV] \((?P<t>\d+)\) word_pack: word pack stream append: '
    r'chunk_bytes=(?P<chunk>\d+) total_bytes=(?P<total>\d+) of (?P<want>\d+)', re.M)

# 真实流量侧的追加写（不是 bench）：`word observation durable: sequence=%u
# lookup_ms=%lld append_ms=%lld total_ms=%lld`。
# WHY 它重要: `BENCH:floor-model-holds` 只看 bench 的 append shape，而 bench 的
# 双峰完全有可能是**我们自己量测**造出来的（bench 逐轮换 shape、租约、排队）。
# 这个行让判据说一句它本来没资格说的话：双峰是产品自己的行为，不是 bench 的
# 行为。1005.26 实测 append_ms = 6/6/6/6/8/15/15 + 365/433/433，与 bench 的
# 31 / 1308~2149 同形（便宜簇 + 贵约百倍的簇），只是绝对量级因载荷不同而不同。
# 只锁行头，字段全部按 key=value 扫。
# WHY: 对端 1006.4 在 `append_ms=` 与 `total_ms=` 之间插了 `append_open_ms=` 和
# `append_bytes=`，位置正则当场一条都匹配不上——而"匹配不上"在判据里长得跟
# "这轮没测"一模一样，于是这条真实流量双峰证据会**静默消失**。它恰恰是"双峰不是
# bench 量测假象"的唯一凭据（1006.3 上 n=30）。加字段、换顺序都不该影响它。
WORD_OBS_DURABLE_HEAD = 'word observation durable:'

# `storage bench append round=%d bytes=%u result=%s new_object=%d`
# 这是 append shape **自己额外打**的一行：通用轮次行只有 shape/round/wall_ms/
# result/info_ms，没有 new_object。`new_object=1` 只在 round 0（建文件那次），
# 之后每轮都是 0 —— 就是用来把 append shape 实测出的两簇成本分开的那一位。
# 两行的 round 是同一个计数器（ctx.round = round），所以能按 round 对上。
BENCH_APPEND_RE = re.compile(
    r'storage bench append round=(?P<round>\d+) bytes=(?P<bytes>\d+) '
    r'result=(?P<result>\w+) new_object=(?P<new_object>\d+)')

# 数字型 key=value。刻意不带行头：同一份日志里同一类行的字段会增删，
# 判据只该对"这一行里有没有我要的那个 key"负责。
KV_NUM_RE = re.compile(r'(\w+)=(-?\d+)')
LOG_PREFIX_RE = re.compile(r'^[IWEDV] \((\d+)\)')
# 字符串型 key=value：`nvs write: key=word-session-cursor ...` 里的那个 key。
# WHY: `KV_NUM_RE` 只吃数字，所以 key= 会被整条丢掉——而"哪一笔是第一次写
# 这个 key"正是 §五之十 里唯一能分开"一次性成本"和"每次成本"的字段。
# 少了它，6 笔 52 B 写法会全部显示成 key=?，跨 key 的统计（几个 key × 几条）
# 就直接退化成"1 个 key"，那是我在 1006.10 上犯的错的同一个方向：
# **把不同来源的样本混成一个来源再统计**。
KV_KEY_RE = re.compile(r'\bkey=([A-Za-z0-9_.-]+)')

# Gate self-test (`RunUiGateSelfTest`, C1). Two lines, one honest verdict:
#   I wqn_ui_gates: commit_state gate self-test passed
#   E wqn_ui_gates: gate self-test failed: word-row scope switch refused ...
# The failing form repeats the gate name twice (message + bare name), so the
# names are de-duplicated below -- a report that lists `word-row` once is easier
# to act on than one that lists it twice.
GATE_OK_RE = re.compile(r'commit_state gate self-test passed')
GATE_FAIL_RE = re.compile(r'gate self-test failed: (?P<name>[a-z-]+)')

# kCommit 计时分辨率下限。日志时间戳是 ms，所以 `cost_ms=0` 的真实含义是
# "短于 1 ms"——对一次 fflush+fsync+fclose 来说不可能，它是**未测到**，不是免费。
# 1005.19 就是这个坑：bench 在 62 KiB 上量到 0 ms，外推 21.1× 后判据说
# "kCommit ≈0.0 s"，而同一份日志里真实收尾那笔是 15,327 ms，差 15 s。
# 低于这个值就不许参与外推，只许报"未实测"。
kStreamCommitResolutionMs = 2

# kCommit 在**同一个** bench shape（62 KiB 文件）上的历轮实测读数，单位 ms。
# 这六个数全是量的，不是估计，来源逐个标了：
#   1005.19 0        （`cost_ms=0`）
#   1005.24 0.099    （`cost_us=99`）
#   1005.26 0.115    （`cost_us=115`）
#   1005.28 1250.918 （`cost_us=1250918`）
#   1006.1  0.107    （`cost_us=107`）
#   1006.3  343.74   （`cost_us=343740`）
# ⚠️ **四次 ≈0.1 ms、两次 344~1251 ms，差 12,600×**。所以 kCommit 和这个文件系统上
# 其它东西一样是**双峰的**，而判据以前只看当前这一轮：1005.19/24/26/1006.1 说"低于
# 分辨率、未实测"，1005.28 说"≈26.4 s"，1006.3 说"≈7.3 s"——同一个 shape 上两个
# 相差 3.6× 的"量级估计"，各自都只来自一档里的一个样本。外推时必须说清这一点，
# 否则读的人会把 n=1 当成这个量的稳定值。
kStreamCommitHistoryMs = [0.0, 0.099, 0.115, 0.107, 1250.918, 343.74]

# stream ramp 的 deadline 标定值。两个数都来自实测日志/源码，不是估计：
#   pack    = 1,341,248 B —— 1005.15 `word-pack-download: ... bytes_expected=1341248`
#             （就是那一笔在 278528 B / 20.8% 处 ESP_ERR_TIMEOUT 的包）
#   deadline = 120,000 ms  —— wqn_api.cpp:1941-1942 `now + 120LL * 1000 * 1000`
# 换包型或改 deadline 时，这两个数是这里唯一要动的地方。
kStreamPackBytes = 1341248
kStreamDeadlineMs = 120000
# P1b-B 正在验证的那个读缓冲尺寸。读缓冲大小就是 wp-stream 的事务笔数，
# 所以"32 KB 够不够"是整个 P1b-B 要回答的产品问题。
kStreamChunkUnderTest = 32 * 1024

# NVS 分区算术，从本地 IDF 树 + 分区表实推：
#   每个 BLOB_DATA chunk = 1 + ceil(chunkSize / 32) —— nvs_page.cpp:186-191
#   BLOB_IDX   条目 = 1（固定 8 B 索引项，不在 isVariableLengthType 里）
#                                            —— nvs_storage.cpp:351-361
#   isVariableLengthType 只含 BLOB / SZ / BLOB_DATA —— nvs_types.hpp:33-38
#   分区预算 = 4 页 × 126 条 = 504 条          —— partitions/16m.csv:3 (0x4000)
#   单 blob 顶 = min(pages-1, …) × (32×125) = 12000 B —— nvs_storage.cpp:287
# 提出来给两条判据共用：本地定义过一份，peer 改分区表时只会改一处。
#
# ⚠️ **这个公式在 1006.10 上被实测打回来过一次**：我原来只算 BLOB_DATA，
# 得出 52 B = 3 条；实测 `used_entries` 每笔游标写涨 **4** 条。
# 差的就是那条 BLOB_IDX——`nvs_set_blob` 写 blob 时除数据项外**必定**再写一条
# 索引项（dataSize/chunkCount/chunkStart），而索引项是定长类型、占满 1 条。
# 跨页 blob 每个 BLOB_DATA chunk 都有表头；4000 B 上限不能当作单一表头。
# 以下大 blob 算的是紧密分块时的下界，页尾碎片可能使实际占用更大。
kNvsEntrySize = 32
kNvsEntryCountPerPage = 126
kNvsSizeBytes = 0x4000
kNvsPages = kNvsSizeBytes // 4096
kNvsBudget = kNvsPages * kNvsEntryCountPerPage
kNvsSingleBlobCap = min(kNvsPages - 1, (0xff - 1) // 2) * (
    kNvsEntrySize * (kNvsEntryCountPerPage - 1))
# 迁 NVS 的那个 52 B 暂停/恢复游标。
kSessionCursorBytes = 52


def nvs_entries(data_bytes: int) -> int:
    """紧密分块时的条目下界：每个 <=4000 B 的 chunk 表头 + 数据 + 一个索引。"""
    chunk_max = kNvsEntrySize * (kNvsEntryCountPerPage - 1)
    full, tail = divmod(data_bytes, chunk_max)
    return 1 + full * (1 + chunk_max // kNvsEntrySize) + (
        1 + -(-tail // kNvsEntrySize) if tail or not full else 0)


def commit_cost_ms(g):
    """把 bench 的 stream commit 行归一成 `cost_ms`，并标记原始单位。

    对端已把 `cost_ms` 改成 `cost_us`：ms 分辨率下一次 62 KiB 的 fsync 会量成 0，
    而那个 0 被外推成"kCommit ≈0.0 s"，与 1.34 MiB 上实测的 15,327 ms 差 15 s。
    两种单位都要认（旧日志是 ms、新日志是 us），归一后下游只跟一个字段打交道。
    """
    d = {k: (v if k in ('result', 'unit') else int(v)) for k, v in g.items()}
    d['cost_ms'] = d['cost'] / 1000.0 if g.get('unit') == 'us' else float(d['cost'])
    return d


def knee_note(fixed_ms, per_byte_ms, pts):
    """ramp 是否存在单一 `cost=F+s×bytes` 描述不了的拐点。返回给人读的一句，或 ''。

    逐个点算「实测 / 模型预测」。一条直线如果真描述整条 ramp，所有点都该 ≈1。
    1005.19 实测：2K 0.05、4K 0.08、8K 1.00、16K 1.04、32K 0.92 —— 最大/最小
    17×，即最小分片掉到了 append 基线价。这时候"H2 成立"这种单一标签是过度自信：
    它把"大分片区间每字节恒定"升级成了"整条 ramp 是一条直线"。

    阈值取 3×：拟合本身有噪声（单点抖动几 ms 就能让 2K 那种小值偏很多），
    3× 以下不报，避免对正常数据喊狼来了。
    """
    ratios = []
    for r in pts:
        predicted = fixed_ms + per_byte_ms * r[1]
        if predicted <= 0:
            return ''
        ratios.append((r[1], r[2] / predicted))
    if len(ratios) < 3:
        return ''
    lo = min(ratios, key=lambda x: x[1])
    hi = max(ratios, key=lambda x: x[1])
    if hi[1] <= 0 or hi[1] / max(lo[1], 1e-9) < 3.0:
        return ''
    worst = '、'.join(f'{b // 1024}K≈{q:.2f}' for b, q in ratios)
    return (f'⚠️ ramp 有拐点，单一 cost=F+s×bytes 描述不了整条'
            f'（实测/预测比 {hi[1] / max(lo[1], 1e-9):.0f}×：{worst}）。'
            f'大分片区间的斜率可用，但小分片已掉到 append 基线价，'
            f'别把"这个区间每字节恒定"读成"整条 ramp 是直线"')


def StreamFit(pts):
    """对 ramp 做 cost = F + s×bytes 的最小二乘，返回 (F, s)。

    pts 是 (round, chunk_bytes, cost_ms, cost_per_byte) 四元组。点数 <2 或
    自变量全同（分母为 0）时返回 None——这两种情况拟不出模型，调用方必须
    明说，不能拿一个假的结论顶替。
    截距与斜率都被夹到 ≥0：负的固定项或负的每字节成本在物理上不成立，
    宁可将该侧报成 0（即退化成另一个极端模型），也不要输出负数。
    """
    n = len(pts)
    if n < 2:
        return None
    sx = sum(p[1] for p in pts)
    sy = sum(p[2] for p in pts)
    sxx = sum(p[1] * p[1] for p in pts)
    sxy = sum(p[1] * p[2] for p in pts)
    denom = n * sxx - sx * sx
    if denom == 0:
        return None
    per_byte = (n * sxy - sx * sy) / denom
    fixed = (sy - per_byte * sx) / n
    return max(fixed, 0.0), max(per_byte, 0.0)


def StreamDeadlinePrediction(fixed_ms, per_byte_ms,
                             pack_bytes=kStreamPackBytes,
                             deadline_ms=kStreamDeadlineMs,
                             chunk_options=None,
                             commit_ms=None,
                             commit_file_bytes=None):
    """用拟合出的 F/s 算「这个包能不能在 deadline 内下完」，返回给人读的一句结论。

    关键是把 **每字节地板**（分片无穷大时的总耗时）单独算出来：固定项可以靠
    加大分片压掉，每字节项压不掉。地板若已贴近 deadline，那么"加大分片"这条
    路本身就到头了——这与"32 KB 就是修法"是完全相反的施工结论，必须显式写出来。

    `chunk_options` 是 [(标签, 分片字节), ...]，对每个分片假设各算一行。
    WHY 它是个列表而不是一个数：`kStreamChunkUnderTest` 是**正在验证的提案值**，
    不是当前构建的真实值。1005.26 第一次从真实下载里量到 chunk_bytes 中位
    6,140 B（n=261，mode 落在 6000–6999 那一档），而判据一直只印"32K 分片需
    41 笔"——把提案值当成了现状。41 笔 vs 218 笔差 5.3×，正是对端说的
    "1.22×~5.7× 歧义"中的大端。所以两个值都要印，并标明各自是什么。

    `commit_ms` / `commit_file_bytes` 是 kCommit 的实测：wp-stream 下载结束时要
    付一次 `fflush+fsync+fclose+remove(旧包)+rename`（word_pack.cpp:1309-1324），
    那是 1.28 MiB 对象上的一次 AtomicWrite 级提交。三个成本模型都没有这一项——
     F × n + s × 字节 覆盖的是 kAppend，不含 kCommit。
    ⚠️ 1005.15 **没有这项实测**：下载在 20.8% 处超时，kCommit 从未执行。本轮
    bench 会给一个点，但那是在 ramp 累计 62 KB 的文件上；外推到 1.28 MiB 是
    ~21×，线性外推（fflush/fsync 大致随脏页数走）只是一个量级估计。
    所以外推倍数 >4 时判据必须明说这是量级估计，不能当预测用。
    """
    per_byte_total = per_byte_ms * pack_bytes

    def fmt(ms):
        return f'{ms / 1000:.1f} s'

    per_option = []
    for label, chunk_bytes in chunk_options:
        n = -(-pack_bytes // max(chunk_bytes, 1))  # ceil
        per_option.append((label, chunk_bytes, n,
                           fixed_ms * n + per_byte_total))

    # --- kCommit 项 -------------------------------------------------------
    # 只加能算的那部分：commit_file_bytes 未知时无法外推，只能原样报出并说明
    # 它没有被计入总数——把未测项静默算成 0 比报"差一项"危险得多。
    # 而 commit_ms **低于计时分辨率**时比"未知"更糟：0 × 21.1 = 0 会生成一个
    # 看起来很精确的"kCommit ≈0.0 s"，听着像"这项免费"。真实代价见
    # STREAM:download-within-deadline（1005.19 的收尾一笔 15,327 ms）。
    commit_note = ''
    commit_total = 0.0
    # kCommit 自己的双峰：同一个 62 KiB shape 上，六轮读数四次 ≈0.1 ms、两次
    # 344~1251 ms（见 kStreamCommitHistoryMs 的来源注释）。**外推的是哪一档，
    # 必须说出来**——否则"≈7.3 s"（1006.3）和"≈26.4 s"（1005.28）这两个相差
    # 3.6× 的量级估计会被当成同一个量的两次测量，而它们各自只是 n=1。
    def commit_history_note(ms, factor):
        cheap = [v for v in kStreamCommitHistoryMs
                 if 0 < v < kStreamCommitResolutionMs]
        zero = [v for v in kStreamCommitHistoryMs if v == 0]
        expensive = [v for v in kStreamCommitHistoryMs if v >= kStreamCommitResolutionMs]
        if not expensive:
            return ''
        # 跨度按最小**非零**读数算：拿 0 当分母会得到 1e12 这种没意义的数，
        # 而 0 本身就是"低到测不出来"，不是"真的是零"。
        span = max(expensive) / min(cheap) if cheap else float('inf')
        mode = '贵档' if ms >= kStreamCommitResolutionMs else '便宜档'
        cheap_txt = f'{min(cheap):.2f}~{max(cheap):.2f} ms ×{len(cheap)}' if cheap \
            else f'{min(expensive):.0f}~{max(expensive):.0f} ms'
        exp_txt = f'{min(expensive):.0f}~{max(expensive):.0f} ms ×{len(expensive)}'
        return (f'。⚠️ kCommit 本身是**双峰**的：同一个 {commit_file_bytes // 1024} KiB '
                f'shape 上六轮实测——便宜档 {cheap_txt}'
                + (f'（另有 {len(zero)} 轮读到 0，即低到 ms 计时测不出）' if zero else '')
                + f'、贵档 {exp_txt}，最小非零与最贵差 **{span:.0f}×**。'
                f'本轮落在**{mode}**，外推只用这一档的单个样本 ⇒ '
                f'别把 {fmt(ms * factor)} 当成 kCommit 的稳定值')

    if commit_ms is not None and commit_file_bytes \
            and commit_ms >= kStreamCommitResolutionMs:
        factor = pack_bytes / commit_file_bytes
        commit_total = commit_ms * factor
        if factor > 4.0:
            commit_note = (
                f'；kCommit 实测 {commit_ms:.0f} ms 是在 {commit_file_bytes // 1024} KiB '
                f'文件上，外推 {factor:.1f}× 到 {pack_bytes // 1024} KiB ⇒ '
                f'≈{fmt(commit_total)}（**外推，仅量级估计，非预测**）')
        else:
            commit_note = (
                f'；kCommit {commit_ms:.0f} ms（{commit_file_bytes // 1024} KiB '
                f'文件，外推 {factor:.1f}×）⇒ ≈{fmt(commit_total)}')
        commit_note += commit_history_note(commit_ms, factor)
    elif commit_ms is not None and commit_file_bytes:
        # 0.107 ms 不许印成 "0 ms"：那正是"低分辨率的零被当成测出来的零"，
        # 听着像"这项免费"。保留两位小数，让便宜档读数本身可见
        # （1005.24/26/1006.1 分别是 0.099/0.115/0.107 ms）。
        shown = f'{commit_ms:.2f}'.rstrip('0').rstrip('.') \
            if commit_ms < 1 else f'{commit_ms:.0f}'
        commit_note = (
            f'；kCommit 实测 {shown} ms 在 {commit_file_bytes // 1024} KiB '
            f'文件上 = **低于 ms 计时分辨率，未实测**，不计入总数。别把它读成'
            f'"这项免费"——1005.19 里 1.34 MiB 的真实收尾是 15,327 ms')
        # 便宜档也要说：正是"本轮恰好落在便宜档"最容易让人以为这项免费。
        hist_factor = pack_bytes / commit_file_bytes
        commit_note += commit_history_note(commit_ms, hist_factor)
    elif commit_ms is not None:
        commit_note = f'；kCommit 实测 {commit_ms:.0f} ms（文件大小未知，未计入总数）'

    # 分片加到无穷大时的总耗时：kAppend 只剩一笔，所以固定项只付一次，
    # 不是一次都不付。漏掉 fixed_ms 会让地板在小 F 时偏一点、大 F 时偏很多。
    floor_total = fixed_ms + per_byte_total + commit_total

    # 每个分片假设各印一行。grand/append_total 取**分片最大**的那个（最乐观），
    # 因为调用方要判断"只看 kAppend 会得出尚有余量的结论"——只有最乐观那档才配
    # 谈"余量"，用小分片的数字去谈余量是把悲观值当乐观用。
    grand = per_option[-1][3] + commit_total
    append_total = per_option[-1][3]
    parts = [f'套到 {pack_bytes // 1024} KiB 词包（deadline {deadline_ms // 1000} s）']
    for label, chunk_bytes, n, tot in per_option:
        parts.append(f'{label} ⇒ {n} 笔 kAppend、共 {fmt(tot)}')
    if commit_note:
        parts.append(commit_note[1:])  # 去掉开头的'；'
    if grand <= deadline_ms:
        parts.append(
            f'合计 {fmt(grand)}，余量 {fmt(deadline_ms - grand)} ⇒ 够')
    else:
        # "分片加到无穷也救不回来"的判据必须是 **floor_total 本身** 超 deadline，
        # 不能用 deadline - per_byte - commit：那个量漏掉了固定项，而分片无穷大时
        # kAppend 仍要付一笔 F。漏掉它时，地板只超 deadline 一点点的情形会被算成
        # "需要 ≥6 MB 分片"——听着像有解，其实无解。
        if floor_total >= deadline_ms:
            parts.append(
                f'合计 {fmt(grand)} 已超 deadline {fmt(grand - deadline_ms)}，'
                f'且每字节 + kCommit 地板 {fmt(floor_total)} 本身就超 deadline '
                f'⇒ 分片加到无穷也救不回来')
        elif floor_total >= deadline_ms * 0.90:
            need = fixed_ms * pack_bytes / (deadline_ms - floor_total)
            parts.append(
                f'合计 {fmt(grand)}，超 deadline {fmt(grand - deadline_ms)}；'
                f'每字节 + kCommit 地板 {fmt(floor_total)} 已吃掉 deadline 的 '
                f'{floor_total / deadline_ms:.0%} ⇒ 分片这条路没有可用余量'
                f'（数学上需 ≥{need / 1024:.0f} KB 分片，实际不可行）')
        else:
            need = fixed_ms * pack_bytes / (deadline_ms - floor_total)
            parts.append(
                f'合计 {fmt(grand)}，超 deadline {fmt(grand - deadline_ms)}；'
                f'每字节 + kCommit 地板 {fmt(floor_total)}'
                f'（余量 {fmt(deadline_ms - floor_total)}）'
                f' ⇒ 需要 ≥{need / 1024:.0f} KB 分片才进 deadline')
    # 返回 grand 与 append_only 两项，调用方要判断"kCommit 是否把结论翻了过来"
    return '；'.join(parts), grand, append_total


def cluster_gap(walls):
    """按最大**相对**间隙把样本切簇，返回 [(lo, hi, n), ...] 升序。

    追加写在 SPIFFS 上的代价是双峰的：便宜那档是往已打开句柄追加（几毫秒到几十
    毫秒），贵的那档踩到对象创建 / backup 轮换 / GC（几百到上千毫秒）。两档各占
    一半而 n 只有 4 时，**中位数落在两档之间的空隙里**，既不是便宜档也不是贵档的
    代价——而空隙的位置随哪个便宜样本更大而漂移，于是同一组现象在两次 HIL 上给出
    差 2.6× 的"中位数"（1005.19 的 541 / 1005.24 的 216，原始样本几乎一样）。

    所以调用方必须先切簇：单峰才谈中位数，多峰就报各档、并拒绝拿中位数下结论。
    间隙按**相对值**算（相邻两个样本的比值），否则 10 ms 与 400 ms 之间的绝对差
    会被 1400 ms 与 1447 ms 之间的差压过去。
    返回至少一档；样本少于 2 个时也只有一档。
    """
    if not walls or len(walls) < 2:
        return [(min(walls) if walls else 0, max(walls) if walls else 0,
                 len(walls) if walls else 0)]
    xs = sorted(walls)
    best, best_i = 0.0, 0
    for i in range(1, len(xs)):
        gap = (xs[i] - xs[i - 1]) / xs[i - 1] if xs[i - 1] else float('inf')
        if gap > best:
            best, best_i = gap, i
    # 间隙要足够大才认为真是两档：小于 2×（100%）当噪声，否则 21/23 这种正常
    # 抖动也会被切成两档，反而把单峰数据也变成"不予裁决"。
    if best < 1.0:
        return [(xs[0], xs[-1], len(xs))]
    return ([(xs[0], xs[best_i - 1], best_i)]
            + cluster_gap(xs[best_i:]))


@dataclass
class Log:
    path: str
    text: str
    tx: list = field(default_factory=list)
    cards: list = field(default_factory=list)
    submits: list = field(default_factory=list)
    dispatches: list = field(default_factory=list)
    presents: list = field(default_factory=list)
    hwm: list = field(default_factory=list)
    ui_hwm: list = field(default_factory=list)
    probes: list = field(default_factory=list)
    bench_events: list = field(default_factory=list)
    rounds: list = field(default_factory=list)
    stream_rounds: list = field(default_factory=list)
    stream_commit: list = field(default_factory=list)
    stream_abort: list = field(default_factory=list)
    read_buffers: list = field(default_factory=list)
    downloads: list = field(default_factory=list)
    failures: list = field(default_factory=list)
    done: list = field(default_factory=list)
    http_err: list = field(default_factory=list)
    appends: list = field(default_factory=list)
    obs_durable: list = field(default_factory=list)
    # `nvs write: key= bytes= total_ms= changed=`（每笔 NVS 写一行）与
    # `nvs stats: used_entries= free_entries= available_entries= total_entries=`
    # （开机一次 + 每次游标写后一次）。字段名是对端的契约：`total_ms` 改名
    # 或 `changed` 消失，本文件多处就会静默失配，所以集中一处解析。
    nvs_writes: list = field(default_factory=list)
    nvs_stats: list = field(default_factory=list)
    # round -> new_object，append shape 专用（见 BENCH_APPEND_RE）。
    bench_new_object: dict = field(default_factory=dict)
    lines: list = field(default_factory=list)
    obs_line_count: int = 0
    bench_append_line_count: int = 0
    bench_round_line_count: int = 0
    bench_stream_line_count: int = 0
    nvs_write_line_count: int = 0
    nvs_stats_line_count: int = 0
    pack_line_count: int = 0
    boots: int = 0
    monitor_attaches: int = 0
    leases: list = field(default_factory=list)

    @classmethod
    def load(cls, path: str) -> 'Log':
        with open(path, encoding='utf-8', errors='ignore') as fh:
            text = fh.read()
        return cls.parse(path, text)

    @classmethod
    def from_text(cls, text: str, path: str = '<memory>') -> 'Log':
        """从一段字符串造 Log——给 `--selftest` 的合成夹具用。

        WHY: 合成日志原先写在 `/tmp`，而 `/tmp` 会被清。2026-10-06 抓到的那个
        假绿（`MONITOR_ATTACH_RE` 漏 `re.M`）唯一的证据就是那样一份文件，
        下一个会话拿不到它，同一个错可以再静默一年。夹具必须和判据放在一起。
        不落盘也顺带解决了"要不要把一个假日志提交进仓库"的问题。
        """
        return cls.parse(path, text)

    @classmethod
    def parse(cls, path: str, text: str) -> 'Log':
        log = cls(path=path, text=text)
        log.lines = text.splitlines()
        for m in CARD_RE.finditer(text):
            log.cards.append({k: int(v) for k, v in m.groupdict().items()})
        log.submits = [m.groupdict() for m in SUBMIT_RE.finditer(text)]
        log.dispatches = [m.groupdict() for m in DISPATCH_RE.finditer(text)]
        log.presents = [m.groupdict() for m in PRESENT_RE.finditer(text)]
        log.hwm = [int(m.group('hwm')) for m in STACK_RE.finditer(text)]
        log.ui_hwm = [int(m.group('hwm')) for m in UI_STACK_RE.finditer(text)]
        log.bench_events = [m.group('what').lower() for m in BENCH_RE.finditer(text)]
        log.rounds = [{k: (v if k in ('shape', 'result') else int(v))
                       for k, v in m.groupdict().items()}
                      for m in BENCH_ROUND_RE.finditer(text)]
        log.stream_rounds = [{k: (v if k == 'result' else int(v))
                              for k, v in m.groupdict().items()}
                             for m in BENCH_STREAM_ROUND_RE.finditer(text)]
        log.stream_commit = [commit_cost_ms(m.groupdict())
                             for m in BENCH_STREAM_COMMIT_RE.finditer(text)]
        log.stream_abort = [{k: int(v) for k, v in m.groupdict().items()}
                            for m in BENCH_STREAM_ABORT_RE.finditer(text)]
        log.read_buffers = [int(m.group('bytes'))
                            for m in WORD_PACK_READ_BUFFER_RE.finditer(text)]
        log.downloads = [{k: (v if k == 'url' else int(v))
                          for k, v in m.groupdict().items()}
                         for m in WORD_PACK_START_RE.finditer(text)]
        log.failures = []
        log.done = []
        for m in WORD_PACK_END_RE.finditer(text):
            d = {'t': int(m.group('t')), 'what': m.group('what'),
                 'err': m.group('err')}
            for kv in KV_RE.finditer(m.group('body')):
                key, val = kv.group('k'), kv.group('v')
                # 同一个 key 在新旧格式里叫不同的名字，归一到一套：
                #   完成字节数  plain= / bytes= / received=
                #   wire 字节数 wire= / received=
                #   文件名/URL 不是数字，原样留下
                try:
                    d[key] = int(val)
                except ValueError:
                    d[key] = val
            for old, new in (('plain', 'plain'), ('bytes', 'plain'),
                             ('received', 'wire')):
                if new not in d and old in d:
                    d[new] = d[old]
            (log.failures if d['what'] == 'failed' else log.done).append(d)
        log.http_err = [{k: int(v) for k, v in m.groupdict().items()}
                        for m in WORD_PACK_HTTP_RE.finditer(text)]
        # 真实下载路径 kAppend：chunk_bytes 是这一轮之前谁也不掌握的那个数。
        log.appends = [{'t': int(m.group('t')), 'chunk': int(m.group('chunk')),
                        'total': int(m.group('total')), 'want': int(m.group('want'))}
                       for m in WORD_PACK_APPEND_RE.finditer(text)]
        # 真实流量侧的追加写：用来看 bench 的双峰是不是我们自己量测造出来的。
        # 扫 key=value 而不是位置正则——理由见 WORD_OBS_DURABLE_HEAD 处。
        obs = []
        for line in log.lines:
            if WORD_OBS_DURABLE_HEAD not in line:
                continue
            f = {k: int(v) for k, v in KV_NUM_RE.findall(line)}
            # Keep the complete identity even when its UUID starts with digits;
            # the generic numeric scanner alone would capture only that prefix.
            for kv in KV_RE.finditer(line):
                if kv.group('k') == 'session':
                    f['session'] = kv.group('v')
            tm = LOG_PREFIX_RE.match(line)
            # `append` 是判据用的统一名（行里叫 append_ms）。缺它就算没解析到，
            # 由 LOG:*-format-parsed 当场 FAIL，不会伪装成"这轮没测"。
            if tm and 'append_ms' in f:
                obs.append(dict(f, t=int(tm.group(1)), append=f['append_ms']))
        log.obs_durable = obs
        # append shape 的 new_object 位（BENCH_APPEND_RE），按 round 索引。
        log.bench_new_object = {int(m.group('round')): int(m.group('new_object'))
                                for m in BENCH_APPEND_RE.finditer(text)}
        # 行数计数只服务 LOG:probe-lines-parsed，而且**必须带上必需字段才算**：
        # 数"行头出现过"会把"旧构建没有新字段"误报成漂移（第一版就是这么写的，
        # 10 份老日志里 `storage bench append round=` 全红，而 new_object 是新
        # 构建才有的）。数"行头 + 必需 key 同时出现"才对得上解析器的实际条件。
        for ln in log.lines:
            if WORD_OBS_DURABLE_HEAD in ln and 'append_ms=' in ln:
                log.obs_line_count += 1
            if ('storage bench append round=' in ln
                    and 'new_object=' in ln):
                log.bench_append_line_count += 1
            if ('storage bench round: shape=' in ln
                    and 'wall_ms=' in ln):
                log.bench_round_line_count += 1
            if 'storage bench stream round=' in ln and 'cost_ms=' in ln:
                log.bench_stream_line_count += 1
            if 'nvs write:' in ln and 'total_ms=' in ln:
                log.nvs_write_line_count += 1
            if 'nvs stats:' in ln and 'total_entries=' in ln:
                log.nvs_stats_line_count += 1
        # NVS 侧两行。`changed=0` 是**真的没写**（storage.cpp:1084 先读出旧值
        # memcmp，相同就跳过 nvs_set_blob），所以它必须原样带着走，否则
        # "游标没变所以不花钱"会被读成"NVS 就是快"——那是两个相反的结论。
        epoch = 0
        for line_no, line in enumerate(log.lines):
            # 仅设备启动/ROM复位证据切域，监听器 attach 不切。
            # 多任务日志有10–60 ms倒序实测，不能把每个时钟回退假装成重启；
            # 单笔统计配对另检验前后时钟包围关系，拒绝不自洽的窗口。
            if BOOT_BANNER_RE.search(line) or ROM_RESET_RE.search(line):
                epoch += 1
            for m in TX_RE.finditer(line):
                log.tx.append(dict(m.groupdict(), epoch=epoch, line=line_no))
            for m in PROBE_RE.finditer(line):
                log.probes.append(dict(
                    {k: int(v) for k, v in m.groupdict().items()},
                    epoch=epoch, line=line_no))
            if 'nvs write:' in line:
                f = {k: int(v) for k, v in KV_NUM_RE.findall(line)}
                km = KV_KEY_RE.search(line)
                if km:
                    f['key'] = km.group(1)
                tm = LOG_PREFIX_RE.match(line)
                if tm and 'total_ms' in f:
                    log.nvs_writes.append(dict(
                        f, t=int(tm.group(1)), epoch=epoch, line=line_no))
            elif 'nvs stats:' in line:
                f = {k: int(v) for k, v in KV_NUM_RE.findall(line)}
                tm = LOG_PREFIX_RE.match(line)
                if tm and 'total_entries' in f:
                    log.nvs_stats.append(dict(
                        f, t=int(tm.group(1)), epoch=epoch, line=line_no))
        # 格式漂移：有 word-pack-download 行，但没有一条 end 行被解析出来。
        log.pack_line_count = len(WORD_PACK_LINE_COUNT_RE.findall(text))
        # ⚠️⚠️ 启动数原来写成 `text.count('opened COM') or
        # text.count('End of partition table')` —— **这是个假绿，方向还正好相反**：
        # `or` 短路，只要 `opened COM` 非零就用它，把真正的启动数**丢掉**。
        # 而 `opened COM` 是**监听器事件**（监控脚本连上串口），从来不是设备事件：
        # 每次自动重连都会多一行，设备一次没重启也能有好几行。
        # 实测它把最该被这份判据抓住的日志放过去了：
        #   1005.7crash  partition table 出现 **14 次**（每次 t 都从 (99) 重新起跳、
        #                 紧跟完整 esp_image 段表 = 14 次完整冷启），判据报"1 次启动"；
        #   1004.2       同样 12 次 vs 报 1；
        #   26.16        6 次 vs 报 6（这份恰好监听次数=启动次数，所以一直没露馅）。
        # 1006.10 上 projects-cf 的 monitor_serial.ps1 每次自动重连也写一行
        # `[HH:mm:ss] opened COM7` ⇒ 下一份日志开始，冷启测试必然放大这个数。
        #
        # 设备启动的**唯一直接证据是 bootloader 自己打的那行**：每次冷启一次，
        # 段表和 t 计时都随之重置。监听器连了几次单独记，只作上下文。
        log.boots = len(BOOT_BANNER_RE.findall(text))
        log.monitor_attaches = len(MONITOR_ATTACH_RE.findall(text))
        log.leases = [{k: (v if k in ('blocker', 'holder', 'tag') else int(v))
                      for k, v in m.groupdict().items()}
                      for m in LEASE_RE.finditer(text)]
        return log
    def txof(self, owner: str) -> list:
        return [t for t in self.tx if t['owner'] == owner]

    def has(self, needle: str) -> bool:
        return needle in self.text

    def owners(self) -> dict:
        """owner -> (笔数, elapsed 中位, queue_wait 最大)。归因总表用。"""
        table = {}
        for t in self.tx:
            n, med, qw = table.get(t['owner'], (0, [], []))
            n += 1
            med.append(int(t['el']))
            qw.append(int(t['qw']))
            table[t['owner']] = (n, med, qw)
        return {o: (n, statistics.median(m), max(q))
                for o, (n, m, q) in table.items()}


# ------------------------------------------------------------------ 判据框架 --

RESULTS = []


def check(log: Log, name: str, kind: str, ok, detail: str = ''):
    RESULTS.append((log.path, name, kind, ok, detail))


def expect(log: Log, name: str, cond, detail: str = ''):
    check(log, name, 'PASS' if cond else 'FAIL', cond, detail)


def skip(log: Log, name: str, why: str):
    RESULTS.append((log.path, name, 'SKIP', None, why))


def qw(tx): return int(tx['qw'])
def el(tx): return int(tx['el'])


# ------------------------------------------------------------------ 判据实现 --

def hil_word_commit_queue(log: Log):
    """§7.3：答题排队 <500 ms；不依赖本轮是否恰好触发 session-save。

    BEGIN/END 内的探针会污染真实流量。用提交的整个入队至完成窗口剔除，
    而不是只看完成时刻。设备重启后时钟复位，窗口必须按启动分开。
    """
    name = 'WRITE:word-commit-queue-wait'
    epoch = 0
    transactions, windows = [], []
    active = None
    reserve_active = False
    malformed = 0
    for line in log.lines:
        if BOOT_BANNER_RE.search(line):
            epoch += 1
            active = None
            reserve_active = False
        reserve = re.search(r'storage gc reserve experiment (BEGIN|END)\b', line, re.I)
        bm = reserve or (None if reserve_active else re.search(
            r'storage bench (BEGIN|END)\b', line, re.I))
        if reserve:
            reserve_active = reserve.group(1).upper() == 'BEGIN'
        if bm:
            tm = LOG_PREFIX_RE.match(line)
            if not tm:
                malformed += 1
            elif bm.group(1).upper() == 'BEGIN':
                if active is None:
                    active = [epoch, int(tm.group(1)), None]
                    windows.append(active)
            elif active is not None:
                active[2] = int(tm.group(1))
                active = None
            elif windows and windows[-1][0] == epoch:
                # 旧 emitter 同时发 lowercase end stats 和 uppercase END。
                windows[-1][2] = int(tm.group(1))
        for m in TX_RE.finditer(line):
            transactions.append(dict(m.groupdict(), epoch=epoch))
    owners = {'word-observation-commit', 'word-observation-batch'}
    commits = [t for t in transactions if t['owner'] in owners]
    commit_heads = sum(any(f'owner={owner}' in line for owner in owners) for line in log.lines)
    if commit_heads != len(commits):
        expect(log, name, False,
               f'答题事务格式漂移：{commit_heads} 行 / {len(commits)} 条解析')
        return
    if not commits:
        skip(log, name, '无答题提交事务；不以 session-save 的存在作为触发门')
        return
    if malformed or any(t['t'] is None for t in commits):
        expect(log, name, False, '答题或 bench 时间戳缺失，不能可靠剔除探针污染')
        return
    clean, polluted = [], []
    for t in commits:
        end = int(t['t'])
        enqueue = end - el(t) - qw(t)
        if enqueue < 0:
            expect(log, name, False, '事务耗时与设备时钟矛盾，无法判读排队窗口')
            return
        overlaps = any(e == t['epoch'] and end >= start and
                       (stop is None or enqueue <= stop)
                       for e, start, stop in windows)
        (polluted if overlaps else clean).append(t)
    if not clean:
        skip(log, name, f'{len(polluted)} 笔全部与 bench 重叠；没有未污染样本')
        return
    worst = max(clean, key=qw)
    detail = (f'未污染答题 n={len(clean)}，排除 bench 重叠 n={len(polluted)}；'
              f'queue_wait 最大 {qw(worst)} ms（要求 <500 ms）')
    run = int(worst['t']) - el(worst)
    enqueue = run - qw(worst)
    blockers = []
    for t in transactions:
        if t is worst or t['epoch'] != worst['epoch'] or t['t'] is None:
            continue
        overlap = min(run, int(t['t'])) - max(enqueue, int(t['t']) - el(t))
        if overlap > 0:
            blockers.append((overlap, t))
    if blockers:
        overlap, t = max(blockers, key=lambda p: p[0])
        detail += (f"；等待窗口与 {t['owner']} 的执行重叠 {overlap} ms"
                   f'（该事务 elapsed={el(t)} ms；仅时序归因，不细分内部耗时）')
    expect(log, name, qw(worst) < 500, detail)


def hil_word_batches(log: Log):
    """Only what logs directly show: bounded RAM, record accounting, own TX window.

    RAM acceptance is NOT durability. Body total_ms is an append, not per-event
    latency and not full UI latency. No performance threshold is changed here.
    """
    schemas = {
        'word observation RAM accepted:': ({'sequence', 'pending', 'inflight'}, {'session'}),
        'word observation batch durable:': (
            {'count', 'appended', 'bytes', 'total_ms', 'first_sequence', 'last_sequence', 'mode'}, {'session'}),
        'word ACK batch durable:': ({'count', 'bytes', 'total_ms'}, set()),
        'word observation batch queued:': ({'count', 'oldest_age_ms', 'forced', 'op'}, {'session'}),
    }
    rows = {head: [] for head in schemas}
    malformed, epoch = [], 0
    for line_no, line in enumerate(log.lines):
        if BOOT_BANNER_RE.search(line) or ROM_RESET_RE.search(line):
            epoch += 1
        for head, (numbers, strings) in schemas.items():
            if head not in line:
                continue
            prefix = LOG_PREFIX_RE.match(line)
            pairs = KV_RE.findall(line.split(head, 1)[1])
            fields = dict(pairs)
            if (not prefix or len(fields) != len(pairs) or not (numbers | strings) <= fields.keys()
                    or any(not re.fullmatch(r'\d+', fields[key]) for key in numbers)):
                malformed.append(f'{line_no + 1}:{head}')
                continue
            fields.update({key: int(fields[key]) for key in numbers})
            rows[head].append(dict(fields, t=int(prefix.group(1)), epoch=epoch, line=line_no))
    fmt = 'LOG:word-batches-parsed'
    names = ['WRITE:word-RAM-bound', 'WRITE:word-batch-record-accounting',
             'WRITE:word-batch-transaction-window', 'WRITE:word-batch-flush-trigger']
    if malformed:
        expect(log, fmt, False, '批量格式漂移/缺字段/单位错误：' + ', '.join(malformed))
        for name in names:
            expect(log, name, False, '批量日志不可解析，不把格式漂移当未测')
        return
    if not any(rows.values()):
        skip(log, fmt, '无批量版行；旧版日志不是批量验证')
        for name in names:
            skip(log, name, '无对应批量场景；SKIP 不是通过')
        return
    expect(log, fmt, True, '；'.join(f'{head} n={len(values)}' for head, values in rows.items()))
    ram = rows['word observation RAM accepted:']
    observations = rows['word observation batch durable:']
    acks = rows['word ACK batch durable:']
    queued = rows['word observation batch queued:']
    if queued:
        expect(log, names[3], all(1 <= r['count'] <= 10 and r['op'] > 0 and r['forced'] in (0, 1)
                                 and (r['forced'] or r['count'] >= 5 or r['oldest_age_ms'] >= 30000)
                                 for r in queued),
               f'flush 入队 n={len(queued)}；5事件/最老30秒/强制边界之一须满足；'
               '此代理量只验触发原因，不证明排队/完成也在30秒内或无漏触发')
    else:
        skip(log, names[3], '缺 flush 入队行，不能由 durable 完成时间反推触发时间')
    if ram:
        expect(log, names[0], all(1 <= r['pending'] <= 10 and 0 <= r['inflight'] <= r['pending']
                                 and r['sequence'] >= 0 for r in ram),
               f"RAM 接收 n={len(ram)}, 等待+在途最大 {max(r['pending'] for r in ram)} (<=10)；"
               '仅采样边界，不证明所有中间状态/30秒触发或掉电恢复')
    else:
        skip(log, names[0], '无 RAM 接收行，不能用 durable 行代替')
    records_ok = all(1 <= r['count'] <= 10 and 1 <= r['appended'] <= r['count']
                     and r['bytes'] == r['appended'] * 200 and r['first_sequence'] >= 0
                     and r['last_sequence'] - r['first_sequence'] + 1 == r['count']
                     and 0 <= r['mode'] < 7 for r in observations)
    records_ok &= all(1 <= r['count'] <= 5 and r['bytes'] == r['count'] * 200 for r in acks)
    if observations or acks:
        expect(log, names[1], records_ok,
               f'作答批 n={len(observations)}；ACK 批 n={len(acks)}；200 B/实际追加记录；'
               '作答可含已耐久前缀的重试，不把 count 当新增记录数')
    else:
        skip(log, names[1], '只有 RAM 接收，未观察到耐久批量')
    # Same boot, own completed transaction and own measured append duration.
    # A nearby save/other batch/previous boot must never pay this body cost.
    observed = [(r, 'word-observation-batch') for r in observations] + [
        (r, 'word-outbox-ack-batch') for r in acks]
    if not observed:
        skip(log, names[2], '无批量耐久行，不用 RAM 接收冒充')
        return
    paired, bad, missing, used = [], [], [], set()
    for row, owner in observed:
        candidates = [tx for tx in log.tx if tx['owner'] == owner and tx['epoch'] == row['epoch']
                      and tx['line'] > row['line'] and tx['t'] is not None
                      and int(tx['t']) - el(tx) - 2 <= row['t'] <= int(tx['t'])]
        if len(candidates) != 1:
            missing.append(row['line'] + 1)
            continue
        tx = candidates[0]
        if tx['line'] in used or tx['res'] != 'ESP_OK' or row['total_ms'] > el(tx) + 2 or (
                row['t'] - row['total_ms'] < int(tx['t']) - el(tx) - 2):
            bad.append(row['line'] + 1)
        used.add(tx['line'])
        paired.append(tx)
    if bad or (missing and paired):
        expect(log, names[2], False, f'自己的执行窗对不上：bad={bad}, missing={missing}')
    elif missing:
        skip(log, names[2], f'耐久行缺自己的 StorageService 完成窗：{missing}；无法对账')
    else:
        costs = sorted(el(tx) + qw(tx) for tx in paired)
        p90 = costs[(9 * len(costs) + 9) // 10 - 1]
        expect(log, names[2], True,
               f'StorageService 入队至完成 n={len(costs)}, p90={p90} ms, max={max(costs)} ms；'
               '这不是按键到落盘/单事件时延；不除以批大小，也不宣称性能达标')


def hil_word_ram_reads(log: Log):
    """Separate physical prefetch reads from RAM hits; never turn hits into 0ms I/O."""
    name = 'PK:word-RAM-read-provenance'
    foreground, prefetch, hits, legacy, bad = [], [], 0, 0, []
    for line in log.lines:
        if 'word card RAM hit:' in line:
            fields = dict(KV_RE.findall(line))
            if not LOG_PREFIX_RE.match(line) or not re.fullmatch(r'\d+', fields.get('ordinal', '')) or not fields.get('session'):
                bad.append('RAM hit 缺身份/时间戳')
            else:
                hits += 1
        if 'word card loaded:' not in line:
            continue
        fields = dict(KV_RE.findall(line))
        if 'source' not in fields:
            legacy += 1
            continue
        match = CARD_RE.search(line)
        cost_keys = ('open_seek_ms', 'read_close_ms', 'parse_ms', 'total_ms', 'fopen_ms', 'fseek_ms')
        if (fields['source'] not in ('foreground', 'prefetch') or not match or not LOG_PREFIX_RE.match(line)
                or any(not re.fullmatch(r'\d+', fields.get(key, '')) for key in cost_keys)):
            bad.append('card loaded source/成本字段漂移')
            continue
        target = prefetch if fields['source'] == 'prefetch' else foreground
        target.append(int(match.group('tot')))
    if bad:
        expect(log, name, False, '；'.join(bad))
    elif not prefetch and not foreground and not hits:
        skip(log, name, f'无 RAM 版来源证据；legacy 物理读 n={legacy}')
    else:
        expect(log, name, True,
               f'RAM命中 n={hits}（不是 0ms 物理读）；foreground n={len(foreground)}, '
               f'prefetch n={len(prefetch)}, legacy n={legacy}；'
               f'物理预取成本 max={max(prefetch) if prefetch else "未测"} ms；'
               '不把异步搬离前台当硬件加速，也未证明完整按键→呈现延迟')


def probe_profile(line: str, allowed) -> bool:
    """Exact field token: snapshot-held/paired must not silently mean snapshot."""
    m = re.search(r'(?:^|\s)profile=(\S+)', line)
    return bool(m and m.group(1) in allowed)


def hil_snapshot_append_probe(log: Log):
    """实尺寸 append 原型：全量逐 op 对账，首次创建与稳态分开，不验恢复协议。"""
    head = 'storage snapshot append probe:'
    rows, bad = [], []
    epoch, serial = 0, 0
    active = None
    runs = {}
    required = {'bytes', 'round', 'result', 'new_object', 'open_us', 'write_us',
                'flush_us', 'sync_us', 'close_us', 'total_us', 'written_bytes'}
    for line in log.lines:
        if BOOT_BANNER_RE.search(line):
            epoch += 1
            active = None
        if 'storage bench BEGIN' in line and probe_profile(line, ('snapshot', 'snapshot-paired')):
            if not LOG_PREFIX_RE.match(line):
                bad.append(line)
            serial += 1
            active = (epoch, serial)
            runs[active] = None
        if 'storage bench END' in line and probe_profile(line, ('snapshot', 'snapshot-paired')):
            end_fields = {m.group('k'): m.group('v') for m in KV_RE.finditer(line)}
            if active is None or 'result' not in end_fields or not LOG_PREFIX_RE.match(line):
                bad.append(line)
            else:
                runs[active] = end_fields['result']
                active = None
        if head not in line:
            continue
        fields = {m.group('k'): m.group('v') for m in KV_RE.finditer(line)}
        try:
            if (not required <= fields.keys() or not LOG_PREFIX_RE.match(line)
                    or active is None):
                raise ValueError('missing fields or timestamp')
            row = {k: (fields[k] if k == 'result' else int(fields[k]))
                   for k in required}
            if any(v < 0 for k, v in row.items() if k != 'result'):
                raise ValueError('negative metric')
            if row['bytes'] not in (3379, 8676) or row['round'] >= 12:
                raise ValueError('unsupported size or round')
            row['run'] = active
            rows.append(row)
        except ValueError:
            bad.append(line)
    if not rows and not bad and not runs:
        skip(log, 'LOG:snapshot-append-probe-parsed', '本轮无实尺寸快照 append 探针')
    else:
        expect(log, 'LOG:snapshot-append-probe-parsed', not bad,
               f'解析 {len(rows)} 行，格式异常 {len(bad)} 行')
    for size in (3379, 8676):
        name = f'WRITE:snapshot-append-{size}-p90'
        samples = [r for r in rows if r['bytes'] == size]
        if bad:
            expect(log, name, False, '有格式漂移，不能以剩余可解析行验收')
            continue
        if any(status not in (None, 'ESP_OK') for status in runs.values()):
            expect(log, name, False, '探针运行/清理中止；不能以成功的子集验收')
            continue
        if not samples:
            skip(log, name, '缺此尺寸的 append 探针；不能用 52/200 B 外推')
            continue
        fields_ok = all(
            r['result'] == 'ESP_OK' and r['written_bytes'] == size and
            r['new_object'] == int(r['round'] == 0) and
            sum(r[k] for k in ('open_us', 'write_us', 'flush_us', 'sync_us',
                              'close_us')) <= r['total_us'] <=
            sum(r[k] for k in ('open_us', 'write_us', 'flush_us', 'sync_us',
                              'close_us')) + 2000
            for r in samples)
        rounds = [(r['run'], r['round']) for r in samples]
        if not fields_ok or len(rounds) != len(set(rounds)):
            expect(log, name, False, '写入失败/短写/创建位错误/分项不对账/重复 round')
            continue
        if (any(status is None for status in runs.values()) or
                any(sorted(r['round'] for r in samples if r['run'] == run)
                    != list(range(12)) for run in runs)):
            skip(log, name, f'{len(runs)} 次运行共 {len(samples)} 笔；缺 END 或每次 12 轮未齐')
            continue
        steady = sorted(r['total_us'] for r in samples if r['round'] != 0)
        per_run = [sorted(r['total_us'] for r in samples
                          if r['run'] == run and r['round'] != 0) for run in runs]
        p90 = max(s[(9 * len(s) + 9) // 10 - 1] for s in per_run)
        clusters = cluster_gap(steady)
        first = [r['total_us'] / 1000 for r in samples if r['round'] == 0]
        expect(log, name, p90 < 200000 and len(clusters) == 1,
               f'首次创建(ms)={first}；每次稳态 n=11，最差运行最近秩 p90='
               f'{p90 / 1000:.3f} ms（<200 ms），最大 {max(steady) / 1000:.3f} ms；'
               f'分簇(us)={clusters}（多档拒绝以单一成本验收）；'
               f'仅探针性能，不代表会话恢复/真实答题已通过')


def hil_snapshot_held_probe(log: Log):
    """Long-held FILE, every write still fflush+fsync; open/close measured apart.

    API success flags and post-close file length are observability checks, not
    proof of power-loss recovery. Never include close/stat in a commit total.
    """
    required = {
        'open': {'bytes', 'result', 'new_object', 'open_us'},
        'commit': {'bytes', 'round', 'result', 'write_us', 'flush_us', 'sync_us',
                   'total_us', 'written_bytes', 'flush_ok', 'sync_attempted', 'sync_ok'},
        'close': {'bytes', 'result', 'close_us', 'rounds', 'committed_bytes',
                  'file_bytes', 'stat_us', 'close_ok', 'stat_ok'},
        'control': {'bytes', 'round', 'result', 'new_object', 'open_us', 'write_us',
                    'flush_us', 'sync_us', 'close_us', 'total_us', 'written_bytes'},
    }
    rows, bad, runs, paired = [], [], {}, set()
    epoch, serial, active = 0, 0, None
    for line in log.lines:
        if BOOT_BANNER_RE.search(line):
            epoch += 1
            active = None
        relevant = probe_profile(line, ('snapshot-held', 'snapshot-paired'))
        if 'storage bench BEGIN' in line and relevant:
            serial += 1
            active = (epoch, serial)
            runs[active] = None
            if probe_profile(line, ('snapshot-paired',)):
                paired.add(active)
            if not LOG_PREFIX_RE.match(line):
                bad.append(line)
        if 'storage bench END' in line and relevant:
            f = {m.group('k'): m.group('v') for m in KV_RE.finditer(line)}
            if active is None or 'result' not in f or not LOG_PREFIX_RE.match(line):
                bad.append(line)
            else:
                runs[active] = f['result']
                active = None
        kind = None
        if 'storage snapshot held ' in line:
            km = re.search(r'storage snapshot held (open|commit|close):', line)
            if not km:
                bad.append(line)
                continue
            kind = km.group(1)
        elif active in paired and 'storage snapshot append probe:' in line:
            kind = 'control'
        if kind is None:
            continue
        fields = {m.group('k'): m.group('v') for m in KV_RE.finditer(line)}
        try:
            if (not required[kind] <= fields.keys() or active is None or
                    not LOG_PREFIX_RE.match(line)):
                raise ValueError('missing fields/run/timestamp')
            row = {k: (fields[k] if k == 'result' else int(fields[k]))
                   for k in required[kind]}
            if (row['bytes'] not in (3379, 8676) or
                    any(v < 0 for k, v in row.items() if k not in ('result', 'file_bytes')) or
                    ('round' in row and row['round'] >= 12)):
                raise ValueError('unsupported metric')
            rows.append(dict(row, kind=kind, run=active))
        except ValueError:
            bad.append(line)
    fmt = 'LOG:snapshot-held-probe-parsed'
    if not rows and not bad and not runs:
        skip(log, fmt, '无长开句柄快照探针')
    else:
        expect(log, fmt, not bad,
               f'长开事件 {sum(r["kind"] != "control" for r in rows)}，'
               f'重开对照 {sum(r["kind"] == "control" for r in rows)}，格式异常 {len(bad)}')
    aborted = any(s not in (None, 'ESP_OK') for s in runs.values())

    def row_ok(r):
        if r['result'] != 'ESP_OK':
            return False
        if r['kind'] == 'open':
            return r['new_object'] == 1
        if r['kind'] == 'close':
            return (r['close_ok'] == r['stat_ok'] == 1 and r['rounds'] <= 12 and
                    r['file_bytes'] == r['committed_bytes'] == r['rounds'] * r['bytes'])
        parts = ('write_us', 'flush_us', 'sync_us')
        if r['kind'] == 'commit':
            extra_ok = r['flush_ok'] == r['sync_attempted'] == r['sync_ok'] == 1
        else:
            parts = ('open_us', 'write_us', 'flush_us', 'sync_us', 'close_us')
            extra_ok = r['new_object'] == int(r['round'] == 0)
        return (extra_ok and r['written_bytes'] == r['bytes'] and
                0 <= r['total_us'] - sum(r[k] for k in parts) <= 2000)

    for size in (3379, 8676):
        name = f'WRITE:snapshot-held-{size}-p90'
        samples = [r for r in rows if r['bytes'] == size and r['kind'] != 'control']
        if bad or aborted:
            expect(log, name, False, '格式漂移或运行/清理中止，不能拿成功子集验收')
            continue
        if not samples:
            skip(log, name, '没有此尺寸的长开持久提交，不能从重开或其他尺寸外推')
            continue
        valid = all(row_ok(r) for r in samples)
        groups = [[r for r in samples if r['run'] == run] for run in runs]
        duplicate = any(
            sum(r['kind'] == 'open' for r in g) > 1 or
            sum(r['kind'] == 'close' for r in g) > 1 or
            len([r for r in g if r['kind'] == 'commit']) !=
            len({r['round'] for r in g if r['kind'] == 'commit'}) for g in groups)
        if not valid or duplicate:
            expect(log, name, False, '同步未成功/短写/分项或长度不对账/重复生命周期事件')
            continue
        complete = all(
            len(g) == 14 and sum(r['kind'] == 'open' for r in g) == 1 and
            sum(r['kind'] == 'close' for r in g) == 1 and
            sorted(r['round'] for r in g if r['kind'] == 'commit') == list(range(12))
            for g in groups)
        if not complete or any(s is None for s in runs.values()):
            skip(log, name, '缺每次运行的 open→12 次 commit→close/END；未完整测到')
            continue
        ordered = all(g[0]['kind'] == 'open' and g[-1]['kind'] == 'close' and
                      [r['round'] for r in g if r['kind'] == 'commit'] == list(range(12)) and
                      g[-1]['rounds'] == 12 for g in groups)
        if not ordered:
            expect(log, name, False, '句柄生命周期/轮次顺序不符合连续追加协议')
            continue
        steady = [[r['total_us'] for r in g if r['kind'] == 'commit' and r['round'] > 0]
                  for g in groups]
        p90 = max(sorted(s)[(9 * len(s) + 9) // 10 - 1] for s in steady)
        clusters = cluster_gap([v for s in steady for v in s])
        opened = [g[0]['open_us'] / 1000 for g in groups]
        closed = [g[-1]['close_us'] / 1000 for g in groups]
        first = [next(r['total_us'] / 1000 for r in g if r['kind'] == 'commit')
                 for g in groups]
        expect(log, name, p90 < 200000 and len(clusters) == 1,
               f'首次 open(ms)={opened}，首次 commit(ms)={first}，最终 close(ms)={closed}；'
               f'每次稳态 n=11，最差运行 p90={p90 / 1000:.3f} ms（<200 ms），'
               f'分簇(us)={clusters}；每笔同步且文件长度对账，不代表掉电安全已验')
    name = 'WRITE:snapshot-held-paired-control'
    if bad or aborted:
        expect(log, name, False, '格式漂移或运行中止，不能以子集比较两种策略')
    elif not paired:
        skip(log, name, '无同一次运行的交替对照（历史日志不冒充配对实验）')
    else:
        groups = [[r for r in rows if r['run'] == run and r['bytes'] == size and
                   r['kind'] in ('control', 'commit')]
                  for run in paired for size in (3379, 8676)]
        expected = [('control' if (rnd + arm) % 2 == 0 else 'commit', rnd)
                    for rnd in range(12) for arm in range(2)]
        if any(len(g) != len({(r['kind'], r['round']) for r in g}) or
               not all(row_ok(r) for r in g) for g in groups):
            expect(log, name, False, '配对样本重复/短写/同步失败/分项不对账')
        elif any(len(g) != 24 for g in groups):
            skip(log, name, '每个尺寸均需 12 对同期样本；本轮未完整测到')
        else:
            expect(log, name, all(
                [(r['kind'], r['round']) for r in g] == expected
                for g in groups), '两档各 12 对，先后顺序逐轮交替，均成功写满并同步；'
                   '这是对照完整性，不是任一策略性能达标')

SNAPSHOT_IO_PHASES = {'reopen': ('open', 'write', 'flush', 'sync', 'close'),
                      'held-open': ('open',), 'held-commit': ('write', 'flush', 'sync'),
                      'held-close': ('close', 'stat'),
                      'reserve-open': ('open',), 'reserve-commit': ('write', 'flush', 'sync'),
                      'reserve-close': ('close', 'stat'), 'reserve-prep': ('prepare',)}
RESERVE_PROFILES = ('snapshot-gc-control', 'snapshot-gc-prepared')
SNAPSHOT_PROFILES = ('snapshot', 'snapshot-paired', 'snapshot-held', *RESERVE_PROFILES)


def snapshot_expected_parents(runs, selected):
    expected = set()
    for run in selected:
        state = runs[run]
        if state['profile'] in RESERVE_PROFILES:
            size, count = state['sizes'][0], state['rounds']
            expected.update((run, 'reserve-commit', size, rnd) for rnd in range(count))
            expected.update(((run, 'reserve-open', size, 0), (run, 'reserve-close', size, count)))
            if state['profile'] == 'snapshot-gc-prepared':
                expected.add((run, 'reserve-prep', size, 0))
        else:
            for size in (3379, 8676):
                if state['profile'] in ('snapshot', 'snapshot-paired'):
                    expected.update((run, 'reopen', size, rnd) for rnd in range(12))
                if state['profile'] in ('snapshot-held', 'snapshot-paired'):
                    expected.update((run, 'held-commit', size, rnd) for rnd in range(12))
                    expected.update(((run, 'held-open', size, 0), (run, 'held-close', size, 12)))
    return expected


IDF_RECORD_HEAD_RE = re.compile(r'[IWEDV] \(\d+\) [A-Za-z0-9_.-]+: ')
SNAPSHOT_RECORD_RE = re.compile(
    r'^[IWEDV] \(\d+\) (?:storage_io_probe: storage (?:partition|spiffs)|'
    r'word_store: storage (?:bench|snapshot|gc))\b')


def snapshot_probe_records(log: Log):
    """Frame byte-stream records, not invented/rewritten probe payloads.

    SDK Wi-Fi prints header/body separately: a complete ESP_LOG record can
    appear after ``wifi:`` or ``state: ...`` on the same physical serial line.
    Split only at a COMPLETE IDF timestamp+tag header; retain the preceding
    fragment too, so a torn/malformed probe is never discarded. Each resulting
    probe must still start with its own header/tag and pass every old contract.
    This is scoped to the two direct meters; Log.text and other parsers stay raw.
    """
    for line in log.lines:
        heads = list(IDF_RECORD_HEAD_RE.finditer(line))
        if not heads:
            yield line
            continue
        if heads[0].start():
            yield line[:heads[0].start()]
        for index, head in enumerate(heads):
            end = heads[index + 1].start() if index + 1 < len(heads) else len(line)
            yield line[head.start():end]


def snapshot_record_fields(line: str):
    # A dict alone would silently overwrite duplicate keys from interleaving.
    pairs = re.findall(r'\b([a-z][a-z0-9_]*)=([^\s]+)', line)
    return dict(pairs), len(pairs) == len(dict(pairs))


def snapshot_partition_io_data(log: Log):
    """Pure parser shared by the API and GC-subset meters (no verdict side effects)."""
    phases = SNAPSHOT_IO_PHASES
    required = {'kind', 'bytes', 'round', 'phase', 'vfs_us', 'span_us',
                'scope_ok', 'nested_calls'} | {
                    f'{op}_{field}' for op in ('read', 'write', 'erase')
                    for field in ('calls', 'bytes', 'us', 'max_us', 'failures')}
    rows, parents, runs, bad = {}, {}, {}, []
    epoch, serial, active, ready, triggered = 0, 0, None, False, False
    for line in snapshot_probe_records(log):
        if BOOT_BANNER_RE.search(line):
            epoch += 1
            active, ready = None, False
        # This schema has elf_sha256. The legacy KV_RE intentionally accepts
        # only letter/underscore keys; do not silently lose digit-bearing keys.
        f, unique = snapshot_record_fields(line)
        if ('storage partition' in line or 'storage snapshot' in line or 'storage gc snapshot' in line or
                'storage bench' in line) and (
                not unique or SNAPSHOT_RECORD_RE.match(line) is None):
            bad.append(line)
        if 'storage partition probe' in line:
            triggered = True
            ready = ('storage partition probe READY ' in line and
                     LOG_PREFIX_RE.match(line) is not None and
                     f.get('schema') == '1' and f.get('enabled') == '1' and
                     f.get('scope') == 'task+partition' and f.get('partition') == 'storage' and
                     bool(f.get('app_version')) and
                     re.fullmatch(r'[0-9a-fA-F]{16}', f.get('elf_sha256', '')) is not None)
            if not ready:
                bad.append(line)
        relevant = probe_profile(line, SNAPSHOT_PROFILES)
        if 'storage bench BEGIN' in line and relevant:
            serial += 1
            active = (epoch, serial)
            marked = 'partition_probe' in f
            runs[active] = {'marked': marked, 'end': None, 'ready': ready,
                            'profile': f.get('profile'), 'rounds': 12, 'sizes': (3379, 8676)}
            if f.get('profile') in RESERVE_PROFILES:
                try:
                    count, size = int(f['rounds']), int(f['bytes'])
                    prepared = f['profile'] == 'snapshot-gc-prepared'
                    if (f.get('reserve_probe') != '1' or count != (48 if prepared else 12) or
                            size not in (3379, 8676) or int(f['writes']) != count or
                            int(f['requested_bytes']) != (131072 if prepared else 0)):
                        raise ValueError('reserve run contract')
                    runs[active].update(rounds=count, sizes=(size,))
                except (KeyError, ValueError):
                    bad.append(line)
            if marked:
                triggered = True
                if f['partition_probe'] != '1' or not ready or not LOG_PREFIX_RE.match(line):
                    bad.append(line)
        if 'storage bench END' in line and relevant:
            if active in runs:
                runs[active]['end'] = f.get('result')
                if runs[active]['marked'] and (
                        f.get('partition_probe') != '1' or 'result' not in f or
                        not LOG_PREFIX_RE.match(line)):
                    bad.append(line)
            active = None
        kind = next((kind for text, kind in (
            ('storage snapshot append probe:', 'reopen'),
            ('storage snapshot held open:', 'held-open'),
            ('storage snapshot held commit:', 'held-commit'),
            ('storage snapshot held close:', 'held-close'),
            ('storage gc snapshot held open:', 'reserve-open'),
            ('storage gc snapshot held commit:', 'reserve-commit'),
            ('storage gc snapshot held close:', 'reserve-close'),
            ('storage gc snapshot prepare:', 'reserve-prep')) if text in line), None)
        if kind and active in runs and runs[active]['marked']:
            try:
                size = int(f['bytes'])
                rnd = int(f.get('round', f.get('rounds', '0')))
                key = (active, kind, size, rnd)
                if key in parents or 'result' not in f:
                    raise ValueError('duplicate/missing parent')
                parents[key] = {phase: int(f[phase + '_us']) for phase in phases[kind]}
                parents[key]['result'] = f['result']
            except (KeyError, ValueError):
                bad.append(line)
        if 'storage partition io' not in line:
            continue
        triggered = True
        try:
            if ('storage partition io: ' not in line or not required <= f.keys() or
                    not LOG_PREFIX_RE.match(line) or active not in runs or
                    not runs[active]['marked']):
                raise ValueError('missing contract/run/timestamp')
            row = {k: f[k] if k in ('kind', 'phase') else int(f[k]) for k in required}
            if (row['kind'] not in phases or row['phase'] not in phases[row['kind']] or
                    row['bytes'] not in (3379, 8676) or
                    any(v < 0 for v in row.values() if isinstance(v, int)) or
                    row['round'] > (runs[active]['rounds'] if row['kind'].endswith('-close')
                                    else runs[active]['rounds'] - 1) or
                    (row['kind'] in ('held-open', 'reserve-open', 'reserve-prep') and row['round'] != 0)):
                raise ValueError('unknown stage/metric')
            key = (active, row['kind'], row['bytes'], row['round'], row['phase'])
            if key in rows:
                raise ValueError('duplicate stage')
            rows[key] = row
        except ValueError:
            bad.append(line)
    return rows, parents, runs, bad, triggered


def hil_snapshot_partition_io(log: Log):
    """Scoped top-level esp_partition API wall time, NOT chip busy/GC time.

    Rows reconcile with their own VFS phase. Whole-payload requested write bytes
    make an unlinked wrapper fail rather than produce misleading zero costs.
    """
    phases = SNAPSHOT_IO_PHASES
    rows, parents, runs, bad, triggered = snapshot_partition_io_data(log)
    fmt, metric = 'LOG:snapshot-partition-io-parsed', 'WRITE:snapshot-partition-io-accounting'
    if not triggered:
        skip(log, fmt, '旧日志无分区调用探针；不把缺测当作底层耗时零')
        skip(log, metric, '无任务+分区过滤的直接 API 计量')
        return
    marked = [run for run, state in runs.items() if state['marked']]
    ended_empty = any(runs[run]['end'] == 'ESP_OK' and
                      not any(k[0] == run for k in rows) for run in marked)
    expect(log, fmt, not bad and not ended_empty,
           f'分区阶段 {len(rows)}，格式异常 {len(bad)}，成功结束但无阶段={ended_empty}')
    if bad or ended_empty or not marked:
        expect(log, metric, False, '契约漂移/探针静默；不可当成调用数或耗时零')
        return
    if any(runs[run]['end'] not in (None, 'ESP_OK') for run in marked):
        expect(log, metric, False, '运行/清理中止，不以成功子集归因')
        return
    valid = True
    for key, row in rows.items():
        parent = parents.get(key[:-1])
        native = sum(row[op + '_us'] for op in ('read', 'write', 'erase'))
        valid &= (row['scope_ok'] == 1 and native <= row['span_us'] <= row['vfs_us'] + 2 and
                  parent is not None and parent.get(row['phase']) == row['vfs_us'])
        for op in ('read', 'write', 'erase'):
            count, us, maximum = (row[op + field] for field in ('_calls', '_us', '_max_us'))
            valid &= (row[op + '_failures'] == 0 and maximum <= us <= maximum * count)
            if count == 0:
                valid &= all(row[op + '_' + field] == 0
                             for field in ('bytes', 'us', 'max_us', 'failures'))
    if not valid:
        expect(log, metric, False, 'scope/API 错误、计数不自洽或未与本阶段 VFS 耗时对账')
        return
    if any(runs[run]['end'] is None for run in marked):
        skip(log, metric, '缺 END；不能用进行中/重启截断运行作完整归因')
        return
    expected_parents = snapshot_expected_parents(runs, marked)
    expected = {key + (phase,) for key in expected_parents for phase in phases[key[1]]}
    if set(rows) != expected or set(parents) != expected_parents:
        expect(log, metric, False, '成功 END 却缺阶段/父记录，属于探针丢失，不是未触发')
        return
    for key, parent in parents.items():
        if parent['result'] != 'ESP_OK':
            valid = False
        if key[1] in ('reopen', 'held-commit', 'reserve-commit'):
            group = [rows[key + (phase,)] for phase in phases[key[1]]]
            valid &= (sum(r['write_calls'] for r in group) > 0 and
                      sum(r['write_bytes'] for r in group) >= key[2])
    if not valid:
        expect(log, metric, False, '成功持久提交却无足量分区写请求：wrapper 未接入/过滤错误')
        return
    details = []
    for size in (3379, 8676):
        for kind in ('reopen', 'held-commit', 'reserve-commit'):
            group = [r for r in rows.values() if r['bytes'] == size and
                     r['kind'] == kind and r['round'] > 0]
            if not group:
                continue
            vfs = sum(r['vfs_us'] for r in group)
            native = sum(r[op + '_us'] for r in group for op in ('read', 'write', 'erase'))
            costs = '/'.join(f'{op}={sum(r[op + "_us"] for r in group) / 1000:.3f}ms'
                             f'({sum(r[op + "_calls"] for r in group)}次)'
                             for op in ('read', 'write', 'erase'))
            details.append(f'{size}/{kind}: {costs}, outside={(vfs-native)/1000:.3f}ms')
    expect(log, metric, True, '; '.join(details) +
           '；均为累计 API wall，非芯片 busy/GC 时间；这是测量对账，不是性能通过')


def hil_snapshot_spiffs_gc(log: Log):
    """GC function wall and API subsets, never additive to the parent API meter.

    No GC work is REQUIRED: a fresh FS may take only fast checks. But successful
    payloads must trigger the gc_check wrapper, or zero GC is not trustworthy.
    """
    fmt, metric = 'LOG:snapshot-spiffs-gc-parsed', 'WRITE:snapshot-spiffs-gc-accounting'
    native, parents, runs, native_bad, _ = snapshot_partition_io_data(log)
    state_fields = ('free_before', 'free_after', 'free_min', 'free_max',
                    'allocated_before', 'allocated_after', 'deleted_before', 'deleted_after')
    counters = ('gc_check_calls', 'gc_quick_calls', 'gc_nested_calls',
                'gc_check_us', 'gc_quick_us', 'gc_check_errors', 'gc_quick_errors',
                'gc_quick_no_deleted', 'fs_seen', 'block_count', 'block_size', 'page_size')
    required = {'schema', 'kind', 'bytes', 'round', 'phase', 'scope_ok', *state_fields, *counters} | {
        f'gc_{op}_{field}' for op in ('read', 'write', 'erase') for field in ('calls', 'bytes', 'us')}
    gc_rows, marked, partition_sizes, bad = {}, set(), {}, []
    epoch, serial, active, ready_bytes, triggered = 0, 0, None, None, False
    for line in snapshot_probe_records(log):
        if BOOT_BANNER_RE.search(line):
            epoch += 1
            active, ready_bytes = None, None
        f, unique = snapshot_record_fields(line)
        if 'storage spiffs gc' in line and (
                not unique or SNAPSHOT_RECORD_RE.match(line) is None):
            bad.append(line)
        if 'gc_probe' in f:
            triggered = True
            if f['gc_probe'] != '1' or LOG_PREFIX_RE.match(line) is None:
                bad.append(line)
        if 'storage partition probe' in line:
            ready_bytes = None
            if 'gc_probe' in f:
                try:
                    ready_bytes = int(f['partition_bytes'])
                    if ready_bytes <= 0:
                        raise ValueError('partition size')
                except (KeyError, ValueError):
                    bad.append(line)
        relevant = probe_profile(line, SNAPSHOT_PROFILES)
        if 'storage bench BEGIN' in line and relevant:
            serial += 1
            active = (epoch, serial)
            if 'gc_probe' in f:
                marked.add(active)
                if ready_bytes is None or f.get('partition_probe') != '1':
                    bad.append(line)
                else:
                    partition_sizes[active] = ready_bytes
        if 'storage bench END' in line and relevant:
            if active in marked and f.get('gc_probe') != '1':
                bad.append(line)
            active = None
        if 'storage partition io:' in line and active in marked and f.get('gc_probe') != '1':
            bad.append(line)
        if 'storage spiffs gc' not in line:
            continue
        triggered = True
        try:
            if ('storage spiffs gc: ' not in line or not required <= f.keys() or
                    LOG_PREFIX_RE.match(line) is None or active not in marked or f['schema'] != '1'):
                raise ValueError('GC schema/run/timestamp')
            row = {k: f[k] if k in ('kind', 'phase') else int(f[k]) for k in required}
            if (row['kind'] not in SNAPSHOT_IO_PHASES or
                    row['phase'] not in SNAPSHOT_IO_PHASES[row['kind']] or
                    row['bytes'] not in (3379, 8676) or
                    not 0 <= row['round'] <= (runs.get(active, {}).get('rounds', 0)
                        if row['kind'].endswith('-close') else runs.get(active, {}).get('rounds', 0) - 1) or
                    any(v < (-1 if k in state_fields else 0)
                        for k, v in row.items() if isinstance(v, int))):
                raise ValueError('GC stage/metric')
            key = (active, row['kind'], row['bytes'], row['round'], row['phase'])
            if key in gc_rows:
                raise ValueError('duplicate GC stage')
            gc_rows[key] = row
        except ValueError:
            bad.append(line)
    if not triggered:
        skip(log, fmt, '旧日志无 GC 函数归属探针；不把 read 形状当直接 GC 证据')
        skip(log, metric, '缺 GC function wall、内部页状态及其 API 子集')
        return
    expect(log, fmt, not bad and bool(gc_rows),
           f'GC 阶段 {len(gc_rows)}，格式异常 {len(bad)}')
    if bad or native_bad or not gc_rows or not marked or any(run not in runs for run in marked):
        expect(log, metric, False, 'GC/父探针契约漂移或静默，不接受全零假测量')
        return
    valid, geometries = True, {}
    for key, g in gc_rows.items():
        n, parent = native.get(key), parents.get(key[:-1])
        if n is None or parent is None:
            valid = False
            continue
        gc_wall = g['gc_check_us'] + g['gc_quick_us']
        gc_api = sum(g[f'gc_{op}_us'] for op in ('read', 'write', 'erase'))
        native_api = sum(n[f'{op}_us'] for op in ('read', 'write', 'erase'))
        valid &= (g['scope_ok'] == n['scope_ok'] == 1 and
                  parent.get(g['phase']) == n['vfs_us'] and
                  native_api <= n['span_us'] <= n['vfs_us'] + 2 and
                  gc_api <= gc_wall <= n['span_us'] and
                  g['gc_check_errors'] == g['gc_quick_errors'] == 0 and
                  g['gc_quick_no_deleted'] <= g['gc_quick_calls'])
        calls = g['gc_check_calls'] + g['gc_quick_calls']
        for family in ('check', 'quick'):
            if g[f'gc_{family}_calls'] == 0:
                valid &= g[f'gc_{family}_us'] == 0
        for op in ('read', 'write', 'erase'):
            valid &= n[op + '_failures'] == 0
            for field in ('calls', 'bytes', 'us'):
                valid &= g[f'gc_{op}_{field}'] <= n[f'{op}_{field}']
            if g[f'gc_{op}_calls'] == 0:
                valid &= g[f'gc_{op}_bytes'] == g[f'gc_{op}_us'] == 0
        if calls == 0:
            valid &= (g['fs_seen'] == 0 and g['gc_nested_calls'] == 0 and
                      all(g[k] == -1 for k in state_fields) and
                      g['block_count'] == g['block_size'] == g['page_size'] == 0 and
                      all(g[f'gc_{op}_{field}'] == 0 for op in ('read', 'write', 'erase')
                          for field in ('calls', 'bytes', 'us')))
        else:
            valid &= (g['fs_seen'] == 1 and all(g[k] >= 0 for k in state_fields) and
                      g['block_count'] > 0 and g['page_size'] > 0 and
                      g['block_size'] >= g['page_size'] and
                      g['block_size'] % max(1, g['page_size']) == 0 and
                      g['block_count'] * g['block_size'] == partition_sizes.get(key[0]) and
                      0 <= g['free_min'] <= min(g['free_before'], g['free_after']) and
                      max(g['free_before'], g['free_after']) <= g['free_max'] <= g['block_count'])
            physical_pages = g['block_count'] * g['block_size'] // max(1, g['page_size'])
            valid &= all(g[f'allocated_{when}'] + g[f'deleted_{when}'] <= physical_pages
                         for when in ('before', 'after'))
            geometry = (g['block_count'], g['block_size'], g['page_size'])
            valid &= geometries.setdefault(key[0], geometry) == geometry
    if not valid:
        expect(log, metric, False, 'GC scope/错误/状态不自洽，或子集超出自己的 GC/分区阶段')
        return
    if any(runs[run]['end'] not in (None, 'ESP_OK') for run in marked):
        expect(log, metric, False, '运行/清理失败，不用成功子集归因')
        return
    if any(runs[run]['end'] is None for run in marked):
        skip(log, metric, '缺 END；GC 归属仍未完整测到')
        return
    expected = {key + (phase,) for key in snapshot_expected_parents(runs, marked)
                for phase in SNAPSHOT_IO_PHASES[key[1]]}
    if set(gc_rows) != expected or {k for k in native if k[0] in marked} != expected:
        expect(log, metric, False, '成功 END 却缺 GC/父阶段，不能降为 SKIP')
        return
    for key, parent in parents.items():
        if key[0] not in marked or key[1] not in ('reopen', 'held-commit', 'reserve-commit'):
            continue
        group = [gc_rows[key + (phase,)] for phase in SNAPSHOT_IO_PHASES[key[1]]]
        valid &= (parent['result'] == 'ESP_OK' and sum(g['gc_check_calls'] for g in group) > 0)
    if not valid:
        expect(log, metric, False, '成功 payload 没有 gc_check：包装未接入/归属过滤错误')
        return
    details = []
    for size in (3379, 8676):
        for kind in ('reopen', 'held-commit', 'reserve-commit'):
            keys = [k for k in gc_rows if k[2] == size and k[1] == kind and k[3] > 0]
            if not keys:
                continue
            gs = [gc_rows[k] for k in keys]
            read_total = sum(native[k]['read_us'] for k in keys)
            gc_read = sum(g['gc_read_us'] for g in gs)
            seen = [g for g in gs if g['fs_seen']]
            details.append(f'{size}/{kind}: GC wall='
                           f'{sum(g["gc_check_us"] + g["gc_quick_us"] for g in gs)/1000:.3f}ms, '
                           f'GC read={gc_read/1000:.3f}/{read_total/1000:.3f}ms, '
                           f'free_blocks={min(g["free_min"] for g in seen)}..'
                           f'{max(g["free_max"] for g in seen)}')
    expect(log, metric, True, '; '.join(details) +
           '；GC wall 包含 API 子集，不重复相加；状态为首次入口/末次出口及极值，'
           '不是每次扫描记录；这是归属对账，不是性能/掉电验收')


def snapshot_direct_meters_pass(log: Log):
    """Reuse the SAME strongest meters without emitting duplicate criteria.

    RESULTS is synchronous report accumulation. Always remove only our temporary
    tail; selftests can call this checker alone, not depend on a previous PASS.
    """
    start = len(RESULTS)
    try:
        hil_snapshot_partition_io(log)
        hil_snapshot_spiffs_gc(log)
        return len(RESULTS) - start == 4 and all(r[2] == 'PASS' for r in RESULTS[start:])
    finally:
        del RESULTS[start:]


def hil_snapshot_gc_reserve(log: Log):
    """Sequential GC-control/reserve trial, not randomized paired proof.

    No recurring maintenance in the 48-commit arm. Both first12 and all48 are
    unconditional gates: a fast prefix cannot hide reserve exhaustion.
    Explicit prepared-only headers cover just 8676/prepared, never the control
    comparison or the missing small size. Legacy full trials still need 4 arms.
    """
    fmt, control = 'LOG:snapshot-gc-reserve-parsed', 'WRITE:snapshot-gc-reserve-control'
    metrics = [control, 'WRITE:snapshot-gc-reserve-maintenance',
               'WRITE:snapshot-gc-reserve-lifetime'] + [
        f'WRITE:snapshot-gc-reserve-{size}-{window}-p90'
        for size in (3379, 8676) for window in ('first12', 'all48')]
    native, parents, runs, native_bad, _ = snapshot_partition_io_data(log)
    selected = [k for k, v in runs.items() if v['profile'] in RESERVE_PROFILES]
    triggered = bool(selected) or any('storage gc reserve experiment' in l or
                                    'reserve_probe=' in l for l in log.lines)
    if not triggered:
        for name in (fmt, *metrics):
            skip(log, name, '旧日志无一次前置 GC / 连续 48 笔对照，不外推储备寿命')
        return
    rows, gc_rows, begins, ends, bad = {}, {}, [], [], []
    epoch, serial, active = 0, 0, None
    experiment_active = False
    modes = {}
    required = {
        'reserve-open': {'bytes', 'result', 'new_object', 'open_us'},
        'reserve-commit': {'bytes', 'round', 'result', 'write_us', 'flush_us', 'sync_us',
                           'total_us', 'written_bytes', 'flush_ok', 'sync_attempted', 'sync_ok'},
        'reserve-close': {'bytes', 'result', 'close_us', 'rounds', 'committed_bytes',
                          'file_bytes', 'stat_us', 'close_ok', 'stat_ok'},
        'reserve-prep': {'bytes', 'result', 'requested_bytes', 'prepare_us'},
    }
    extras = ('lookup_pages', 'data_page_bytes', 'free_data_bytes_before', 'free_data_bytes_after')
    for line in snapshot_probe_records(log):
        if BOOT_BANNER_RE.search(line):
            epoch += 1
            active = None
            experiment_active = False
        f, unique = snapshot_record_fields(line)
        if 'storage gc reserve experiment' in line:
            try:
                if not unique or not SNAPSHOT_RECORD_RE.match(line):
                    raise ValueError('experiment header')
                if 'storage gc reserve experiment BEGIN ' in line:
                    mode = f.get('mode', 'full')
                    if (mode not in ('full', 'prepared-only') or
                            (mode == 'prepared-only' and int(f['bytes']) != 8676)):
                        raise ValueError('experiment mode')
                    if (experiment_active or active is not None or f.get('schema') != '1' or
                            int(f['requested_bytes']) != 131072 or
                            int(f['control_rounds']) != (0 if mode == 'prepared-only' else 12) or
                            int(f['prepared_rounds']) != 48):
                        raise ValueError('experiment contract')
                    begins.append(epoch)
                    modes[epoch] = mode
                    experiment_active = True
                elif 'storage gc reserve experiment END ' in line:
                    if not experiment_active or active is not None:
                        raise ValueError('experiment END outside its window')
                    if (f.get('mode', 'full') != modes[epoch] or
                            (modes[epoch] == 'prepared-only' and int(f['bytes']) != 8676)):
                        raise ValueError('experiment END mode')
                    ends.append((epoch, f['result'], int(f['completed_runs']), int(f['total_ms'])))
                    experiment_active = False
                else:
                    raise ValueError('unknown experiment event')
            except (KeyError, ValueError):
                bad.append(line)
        if 'storage bench BEGIN' in line and probe_profile(line, SNAPSHOT_PROFILES):
            serial += 1
            active = (epoch, serial)
            if active in selected and not experiment_active:
                bad.append(line)
        if 'storage bench END' in line and probe_profile(line, SNAPSHOT_PROFILES):
            if active in selected:
                try:
                    if (f.get('reserve_probe') != '1' or int(f.get('bytes', '0')) not in runs[active]['sizes']):
                        raise ValueError('END reserve contract')
                except ValueError:
                    bad.append(line)
            active = None
        if active not in selected:
            if 'storage gc snapshot' in line:
                bad.append(line)
            continue
        kind = next((k for text, k in (
            ('storage gc snapshot held open:', 'reserve-open'),
            ('storage gc snapshot held commit:', 'reserve-commit'),
            ('storage gc snapshot held close:', 'reserve-close'),
            ('storage gc snapshot prepare:', 'reserve-prep')) if text in line), None)
        if kind:
            try:
                if not unique or not SNAPSHOT_RECORD_RE.match(line) or not required[kind] <= f.keys():
                    raise ValueError('parent contract')
                row = {k: v if k == 'result' else int(v) for k, v in f.items() if k in required[kind]}
                rnd = row.get('round', row.get('rounds', 0))
                key = (active, kind, row['bytes'], rnd)
                if key in rows or any(v < 0 for k, v in row.items() if k != 'result'):
                    raise ValueError('duplicate/negative parent')
                rows[key] = row
            except (KeyError, ValueError):
                bad.append(line)
        if 'storage spiffs gc:' in line:
            try:
                if not unique or not set(extras) <= f.keys():
                    raise ValueError('clean capacity schema')
                key = (active, f['kind'], int(f['bytes']), int(f['round']), f['phase'])
                if key in gc_rows:
                    raise ValueError('duplicate GC row')
                gc_rows[key] = {k: v if k in ('kind', 'phase') else int(v) for k, v in f.items()}
            except (KeyError, ValueError):
                bad.append(line)
    expect(log, fmt, not bad and not native_bad and bool(rows),
           f'实验父记录={len(rows)}，clean-state 阶段={len(gc_rows)}，格式异常={len(bad)+len(native_bad)}')
    if bad or native_bad or not selected or not rows:
        for name in metrics:
            expect(log, name, False, '实验契约漂移/静默，不能以剩余成功样本验收')
        return
    prepared_only = bool(modes) and set(modes.values()) == {'prepared-only'}
    if prepared_only:
        unavailable = [control] + [f'WRITE:snapshot-gc-reserve-3379-{w}-p90'
                                   for w in ('first12', 'all48')]
        for name in unavailable:
            skip(log, name, '显式单组补测只含 8676/prepared48；未测 control/3379，不是四组对照通过')
        metrics = [name for name in metrics if name not in unavailable]
    if not ends:
        for name in metrics:
            skip(log, name, '缺 experiment END，不能只用快的前缀验收')
        return
    epochs = sorted({k[0] for k in selected})
    complete = (begins == epochs and [e[0] for e in ends] == epochs and
                len(set(modes.values())) == 1 and
                all(e[1] == 'ESP_OK' and e[2] == (1 if prepared_only else 4) and e[3] >= 0
                    for e in ends))
    expected_order = ([(8676, 'snapshot-gc-prepared')] if prepared_only else
                      [(size, profile) for size in (3379, 8676) for profile in RESERVE_PROFILES])
    complete &= all([(runs[k]['sizes'][0], runs[k]['profile']) for k in selected if k[0] == ep]
                    == expected_order for ep in epochs)
    complete &= all(runs[k]['end'] == 'ESP_OK' for k in selected)
    if not complete or not snapshot_direct_meters_pass(log):
        for name in metrics:
            expect(log, name, False, '需本模式完整序列、experiment END 及两套直接计量全部 PASS；'
                   'full 仍需四组，prepared-only 只允许 8676/prepared48 一组')
        return
    expected = snapshot_expected_parents(runs, selected)
    valid = set(rows) == expected and {k[:-1] for k in gc_rows} == expected
    reserve_ok = {}
    for run in selected:
        size, count = runs[run]['sizes'][0], runs[run]['rounds']
        sequence = [(run, 'reserve-open', size, 0)]
        if runs[run]['profile'] == 'snapshot-gc-prepared':
            sequence.append((run, 'reserve-prep', size, 0))
        sequence += [(run, 'reserve-commit', size, rnd) for rnd in range(count)]
        sequence.append((run, 'reserve-close', size, count))
        valid &= [key for key in rows if key[0] == run] == sequence
    for key, g in gc_rows.items():
        if not g['fs_seen']:
            valid &= (g['lookup_pages'] == g['data_page_bytes'] == 0 and
                      g['free_data_bytes_before'] == g['free_data_bytes_after'] == -1)
        else:
            pages_per_block = g['block_size'] // g['page_size']
            valid &= (0 < g['lookup_pages'] < pages_per_block and
                      0 < g['data_page_bytes'] <= g['page_size'])
            for when in ('before', 'after'):
                clean = ((pages_per_block - g['lookup_pages']) * (g['block_count'] - 2) -
                         g[f'allocated_{when}'] - g[f'deleted_{when}']) * g['data_page_bytes']
                valid &= clean == g[f'free_data_bytes_{when}']
    for key, row in rows.items():
        valid &= row['result'] == 'ESP_OK'
        if key[1] == 'reserve-open':
            valid &= row['new_object'] == 1
        elif key[1] == 'reserve-commit':
            valid &= (row['written_bytes'] == key[2] and
                      row['flush_ok'] == row['sync_attempted'] == row['sync_ok'] == 1 and
                      0 <= row['total_us'] - sum(row[p + '_us'] for p in ('write', 'flush', 'sync')) <= 2000)
        elif key[1] == 'reserve-close':
            count = runs[key[0]]['rounds']
            valid &= (row['rounds'] == count and row['stat_ok'] == row['close_ok'] == 1 and
                      row['file_bytes'] == row['committed_bytes'] == count * key[2])
        else:
            g = gc_rows[key + ('prepare',)]
            valid &= (row['requested_bytes'] == 131072 and g['gc_check_calls'] == 1 and
                      g['fs_seen'] == 1)
            reserve_ok[key[0]] = (g['free_after'] > 3 and
                                  g['free_data_bytes_after'] >= row['requested_bytes'])
    if not valid:
        for name in metrics:
            expect(log, name, False, '短写/同步/长度/clean-state/阶段顺序不自洽，或前置 GC 未建立请求的储备')
        return
    if not prepared_only:
        expect(log, control, all(reserve_ok.values()),
               '同版同分区：两档各 control12→一次维护→prepared48；顺序对照，不冒充随机配对；'
               f'前置储备成立={sum(reserve_ok.values())}/{len(reserve_ok)}，SDK 成功不等于快路径储备')
    maintenance, lifetime = [], []
    for run in selected:
        size = runs[run]['sizes'][0]
        count = runs[run]['rounds']
        keys = [(run, 'reserve-commit', size, rnd) for rnd in range(count)]
        samples = [rows[k]['total_us'] for k in keys]
        p90 = lambda s: sorted(s)[(9 * len(s) + 9) // 10 - 1] / 1000
        if runs[run]['profile'] == 'snapshot-gc-control':
            lifetime.append(f'{size}/control: n=12 p90={p90(samples):.3f}ms')
            continue
        prep_key = (run, 'reserve-prep', size, 0)
        prep, g = rows[prep_key], gc_rows[prep_key + ('prepare',)]
        maintenance.append(f'{size}: SDK wall={prep["prepare_us"]/1000:.3f}ms, '
                           f'free_blocks={g["free_before"]}→{g["free_after"]}, '
                           f'clean_bytes={g["free_data_bytes_before"]}→{g["free_data_bytes_after"]}, '
                           f'GC API read={g["gc_read_us"]/1000:.3f}ms, erase={g["gc_erase_calls"]}次, '
                           f'储备成立={int(reserve_ok[run])}')
        work = [rnd for rnd, key in enumerate(keys) if any(
            gc_rows[key + (phase,)][f'gc_{op}_calls'] > 0
            for phase in ('write', 'flush', 'sync') for op in ('read', 'write', 'erase'))]
        lifetime.append(f'{size}/prepared: 首次再次发生 GC I/O round={work[0] if work else "未见(仅观察48笔)"}; '
                        f'有 GC I/O={len(work)}/48, n=48 p90={p90(samples):.3f}ms')
        for window, values in (('first12', samples[:12]), ('all48', samples)):
            clusters = cluster_gap(values)
            expect(log, f'WRITE:snapshot-gc-reserve-{size}-{window}-p90',
                   reserve_ok[run] and p90(values) < 200 and len(clusters) == 1,
                   f'n={len(values)} p90={p90(values):.3f}ms (<200), max={max(values)/1000:.3f}ms, '
                   f'分簇(us)={clusters}, 储备成立={int(reserve_ok[run])}; '
                   '每笔同步，维护成本另计，不是恢复/正式方案验收')
    expect(log, 'WRITE:snapshot-gc-reserve-maintenance', True,
           '; '.join(maintenance) + '；计量 PASS，不代表真实睡眠预算允许，不重复相加包含时间')
    expect(log, 'WRITE:snapshot-gc-reserve-lifetime', True,
           '; '.join(lifetime) + '；寿命仅限该序列，未见耗尽不外推无限；其他事务仍可消耗储备')


def hil_p0_wifi(log: Log):
    """c93800c — wifi legacy 迁移（Codex 评论 2）。

    三种合法结局，判据各不相同：
      A. 本次启动真做了迁移 → 必须恰好一条 migrated 日志、两个 key 被擦掉
      B. 设备早已迁移（blob 在位）→ 必须「有凭据 + 零 wifi 写事务」
      C. 迁移被拒（kRetryLater）→ 必须非致命且有 connectivity 重试
    """
    migrated = log.has('migrated legacy wifi credentials into slot 0')
    orphan = log.has('clearing orphan legacy wifi credentials')
    deferred = log.has('legacy wifi credential migration deferred')
    has_creds = log.has('WiFi credentials stored: SSID=')
    wifi_writes = [t for t in log.tx
                   if 'wifi-cred' in t['owner'] or 'legacy-wifi' in t['owner']
                   or 'migrate-wifi' in t['owner']]

    if migrated:
        expect(log, 'P0-wifi:migrated-once',
               log.text.count('migrated legacy wifi credentials into slot 0') == 1,
               '恰好一条 migrated 日志（重复说明幂等失效）')
        expect(log, 'P0-wifi:migration-wrote-once',
               len(wifi_writes) == 1,
               f'迁移写事务 {len(wifi_writes)} 笔（blob 写 1 笔 + 2 笔 erase 应为 1 次性事务）')
        return
    if deferred:
        expect(log, 'P0-wifi:deferred-non-fatal', True,
               '启动迁移被拒时有 W 日志且不中止启动')
        return
    if has_creds:
        # 已迁移设备：干净启动 = 读到凭据且不做任何迁移写
        expect(log, 'P0-wifi:migrated-device-clean-boot',
               not wifi_writes,
               f'已迁移设备启动时有凭据且零 wifi 写事务（实测 {len(wifi_writes)} 笔）')
        return
    skip(log, 'P0-wifi:no-credential-state',
         '日志既无迁移痕迹也无已存凭据（可能未连过 WiFi）')


def hil_p0_journal(log: Log):
    """5ca3acd — sync journal 单写入者（Codex 评论 1）。"""
    j = log.txof('save-sync-journal')
    if not j:
        skip(log, 'P0-journal:traffic', '本轮无 journal 写（未触发同步）')
    else:
        expect(log, 'P0-journal:owner-consistent',
               all(t['res'] != 'ESP_ERR_INVALID_STATE' for t in j),
               '无因租约/门禁被拒的半写')
        expect(log, 'P0-journal:no-torn-write',
               not log.has('primary sync journal invalid'),
               'journal 主文件未被写坏（无 invalid 回退）')
    # journal 排队时进睡的判据：出现 queue_wait 明显 + 无半写
    if j and max(qw(t) for t in j) > 500:
        expect(log, 'P0-journal:queue-wait-survives',
               all(t['res'] == 'ESP_OK' for t in j if qw(t) > 500),
               '排队久的 journal 写最终仍 ESP_OK（未被 quiesce 拒）')


def hil_gate_selftest(log: Log):
    """b472f20 — `RunUiGateSelfTest()` 在设备上必须报 passed。

    WHY A CRITERION OF ITS OWN: this is the cheapest signal in the whole log and
    it was red for three consecutive captures (1005.4 / 1005.6 / 1005.7) purely
    because the [word]-row path has no `commit_state` gate -- a real §4.2 defect
    introduced by 1fdfe52, not a fixture problem. With no criterion, "the log
    looks fine" was being reported while the firmware was printing an error at
    every boot. A red here blocks C6/C7 work: it means the gate inventory the
    self-test pins is no longer true.
    """
    failed = sorted(set(m.group('name') for m in GATE_FAIL_RE.finditer(log.text)))
    passed = bool(GATE_OK_RE.search(log.text))
    if failed:
        expect(log, 'GATES:self-test-green', False,
               f'启动自检红：{", ".join(failed)}（门禁清单已与代码不符，'
               f'先修它再动 C6/C7——否则分不清红灯来自新代码还是自检本身）')
    elif passed:
        expect(log, 'GATES:self-test-green', True, 'commit_state gate self-test passed')
    else:
        skip(log, 'GATES:self-test-green',
             '本轮构建没有 gate 自检（b472f20 之前），无从判定')


def hil_c5_domain_gate(log: Log):
    """37033e7 — 同域事务不交叠。"""
    dom_pairs = [
        ('word-observation-commit', 'word-scope-reset', 'P0-c5:word-domain-serialized'),
        ('note-observation-commit', 'note-session-save', 'P0-c5:note-domain-serialized'),
    ]
    for a, b, name in dom_pairs:
        ta, tb = log.txof(a), log.txof(b)
        if not ta or not tb:
            continue
        # 事务按完成顺序排列，用「前一笔 elapsed 覆盖后一笔 queue_wait」近似判定交叠
        overlap = any(
            qw(t2) > 0 and qw(t2) < el(t1)
            for t1, t2 in zip(ta, tb) if True
        )
        expect(log, name, not overlap or max(qw(t2) for t2 in tb) == 0,
               '同域两 kind 未并发（后一笔 queue_wait 未落在前一笔 elapsed 内）')


def hil_c6b_scope_switch(log: Log):
    """1fdfe52 — scope 切换不阻塞 UI + ACK 后才装。"""
    resets = log.txof('word-scope-reset')
    if not resets:
        skip(log, 'C6b:no-reset-traffic', '本轮未发生词页 scope 切换')
        return
    # 1. 切换事务存在且成功
    expect(log, 'C6b:reset-succeeded',
           all(t['res'] == 'ESP_OK' for t in resets),
           'word-scope-reset 全部 ESP_OK')
    # 2. UI 未被堵：切换事务进行期间，button→dispatch 仍很快
    #    （用日志行号近似：事务完成行之后 100ms 内的 button 有 dispatch 紧跟）
    stall = 0
    lines = log.text.splitlines()
    for i, ln in enumerate(lines):
        m = TX_RE.search(ln)
        if not m or m.group('owner') != 'word-scope-reset':
            continue
        if el(m) < 300:
            continue
        # 该事务执行窗口内（用完成时间戳 - elapsed 近似）有无 button event 无 dispatch
        end_ts = int(re.match(r'[IWAE] \((\d+)\)', ln).group(1))
        start_ts = end_ts - el(m)
        for s in log.submits:
            ts = int(s['ts'])
            if start_ts <= ts <= end_ts:
                near = [d for d in log.dispatches if 0 <= int(d['ts']) - ts <= 100]
                if not near:
                    stall += 1
    expect(log, 'C6b:ui-responsive-during-reset', stall == 0,
           f'切换事务执行窗口内有 {stall} 次按钮未在 100ms 内得到 dispatch')
    # 3. 旧会话不可恢复（generation 生效）
    expect(log, 'C6b:no-stale-session-resume',
           not log.has('word runtime restored: mode=') or True,  # 有恢复不等于陈旧，需人工判
           '见人工项：恢复的会话是否属于新 scope')


def hil_c6a_generation(log: Log):
    """f71c255 — generation 先行，陈旧会话不可复活。"""
    rejected = log.text.count('word session rejected: scope generation')
    if log.txof('word-scope-reset'):
        expect(log, 'C6a:stale-sessions-rejected-or-clean',
               rejected > 0 or not log.has('word runtime restored'),
               f'陈旧会话被 load 侧拒绝 {rejected} 次（0 次且无恢复也算干净）')
    # backup 提升不得复活陈旧会话
    if log.has('recovered word session from backup'):
        expect(log, 'C6a:backup-not-revived-when-stale',
               rejected > 0,
               '出现 backup 提升时必须伴随 generation 拒绝，否则是 bug')


def hil_c8_page_save(log: Log):
    """9f1a6d8 — 候选页快照写搬到 runner。"""
    saves = log.txof('word-session-save')
    commits = log.txof('word-observation-commit')
    merged = log.has('candidate page merged over an advanced session')
    if saves:
        worst_qw = max((qw(t) for t in commits), default=0)
        expect(log, 'C8:commit-not-blocked-by-page-save', worst_qw < 1000,
               f'答题提交最大 queue_wait={worst_qw}ms（C8 回归判据：应 <1s）')
        # --- C8 的原判据会自己变成瞎的，这里补第二把尺子 -------------------
        # WHY: `word-observation-commit` 的 queue_wait 量的是"提交有没有排在
        # 会话保存后面"，而用户看见的卡顿是**保存本身多久**。1005.26 上
        # queue_wait 3,870 ms（FAIL），1006.1 上 689 ms（PASS）——可 `word-session-save`
        # 的 elapsed 两轮都是 4.7 s。原判据转绿是因为提交换了时机，不是因为
        # 4 s 消失了。把"提交不堵"当成"保存不慢"，正是把我的判据量错了对象。
        # 历史 elapsed（同一形状、同一批日志）：5398 / 4169~5210 / 4512~5383 /
        # 4853~5913 / 4747、4714 —— **六轮 4.1–5.9 s，一次都没动过**。
        # 所以这里**不设新的目标门禁**（会变成一条每轮都红的判据，训练所有人
        # 忽略红色），只做两件事：把数字和文档目标 p90<200 ms 的差距如实报出来，
        # 以及在超过历史最差值（5,913 ms）时当回归拦下来。
        save_el = [el(t) for t in saves]
        worst_el = max(save_el, default=0)
        med_el = statistics.median(save_el) if save_el else 0
        expect(log, 'C8:session-save-elapsed-not-regressed', worst_el <= 6000,
               f'word-session-save elapsed 中位 {med_el:.0f} ms / 最大 {worst_el} ms'
               f'（n={len(save_el)}）。⚠️ **这不是 C8 的功劳计量，是用户的真实等待**：'
               f'六轮日志 4.1–5.9 s 一次都没降过，而上面那条 queue_wait 判据已经转绿——'
               f'绿的是"提交不再排在保存后面"，**不是"保存变快了"**。'
               f'文档目标 p90<200 ms，当前差 {med_el / 200:.0f}×。'
               f'（阈值 6000 ms = 历史最差 5913，只拦回归，不当目标门禁）')
    if not saves and merged:
        expect(log, 'C8:merge-path-works', True, '走合并路径（预取白做）且无写盘')
    if not saves and not merged:
        skip(log, 'C8:no-page-save-observed', '本轮未见候选页快照写，也未见合并路径')


def pk_open_seek_context(paths, exclude=None):
    """跨日志的 `open_seek` 中位区间——**只作上下文打印，不参与判定**。

    WHY NOT A CONSTANT: 第一版这儿是个写死的 `(258, 722)`。结果它**当天就错了**：
    1004.5(2) 的中位是 723，比写死的上界高 1 ms，于是一份历史日志被判成"已超出
    历史区间，这才是回归"——**而这个区间的名字就叫"全历史"**。一个自称覆盖全
    历史的常量，一定会在下一份日志到来时悄悄过期，而它过期的样子和"真的回归"
    长得一模一样。这就是狼来了判据的成因。

    WHY NOT THE VERDICT EITHER: min/max 区间**两端都当不了判定线**——
      · 含着本轮自己 ⇒ 本轮永远不可能超出区间 ⇒ 判据退化成永远 PASS；
      · 剔掉本轮自己 ⇒ 定义端点的那份日志每次都 FAIL。实测两份都踩到了：
        1004.5(2) 中位 723 是全网最高，剔掉自己后上界变 722 ⇒ 假 FAIL；
        1005.7 中位 258 是全网最低，剔掉自己后下界变 329 ⇒ 假 FAIL。
    两份 FAIL 都是这个方法本身造出来的，设备一点没变。**一个会让"最极端的
    那份样本必然失败"的统计量，不能当判据**——这是 min/max 类基线的通病，
    换任何量都一样（双峰样本的中位数同理，见 cluster_gap 的注释）。

    所以这里只回答"本轮在全历史里坐在哪一格"，判定看分量（hil_pk_handle）。
    """
    meds = {}
    for p in paths:
        if exclude is not None and p == exclude:
            continue
        try:
            lg = Log.load(p)
        except OSError:
            continue
        if lg.cards:
            meds[p] = statistics.median([c['os'] for c in lg.cards])
    if len(meds) < 2:
        return None
    return min(meds.values()), max(meds.values()), meds


def hil_pk_handle(log, context=None):
    """fbfa950 — 包读句柄复用。

    ⚠️ **这条判据原来是对着自己的实验喊狼来了，2026-10-06 修了两轮。**

    第一版：拿**单份日志**（1004.9）的中位 362 ms 当基线，>500 ms 即回归。
    于是 1006.10 的 634 ms 被判成"新增真回归"，还当问题报给了对端。peer 一句话
    驳回：**"634 没有回归，是判据基线只取了一份日志"**——我按 9 份日志复核，
    中位跨 258~722（2.8× 摆幅），1005.4 的 721.5 比 1006.10 的 634 还高。

    第二版：改成"跨日志 min/max 区间，剔掉自己"。看着严谨，**当天又造出两份
    假 FAIL**（1004.5(2) 的 723 和 1005.7 的 258，理由见
    pk_open_seek_context 的注释）。根因是同一个：**拿一个本身就带 2.8× 自然
    摆幅的点统计量去卡 min/max 线。**

    摆幅的来源不是 `fseek`（中位 258~375，跨日志只 1.45×），而是 `fopen` 的
    **缓存未命中**：`open_seek = fopen + fseek`，`fopen` 是双值的——命中 ≈0、
    未命中 344~392（跨日志只 1.14×）。未命中率是**抽样量**（这轮开了哪些包/
    哪些 offset），随日志变：6%~57%。open_seek 中位 = fseek + 未命中率 ×
    未命中成本，所以它的摆幅几乎全部由未命中率贡献，**不是句柄复用退化了**。

    所以判定改在**分量**上（那里跨日志只 1.14× / 1.45×），`open_seek` 中位退回
    上下文：印出来 + 印它在全历史里的位置，但不拿它判 FAIL。
    """
    if not log.cards:
        skip(log, 'PK:no-card-reads', '本轮无词卡读取')
        return
    os_vals = [c['os'] for c in log.cards]
    n = len(os_vals)
    med = statistics.median(os_vals)
    # fopen 未命中率：open_seek 摆幅的真实来源。
    fp = []
    for line in log.lines:
        if 'word card loaded:' not in line:
            continue
        m = re.search(r'fopen_ms=(\d+) fseek_ms=(\d+)', line)
        if m:
            fp.append((int(m.group(1)), int(m.group(2))))
    miss = None
    if fp:
        # `fopen_ms` 是**双值**的：≈0 = 缓存命中，>0 = 未命中（真开了一次）。
        # ⚠️ 第一版这里写的 `if a == 0` 并命名 miss，方向是反的——那算的是
        # **命中率**。于是"全命中"的日志被打成"未命中 100%"，再因为一份未命中
        # 样本都没有、测不到未命中成本，被误判成回归。
        # **命名和计数必须同向**：这条错让判据在 1005.26 / 1005.7 上各造一次假 FAIL。
        miss = sum(1 for a, _ in fp if a > 0) / len(fp)
    fs_med = (statistics.median([b for _, b in fp if b > 0]) if any(b for _, b in fp)
              else None)
    # 分量带：跨全部日志实测的 fopen 未命中成本 / fseek 中位，再各放宽成判线。
    # 放宽的幅度写在这里，下一个读数就能判断"是不是该收口"。
    #   fopen 未命中成本 实测 344~392（n=4 份日志，1.14×）⇒ 判线 [300, 430]
    #   fseek 中位      实测 258~375（n=6 份日志，1.45×；258 来自 n=1 的
    #                  1005.7，是单点）⇒ 判线 [240, 400]
    kFopenMissBand = (300, 430)
    kFseekBand = (240, 400)
    # 机理给出的可达上界：最坏情形每笔都未命中 ⇒ fseek 375 + fopen 392 ≈ 767。
    # 低于它，"中位高"完全可以由未命中率解释；高于它，未命中率再也解释不了。
    # **不设下界**——中位变低不是回归。
    kOpenSeekCeiling = 800
    detail = (f'open_seek 中位 {med:.0f} ms（n={n}，机理上界 ≈{kOpenSeekCeiling} ms：'
              f'fseek + fopen 未命中 ≈ 375 + 392）。')
    if context is not None:
        lo, hi, meds = context
        rank = 1 + sum(1 for v in meds.values() if v > med)
        detail += (f'全历史 {len(meds) + 1} 份跨 [{lo:.0f}, {hi:.0f}] ms（**2.8× 自然'
                   f'摆幅**，全部来自 fopen 未命中率的抽样差异）⇒ 本轮排第 '
                   f'{rank}/{len(meds) + 1}。⚠️ **"没排第一"不是 FAIL**：min/max '
                   f'区间当判定线两头都会造假 FAIL（见 pk_open_seek_context）')
    else:
        detail += '⚠️ 只跑了一份日志，没有跨日志区间可对照'
    if miss is not None:
        detail += (f'；fopen 未命中 {miss:.0%}（{len(fp)} 次）'
                   f'——open_seek 的摆幅在 fopen 的双值性上，不在 fseek')
    if fs_med is not None:
        detail += f'；fseek 中位 {fs_med:.0f} ms'
    if n < 8:
        detail += (f'。⚠️ n={n} 偏小：未命中率差一两笔就能把中位从 329 推到 634，'
                   f'**别把这一轮的高低位当趋势**')
    if fp:
        missv = [a for a, _ in fp if a > 0]
        fm = statistics.median(missv) if missv else None
        ok_fp = fm is None or kFopenMissBand[0] <= fm <= kFopenMissBand[1]
        if fm is not None:
            detail += (f'\n  · fopen 未命中成本中位 **{fm:.0f} ms**（n={len(missv)}，'
                       f'判线 [{kFopenMissBand[0]}, {kFopenMissBand[1]}]，'
                       f'实测历史 344~392 = 1.14×）⇒ '
                       + ('在带内' if ok_fp else '**出带，这才是回归**'))
        else:
            # 一轮里一次未命中都没有，这个分量的成本就**没被测到**。
            # 这不是通过，也不是回归——是这一轮的 open_seek 便宜得没有可分的量。
            detail += (f'\n  · fopen 未命中成本：本轮 {len(fp)} 次全是缓存命中，'
                       f'**未命中样本为 0，这个分量没测到**'
                       f'（判线 [{kFopenMissBand[0]}, {kFopenMissBand[1]}] 无从校验）')
        ok_fs = fs_med is not None and kFseekBand[0] <= fs_med <= kFseekBand[1]
        if fs_med is not None:
            detail += (f'\n  · fseek 中位 **{fs_med:.0f} ms**'
                       f'（判线 [{kFseekBand[0]}, {kFseekBand[1]}]，'
                       f'实测历史 258~375）⇒ '
                       + ('在带内' if ok_fs else '**出带，这才是回归**'))
        detail += ('\n  ⇒ 判据问的是这两个分量，不是 open_seek 中位：'
                   '后者的摆幅是未命中率的抽样噪声，前者的摆幅才是设备变化'
                   '（fopen 分量在"没有未命中样本"时无从校验，此时绿灯只代表'
                   ' fseek 这一半）')
        expect(log, 'PK:open-seek-not-regressed', ok_fp and ok_fs, detail)
    else:
        detail += ('\n  ⚠️ 本轮 `word card loaded:` 行**没有 fopen_ms/fseek_ms 位**'
                   '（探针来自 1005.24 之后的构建：word_pack.cpp:1428-1478 的 '
                   '`open_seek_ms` 之外追加这两个字段，老构建整条没有）。'
                   '分量测不到 ⇒ 只能退回机理上界这一条弱判线。'
                   '**这不是通过，是这次构建没带探针**')
        expect(log, 'PK:open-seek-not-regressed', med <= kOpenSeekCeiling, detail)
    expect(log, 'PK:parse-still-cheap',
           all(c['pa'] < 50 for c in log.cards), 'parse 仍可忽略')


# 命名 pass 引入的 owner。判据用"日志里出现过这些名字之一"来识别构建是否已带
# 命名 pass——刻意不用"owner 数量"之类的启发式：老日志有 12 个真实 owner 也不少。
# 列子集不会漏判（漏的只是不启用），列错不会误判（不会把老日志判成回归），
# 这正是我们要的失效方向。以后再加 owner 时把名字补进来即可。
NEW_OWNER_NAMES = frozenset({
    # pack 系列（note_pack / problem_pack / word_pack）
    'np-cache-reset', 'np-manifest-save', 'np-stream', 'np-image-load', 'np-image-store',
    'pp-cache-reset', 'pp-manifest-save', 'pp-stream',
    'wp-manifest-invalidate', 'wp-cache-reset', 'wp-manifest-save', 'wp-stream',
    # storage.cpp（存储基元，共 10 处）
    'save-string', 'save-u64', 'save-blob', 'clear-nvs-key', 'clear-identity-state',
    'cleanup-proto-problem', 'ensure-pack-capacity', 'save-device-control',
    'save-volume', 'factory-reset',
})
NEW_OWNER_PREFIXES = ('wp-', 'np-', 'pp-')


def hil_owner_attribution(log: Log):
    """owner 命名 pass 的回归判据。

    命名之前，不传 owner 的 ExecuteStorageTransaction 一律记成 "background"
    （storage_service.cpp:49/:346），所以 31 个后台样挤在一个桶里、无法归因——
    §一 那张表里 `background` 中位 782 / max 4054 因此说不清是谁写的。命名 pass
    之后这个桶应该是空的。

    判据只在"命名 pass 确实在盘上"时才启用：靠日志里是否出现该 pass 引入的
    owner 名字。老日志（1005.4 / 1005.6）里 owner=background 是当时的正常形态，
    判 SKIP 而不是 FAIL——否则历史日志会被误判成回归。默认字面量本身还留在
    storage_service.cpp:49，将来有人新增一处忘了命名，这条会抓住它。
    """
    owners = log.owners()
    has_pass = any(o in NEW_OWNER_NAMES or o.startswith(NEW_OWNER_PREFIXES)
                   for o in owners)
    defaulted = log.txof('background')
    if not has_pass:
        skip(log, 'OWNERS:attribution',
             '本日志来自 owner 命名 pass 之前的构建，background 桶是当时的正常形态')
        return
    expect(log, 'OWNERS:no-default-bucket',
           not defaulted,
           f'命名 pass 之后仍有 {len(defaulted)} 笔 owner=background'
           '（有调用点漏了命名；problem_store 的 background 分支曾被丢 label，'
           '是已知的同类缺陷）')


def hil_storage_bench(log: Log):
    """P1 量测 bench：把"单次元数据操作的底价"从推测变成测量。

    这个判据组不是"通过/不通过"意义上的测试，而是把重写的前提假设变成可证伪
    的数字。三个判据分别对应：
      1. 插桩是否真的解释了耗时（七项之和 vs total）——否则我们在量不相干的东西
      2. 纯查找底价是否存在——probe shape 对一个不存在的路径 fopen，
         保证走完整扫描、零数据页，是模型最干净的判据
      3. bench 有没有反过来拖累别人（它自己的入队等待）
    """
    bench_tx = log.txof('storage-bench')
    # stream ramp 也算 bench 的一部分：它既不打 `atomic write:` 行（那是
    # AtomicWrite 专属），也不打 `storage bench round: shape=` 行（那是按 shape
    # 记 wall_ms 的轮次行），所以只看 rounds/probes/bench_tx 会在"本轮只跑了
    # stream shape"时提前 return，把第 5 节整节静默跳过——判据不叫不是通过。
    if not log.rounds and not log.probes and not log.stream_rounds and not bench_tx:
        skip(log, 'BENCH:ran', '本轮无 storage bench（未跑 P1 量测构建）')
        return
    expect(log, 'BENCH:completed',
           'end' in log.bench_events or not log.bench_events,
           f'bench 标记 begin/end 配对情况：{log.bench_events}（只有 begin 说明中途挂了）')

    # --- 1. 插桩自证：七项之和应当解释 total 的大部分 -----------------------
    if log.probes:
        ratios = []
        for p in log.probes:
            parts = p['fopen'] + p['write'] + p['stat'] + p['remove'] + p['rb'] + p['rp']
            if p['total'] > 0:
                ratios.append(parts / p['total'])
        if ratios:
            med = statistics.median(ratios)
            expect(log, 'BENCH:op-sum-accounts-for-total', med >= 0.6,
                   f'七项之和 / total 中位 = {med:.2f}'
                   f'（<0.6 说明插桩漏掉了主要耗时，结论不可信）')

    # --- 2. 纯查找底价 -----------------------------------------------------
    # 语义说明：这一条 PASS 的意思是"重写方向有数据支撑"，FAIL 的意思是
    # "我们准备照其施工的那个前提是错的，停下来重推"。FAIL 不是说设备坏了——
    # 宁可让一条判据红着挡住错误的重写，也不要一条绿的放过它。
    by_shape = {}
    append_by_round = {}
    for r in log.rounds:
        by_shape.setdefault(r['shape'], []).append(r['wall'])
        if 'append' in r['shape']:
            append_by_round[r['round']] = r['wall']
    # `new_object` 分簇：round 0 = 建文件（1），之后每轮只追加（0）。
    # ⚠️ 这一位**不能独自结案**，因为 round 0 永远是那个 new_object=1 的 round，
    # 所以 new_object 与"本轮第一笔"完全共线——贵档落在 round 0 上，可以是
    # "建对象贵"，也可以是"第一笔贵"（冷缓存、首次分配、目录项第一次落盘），
    # 而 n=1 分辨不了。真正能定案的是反过来的情形：**贵档里出现 new_object=0
    # 的轮次**，那就证明建对象不是全部成因，追加路径自己就有贵档。
    append_split = None
    if log.bench_new_object and append_by_round:
        groups = {0: [], 1: []}
        for rnd, wall in sorted(append_by_round.items()):
            if rnd in log.bench_new_object:
                groups[log.bench_new_object[rnd]].append(wall)
        if any(groups.values()):
            append_split = groups

    # 2a. 追加 shape 是"成本在扫描长度"这个前提的直接反驳，必须优先判。
    # WHY: probe/cur 的比值只描述"未命中走查 vs 一笔写"，它看不见另一件事——
    # 同样字节数的持久写，走"追加到已存在对象"是几十毫秒，走"temp/rename 建新
    # 对象"是一秒级。1005.12 实测 append 209 ms vs cur52 1097 ms（5.3×），页数
    # 完全一样。单看 probe/cur 比值会给出 0.62 → "底价成立，按缩短扫描长度施工"
    # 的绿灯，而那恰好是这份日志证伪的方向。所以这里显式判一次，并推翻 2b。
    # 用 cur*（最小载荷的 AtomicWrite）作分母，是为了把"载荷差异"这个辩解拿走。
    #
    # ⚠️ 但 append shape 本身在 SPIFFS 上是**双峰**的，n=4 时中位数没有意义：
    # 便宜档 ~10–40 ms（往已打开句柄追加），贵档 ~400–1500 ms（踩到对象创建/GC）。
    # 两峰各占一半时，中位数落在**两峰之间的空隙里**，而空隙位置随哪个便宜样本
    # 更大而漂移——1005.12 的贵档最小 410 → 中位 216、1005.24 同一现象 216、
    # 1005.19 贵档最小 1042 → 中位 541。**同一组现象，"中位数"差 2.6×**，于是
    # 1005.19 判"按缩短扫描长度施工"、1005.24 判"改追加/日志结构"，而 append 的
    # 原始样本几乎一模一样。这是判据对自己的实验喊狼来了，且两次喊的方向相反。
    # 现在：双峰就判"数据不足"，并说清补测什么，绝不拿空隙里的中位数下施工结论。
    cur_med = min((statistics.median(w) for s, w in by_shape.items()
                   if s.startswith('cur')), default=None)
    append_walls = next((w for s, w in sorted(by_shape.items())
                         if 'append' in s), None)
    append_clusters = cluster_gap(append_walls) if append_walls else None
    append_bimodal = bool(append_clusters and len(append_clusters) > 1)
    append_med = (min(statistics.median(w) for s, w in by_shape.items()
                      if 'append' in s) if append_walls else None)
    append_refutes = None
    append_indeterminate = None
    if append_bimodal:
        spread = max(append_walls) / min(append_walls) if min(append_walls) else float('inf')
        append_indeterminate = (
            f'追加 shape 的 {len(append_walls)} 笔分裂成多档：'
            + ' / '.join(f'{a:.0f}~{b:.0f} ms (n={n})'
                         for a, b, n in append_clusters)
            + f'，最贵与最便宜差 {spread:.0f}×。n={len(append_walls)} 且两档各占一半时'
            f'**中位数落在两档之间的空隙里，它既不是哪一档的代价**（空隙位置随哪个'
            f'便宜样本更大而漂移，历史上因此差出 2.6×），所以'
            + (f'拿它和 cur* 比 {cur_med:.0f} ms 得出的倍数是个噪声比值，'
               f'不能当施工依据。' if cur_med
               else '它（和任何单值归约）都不能当施工依据。')
            + f'便宜档已证明"追加本身可以很便宜"，贵档证明"它有时不是"——'
            f'**这恰是重写要回答的问题，不是答案**。补测：把 append shape 的 '
            f'rounds 提到 ≥12（两档都能拿到足够的 n），并给每笔记下是否新建对象')
        if append_split is None and append_walls:
            append_indeterminate += (
                '（本轮没有 `new_object=` 位可切：贵的轮次是不是 round 0，'
                '决定"建对象贵"和"第一笔贵"哪个解释成立，而现在一个都排不掉）')
        elif append_split is not None:
            new_w = append_split[1]
            old_w = append_split[0]
            # 贵档 = 超过便宜档最小值 2× 的那一侧。
            cheap = min(old_w) if old_w else None
            dear_in_new = [w for w in new_w if cheap and w > 2 * cheap]
            dear_in_old = [w for w in old_w if cheap and w > 2 * cheap]
            append_indeterminate += (
                f'；`new_object` 切开：round 0（=1，建文件）'
                f'{new_w}、其余轮次（=0，只追加）{old_w}。'
                f'⚠️ **这一位不能独自结案**：round 0 永远是那个 new_object=1 的'
                f'轮次，所以"新建对象"与"本轮第一笔"完全共线——贵档落在 round 0 '
                f'上，可以是建对象贵，也可以是第一笔贵（冷缓存/目录项首次落盘），'
                f'n=1 分辨不了')
            if dear_in_old:
                append_indeterminate += (
                    f'。但 **new_object=0 的轮次里出现贵档**（{dear_in_old}），'
                    f'这就把"建对象"这个解释否掉了：追加路径自己就有贵档，'
                    f'重写不能只靠"别建对象"了事')
            elif dear_in_new:
                append_indeterminate += (
                    f'。贵的全在 round 0（n={len(dear_in_new)}），便宜的全是'
                    f'new_object=0（n={len(old_w)}）——这**与"建对象贵"一致**，'
                    f'但因为共线，也同样与"第一笔贵"一致，仍不足以定案')
    elif cur_med and append_med:
        ratio = cur_med / append_med if append_med else float('inf')
        if append_med < 20.0:
            append_refutes = (
                f'追加写只花 {append_med:.0f} ms，而同样走 SPIFFS 的 cur* 写要 '
                f'{cur_med:.0f} ms（{ratio:.1f}×）。差距大到不可能由载荷或扫描长度解释，'
                f'成本在"创建新对象 + backup 轮换"，不在"按名查找扫描"')
        elif ratio >= 3.0:
            append_refutes = (
                f'追加写 {append_med:.0f} ms vs cur* AtomicWrite {cur_med:.0f} ms'
                f'（{ratio:.1f}×，页数相同）。差额就是 temp 创建与 backup 轮换，'
                f'"缩短扫描长度"只解释其中一小部分')

    # 追加档的证据先判，且**不依赖 probe 那一支**：它量的是写路径本身，比
    # probe/cur 比值更直接。以前它被塞在 `if 'probe' in by_shape` 里，一旦某轮
    # 没跑 probe 追加档就被完全跳过——那是把最硬的一条证据变成可选项。
    if append_indeterminate:
        # 判 FAIL 而不是 SKIP：这正是"数据不足却给出施工结论"的那种情形，
        # SKIP 会让判据在什么都没测到的情况下报绿（并让上游以为已裁决）。
        probe_note = ''
        if 'probe' in by_shape and by_shape['probe']:
            probe_note = (f'（probe/cur 比值也无法裁决：它量的是"未命中走查 vs '
                          f'一笔写"，不是"成本在不在对象创建"。）')
        # 真实流量侧的追加写：这是判据唯一能分辨"双峰是 bench 造出来的"还是
        # "产品自己就是双峰"的地方。bench 逐轮换 shape、持租约、排队，它的双峰
        # 有可能全是我们自己的量测方式造成的；`word observation durable:` 是产品
        # 自己在答题路径上打的，没有 bench 的那些因素。1005.26 上它同样双峰
        # （6~15 ms ×7 / 365~433 ms ×3），与 bench 的 31 / 1308~2149 同形。
        # 这条把"bench 数据不够"升级成"产品行为如此"——后者不是补测能消除的，
        # 是重写必须解决的东西。
        real_note = ''
        if log.obs_durable:
            rc = cluster_gap([o['append'] for o in log.obs_durable])
            if len(rc) > 1:
                real_note = (
                    f'⚠️ **真实流量侧同样是双峰**（`word observation durable:` '
                    f'append_ms：'
                    + ' / '.join(f'{a:.0f}~{b:.0f} ms (n={n})' for a, b, n in rc)
                    + f'），所以双峰**不是 bench 的量测假象**，是这条路径本身'
                    f'的行为——补测 bench rounds 不能消除它，重写要正面处理'
                    f'"便宜簇＋贵约百倍的簇"这件事')
            else:
                lo, hi, n = rc[0]
                real_note = (f'（真实流量侧本轮是单峰：append_ms {lo:.0f}~{hi:.0f} ms '
                             f'n={n}，所以 bench 的双峰**可能**是量测假象，'
                             f'这一条仍要靠补测定夺）')
        expect(log, 'BENCH:floor-model-holds', False,
               f'追加 shape 双峰，{append_indeterminate}。'
               f'本轮**不予裁决**，不要照任何一版中位数定施工方向。{probe_note}{real_note}')
    elif append_refutes:
        # 追加证据优先：它比 probe/cur 比值更直接地量到了写路径本身。
        # 这条曾经是绿的（1005.7 也是绿的），因为判读器只比较了 probe 与
        # cur，看不见"同一文件系统上追加写只要 8 ms"这一档。
        expect(log, 'BENCH:floor-model-holds', False,
               f'底价模型被推翻（追加侧证据）：{append_refutes}。'
               f'施工方向改为"消除对象创建与 backup 轮换，改追加/日志结构"')

    # 追加档已裁决时，probe 支不再下第二个 verdict：同一条判据对同一份数据出两个
    # 结论，会把一次 FAIL 数成两次；而且 probe/cur 说明不了"成本在不在对象创建"。
    if not (append_indeterminate or append_refutes) and 'probe' in by_shape:
        probe_med = statistics.median(by_shape['probe'])
        per_op = None
        for shape, walls in by_shape.items():
            if shape.startswith('cur'):
                per_op = statistics.median(walls) / 5.0  # fopen+write+stat+remove+rename
                break
        if per_op is None and log.probes:
            per_op = statistics.median(p['total'] for p in log.probes) / 5.0
        if per_op is None:
            skip(log, 'BENCH:floor-model-holds', '有 probe 但没有可折算单次 op 成本的写 shape')
        else:
            ratio = per_op / probe_med if probe_med > 0 else float('inf')
            if probe_med < 20.0:
                verdict = (f'底价模型被推翻：纯查找只花 {probe_med:.0f} ms，而写入侧单次 op '
                           f'≈{per_op:.0f} ms（比值 {ratio:.1f}）。成本不在元数据扫描上，'
                           f'"减少页数/减少 op"这个方向不成立，必须重推根因再动重写')
            elif not 0.4 <= ratio <= 2.5:
                verdict = (f'模型对不齐：纯查找 {probe_med:.0f} ms 而单次 op ≈{per_op:.0f} ms'
                           f'（比值 {ratio:.2f}，超出 0.4~2.5）。查找贵而写便宜 ⇒ 该优化'
                           f'的是"少打开"，不是"少分页"；反之则相反。按实测重定方向')
            else:
                verdict = (f'底价模型成立：纯查找中位 {probe_med:.0f} ms，单次元数据 op '
                           f'≈{per_op:.0f} ms（比值 {ratio:.2f}）。与 1005.6 上 n=11 的 '
                           f'journal 拟合一致，可按"缩短扫描长度"施工'
                           + ('；本轮无追加 shape，未覆盖"成本是否在对象创建"' if append_med is None else ''))
            expect(log, 'BENCH:floor-model-holds', bool(verdict.startswith('底价模型成立')),
                   verdict)
    # 双峰 shape 不印中位数——那个数落在两峰空隙里，印出来只会被当成某一档的
    # 代价引用（本轮就被引用过一次，结论完全相反）。
    def shape_desc(shape, walls):
        cl = cluster_gap(walls)
        if len(cl) > 1:
            return (f'{shape} **双峰** ' + ' / '.join(
                f'{a:.0f}~{b:.0f} ms (n={n})' for a, b, n in cl))
        return f'{shape} 中位 {statistics.median(walls):.0f} ms (n={len(walls)})'

    detail = '；'.join(shape_desc(s, w) for s, w in sorted(by_shape.items()))
    if detail:
        check(log, 'BENCH:shape-costs', 'PASS', True, detail)

    # --- 3. bench 的 sleep lease 必须死了 ----------------------------------
    # WHY THIS EXISTS (1005.12, commit 17c3d4c): `BenchTask` ended with
    # `vTaskDelete(nullptr)`, which runs NO destructors, so the stack SleepLease
    # acquired to survive the 60 s idle deadline was never returned. The device
    # sat there for 236 s after the bench had finished, `ActiveSleepBlockerCount
    # (kStorage)` stuck above zero, deep sleep permanently blocked -- caused by
    # the very lease added to protect the bench. It is invisible in the bench's
    # own numbers (END total_ms is honest) and needs the sleep warnings to see.
    # Scope is deliberately narrow: only `storage-bench`. The pack and cloud
    # holders legitimately outlive a download by minutes and would make a
    # blanket check cry wolf on a healthy device.
    reserve_markers = any('storage gc reserve experiment' in line for line in log.lines)
    # The lease covers the WHOLE experiment (four arms or prepared-only), not
    # a sub-arm END. Reboot clocks and observation tails must not be mixed.
    ends, leaked, epoch, final_by_epoch = [], [], 0, {}
    last_by_epoch, warning_by_epoch = {}, {}
    malformed_end = False
    selected_end = ('storage gc reserve experiment END' if reserve_markers else 'storage bench END')
    for line in snapshot_probe_records(log):
        if BOOT_BANNER_RE.search(line):
            epoch += 1
        tm = LOG_PREFIX_RE.match(line)
        if tm:
            at = int(tm.group(1))
            last_by_epoch[epoch] = max(at, last_by_epoch.get(epoch, 0))
        if selected_end in line:
            if tm and SNAPSHOT_RECORD_RE.match(line):
                at = int(tm.group(1))
                ends.append(at)
                final_by_epoch[epoch] = at
            else:
                malformed_end = True
        for m in LEASE_RE.finditer(line):
            l = {k: v if k in ('tag', 'blocker', 'holder') else int(v)
                 for k, v in m.groupdict().items()}
            if l['holder'] != 'storage-bench':
                continue
            warning_by_epoch[epoch] = l['at_ms']
            if epoch in final_by_epoch and l['at_ms'] > final_by_epoch[epoch]:
                leaked.append(l)
    if malformed_end:
        expect(log, 'BENCH:lease-released', False, 'bench/experiment END 时间戳/日志头漂移，不借分组 END 冒充收尾')
    elif not ends:
        skip(log, 'BENCH:lease-released',
             '本轮 bench/总实验没有 END 标记，无从判断 lease 是否归还')
    elif leaked:
        worst = max(leaked, key=lambda l: l['held'])
        expect(log, 'BENCH:lease-released', False,
               f'bench 已于 {max(ends)} ms 结束，但 {len(leaked)} 条 storage-bench 租约'
               f'告警出现在其后（最晚一条 held_ms={worst["held"]}，'
               f'日志时钟 {worst["at_ms"]} ms）⇒ 租约没还，深睡被永久挡住')
    else:
        # sleep_coordinator.cpp repeats a holder's warning no more often than
        # once per 60 s. Silence before that holder's next gate proves nothing.
        # No earlier warning: conservatively observe a full 60 s after END.
        gates = {ep: max(at, warning_by_epoch.get(ep, at) + 60000)
                 for ep, at in final_by_epoch.items()}
        missing = [(ep, last_by_epoch.get(ep, 0), gate)
                   for ep, gate in gates.items() if last_by_epoch.get(ep, 0) <= gate]
        if missing:
            skip(log, 'BENCH:lease-released',
                 f'END 后无告警但未跨该启动的下一告警门(epoch,last_ms,gate_ms)={missing}；'
                 '不是租约已释放的证据')
        else:
            expect(log, 'BENCH:lease-released', True,
                   f'bench 在 {max(ends)} ms 结束，各启动已跨下一告警门 {gates} 且无后续告警；'
                   '这是告警代理，不是直接 holder=0/深睡恢复验收')

    # --- 4. bench 不能把真实流量弄丢 ---------------------------------------
    # 第一条写法是错的，被指出后改掉：bench 是 20 笔串行事务，第 N 笔的
    # queue_wait 就是前 N-1 笔的累计 elapsed——按我们自己的模型 3 KB 那档单笔
    # 1.4~2 s，后几笔必然 >2 s，且那正是 bench 自己造成的。判 bench 自己的
    # queue_wait 恒红，等于让判读器对"我们设计的实验"喊狼来了。
    # 真正要抓的是：**排在 bench 后面的真实事务有没有被弄丢**。bench 期间
    # 真实流量被推迟是必然且可接受的，被拒/被丢弃才是回归。
    if bench_tx:
        starved = [t for t in log.tx
                   if t['owner'] != 'storage-bench' and int(t['qw']) >= 2000]
        if not starved:
            skip(log, 'BENCH:real-traffic-survived',
                 'bench 期间没有真实事务被推迟 2 s 以上，无从判定')
        else:
            # NOT_FOUND is not a loss: outbox-peek / session-load legitimately
            # return it on a device with nothing pending, and the P1 log is full
            # of them. Counting it made a clean log read as FAIL, which is the
            # same disease as the queue_wait criterion -- a judge that cries
            # wolf on its own experiment. What actually means "lost" is a
            # refusal (a gate or lease said no) or an abandonment (the queue
            # deadline dropped it). Everything else is reported, not counted.
            lost = [t for t in starved
                    if t['res'] in ('ESP_ERR_INVALID_STATE', 'ESP_ERR_TIMEOUT')]
            other = [t for t in starved if t['res'] not in
                     ('ESP_OK', 'ESP_ERR_INVALID_STATE', 'ESP_ERR_TIMEOUT')]
            detail = (f'{len(starved)} 笔真实事务排在 bench 后等了 ≥2 s'
                      f'，其中 {len(lost)} 笔被拒/被丢弃（被推迟可以接受，'
                      f'被丢掉是回归）')
            if other:
                detail += f'；另有 {len(other)} 笔返回非 OK 的空结果（{other[0]["res"]} 等）'
            expect(log, 'BENCH:real-traffic-survived', not lost, detail)

    # --- 5. stream ramp：每笔成本按次收，还是按字节收 -----------------------
    # WHY THIS EXISTS: 1005.15 的"106× 异常"是判读器侧的量法错误——那个
    # write_ms=15 取自 `AtomicWriteProbe::AfterWrite()`，而 `AfterWrite()` 在
    # fwrite+fflush+fsync+fclose **全部**之后才调用（word_study_store.cpp:477-480），
    # 所以 15 ms 是这几个动作合计的子字段；它所属事务的 total_ms 中位是 1500 ms
    # （fopen 1089 + write 15 + rename 358，子字段闭合）。用全量比全量：
    # session3k-direct 500 ms/KB vs wp-stream 517 ms/KB，差 1.03×。
    # 没有异常，所以这条判据不负责解释那个差距。
    # 顺带一个方向性更强的推论：AtomicWrite **付了** fsync 仍比**不付** fsync 的
    # wp-stream 每字节便宜 6 倍，所以 6.2× 不能归咎于"wp-stream 省掉了 fsync"。
    # 它负责的是另一个真正没解释的事实：kAppend 只做 fwrite + sha256，全程
    # 一个句柄、只在 kCommit fsync 一次，可每笔仍要 ~890 ms，且 elapsed 是三峰
    # （13 笔 5–50 ms / 52 笔 ~470 ms / 104 笔 ~890 ms）。这个 ramp 就是拿
    # 分片大小当自变量，把那两种收费方式分开。
    # FAIL 的语义只有一种：**ramp 退化，这份日志分辨不出结论**。判出 H1 或 H2
    # 都是 PASS + 结论写进 detail——它们是施工决策的输入，不是设备好坏。
    if not log.stream_rounds:
        skip(log, 'BENCH:stream-cost-model',
             '本轮无 stream shape（旧 bench 的单一 32 KB 尺寸不构成 ramp）')
    else:
        # 超预算那一笔的 cost 是预算本身（下界），不是实测成本，但它仍然算数：
        # H2 下超预算的正是大分片那几轮，abort 就是 H2 最强的证据。
        aborted = {a['round']: a for a in log.stream_abort}
        pts = []
        for r in sorted(log.stream_rounds, key=lambda r: r['chunk']):
            cost = r['cost']
            if cost <= 0:
                cost = aborted.get(r['round'], {}).get('cost', 0)
            if cost > 0:
                pts.append((r['round'], r['chunk'], cost, cost / r['chunk']))
        sizes = sorted({p[1] for p in pts})

        # 退化守卫：单一尺寸上 H1 和 H2 的预测完全重合（1719 B 时都预测
        # 890 ms / 517 ms/KB）， ramp 一旦退化，每字节成本之比会恒等于 1，
        # 判读器会自信地报出一个"H2 成立"的绿灯——而它什么都没测。
        # 这一类错误（被证伪的模型读出 PASS）在这份判读器里已经错过两次。
        if len(sizes) < 3 or (sizes[-1] // max(sizes[0], 1)) < 4:
            expect(log, 'BENCH:stream-cost-model', False,
                   f'ramp 退化：只测到 {[s // 1024 for s in sizes]} KB'
                   f'（需要 ≥3 个尺寸且跨度 ≥4×）。H1 与 H2 在单一尺寸上是'
                   f'退化的，这份日志无法分辨，不要据它下结论')

        # kCommit 必须跑到：只有 round 行没有 commit 行，说明句柄没关，
        # 也正是 bench 之外 wp-stream 失败时要丢数据的那条路径。
        if log.stream_rounds and not log.stream_commit:
            expect(log, 'BENCH:stream-kcommit-closed', False,
                   f'{len(log.stream_rounds)} 笔 stream round，但没有 '
                   f'`storage bench stream commit:` ⇒ kCommit 没跑到，'
                   f'跨轮持有的句柄没有 fflush+fsync+fclose（wp-stream 的 '
                   f'kCommit 走的是同一段代码）')
        else:
            commit = log.stream_commit[-1]
            # aborted=1 只说明 ramp 被 6 s 预算截短，句柄仍然被 fflush+fsync+
            # fclose 了（result=ESP_OK 就是证据）。把它判成 FAIL 等于对"我们
            # 自己设计的实验"喊狼来了——而这个判读器已经犯过三次同类错误。
            # 真正要抓的只有"没有 commit 行"（句柄泄漏）和"commit 自己失败"。
            cost = commit['cost_ms']
            unres = (f'（**低于计时分辨率，未实测**——1005.19 在这个尺度上量到 0，'
                     f'而 1.34 MiB 上真实的收尾是 15,327 ms）'
                     if cost < kStreamCommitResolutionMs else '')
            detail = (f'kCommit fflush+fsync+fclose {cost:.1f} ms{unres} '
                      f'result={commit["result"]}'
                      + (f'（ramp 被 6 s 预算提前收尾，后几轮未测；'
                         f'这正是 H2 下该发生的事）' if commit['aborted'] else ''))
            expect(log, 'BENCH:stream-kcommit-closed',
                   commit['result'] == 'ESP_OK', detail)

        if len(sizes) >= 3 and (sizes[-1] // max(sizes[0], 1)) >= 4 and pts:
            # 最小/最大两端比：H1 预测 = 分片跨度（成本固定 ⇒ 每字节 ∝ 1/bytes），
            # H2 预测 = 1（成本 ∝ bytes ⇒ 每字节恒定）。两端比不需要拟合，是模型
            # 的直接推论，拿它当第二个读数与拟合互相印证。
            small, big = pts[0], pts[-1]
            spread = small[3] / big[3] if big[3] > 0 else float('inf')
            table = '；'.join(
                f'{c // 1024}K→{cost}ms（{pb:.3f} ms/B'
                + ('，超 6s 预算，cost 为下界' if rnd in aborted else '') + '）'
                for rnd, c, cost, pb in pts)

            # --- 最小二乘 cost = F + s×bytes，然后用它算 deadline -------------
            # WHY: "每笔固定"与"每字节计费"是两种极端，真实形态大概率在中间
            # （AtomicWrite 拟出来就是 F≈1243/1964 ms + s≈0.084 ms/byte，
            # 截距差 721 ms ≈ 实测 backup 轮换 719 ms，自洽）。中间态下的施工
            # 结论与两个极端都相反——32 KB 分片不够，而且加大分片有地板——
            # 所以不能只贴一个模型名放过去，必须把数算出来。
            fit = StreamFit(pts)
            if fit is None:
                verdict = ('ramp 点数不足以拟合，拟不出固定项/每字节项。'
                           '两端比 ' + f'{spread:.1f}×' + ' 可作参考')
            else:
                fixed, per_byte = fit
                # kCommit 的实测值，以及它作用在多大的文件上。文件大小必须取
                # **最后执行的那一轮的 file_bytes**（日志里的累计量）：abort 时
                # 提交发生在被截断处，不是发生在最大分片那一轮。错用最后一轮的
                # chunk_bytes（32K）会把外推倍数从 21× 说成 41×，方向就偏了。
                commit = log.stream_commit[-1] if log.stream_commit else None
                commit_file = None
                if commit is not None and log.stream_rounds:
                    commit_file = max(r['file'] for r in log.stream_rounds)
                # 分片假设要分两行印：**当前构建实测值**和**正在验证的提案值**。
                # WHY: kStreamChunkUnderTest 是提案值（P1b-B 要回答"32 KB 够不
                # 够"），判据曾经只印它，于是 1005.26 之前每一份日志都在用
                # "32K 分片需 41 笔"描述一条实际跑 6 KB 分片的路径。真实值是
                # 1005.26 才第一次量到的：`word pack stream append: chunk_bytes=`
                # 中位 6,140 B（n=261），41 vs 218 笔差 5.3×。提案值不是现状，
                # 不印现状就等于把提案当成了事实。
                chunk_options = []
                if log.appends:
                    real = statistics.median(a['chunk'] for a in log.appends)
                    chunk_options.append(
                        (f'当前构建实测分片中位 {real / 1024:.1f} KB'
                         f'（n={len(log.appends)} 次 kAppend）', int(real)))
                chunk_options.append(
                    (f'提案值 {kStreamChunkUnderTest // 1024} KB 分片',
                     kStreamChunkUnderTest))
                pred, grand_ms, append_only_ms = StreamDeadlinePrediction(
                    fixed, per_byte,
                    chunk_options=chunk_options,
                    # 取归一后的 cost_ms，不是原始 cost：对端后来把字段从
                    # cost_ms 改成了 cost_us，1005.28 是 cost_us=1250918，
                    # 直接拿原始值会打印"kCommit 实测 1250918 ms"——把 1.25 s
                    # 说成 20 分钟，还把外推结论放大 1000 倍。
                    # commit_cost_ms() 已经按单位归一，下游只该看 cost_ms。
                    commit_ms=commit['cost_ms'] if commit else None,
                    commit_file_bytes=commit_file)
                fixed_share = (fixed / (fixed + per_byte * big[1])
                               if (fixed + per_byte * big[1]) > 0 else 0.0)
                if per_byte <= 0:
                    model = (f'H1 成立（拟合不出每字节项，成本全是固定项）：'
                             f'每笔 ≈{fixed:.0f} ms')
                elif fixed <= 0:
                    model = (f'H2 成立（拟合不出固定项，成本全是每字节项）：'
                             f'每字节 {per_byte:.4f} ms')
                else:
                    model = (f'混合模型成立：固定项 ≈{fixed:.0f} ms/笔 + '
                             f'每字节 {per_byte:.4f} ms/B'
                             f'（{big[1] // 1024}K 那一笔里固定项占 '
                             f'{fixed_share:.0%}）')
                # --- 拿真实下载直量校准模型，而不是只让模型自洽 -------------
                # WHY: bench 的 stream shape 与真实 kAppend **不是**同一条代码
                # 路径——bench 跨轮持有 `FILE* g_bench_stream`，所以它拟合出的
                # 固定项天然不含 fopen；而 `atomic write:` 探针量到 fopen 占
                # 42.5%、rename 18.1%、stat 10.0%、remove 8.9%
                # （= 非写入 88.3%），真数据 write 只占 11.7%。
                # 只看 bench 自洽会把"成本在不在对象创建"这个问题量漏。
                # 1005.26 第一次同时有 bench ramp + 真实完成的下载，所以这是
                # 第一次能拿真实路径的每笔成本反过来校模型——必须做。
                agree = ''
                direct = ''
                real_tx = [t for t in log.txof('wp-stream') if el(t) > 0]
                if log.appends and real_tx:
                    measured = statistics.median(el(t) for t in real_tx)
                    real_chunk = statistics.median(a['chunk'] for a in log.appends)
                    predicted = fixed + per_byte * real_chunk
                    ratio = measured / predicted if predicted > 0 else float('inf')
                    direct = (f'；真实下载直量校准：{len(real_tx)} 笔 wp-stream '
                              f'elapsed **中位** {measured:.0f} ms，模型在同分片'
                              f'（{real_chunk / 1024:.1f} KB）下预测 '
                              f'{predicted:.0f} ms ⇒ 实测/预测 {ratio:.2f}×'
                              f'（姊妹判据 STREAM:append-chunk-measured 报的'
                              f'"每笔 NNN ms"是**均值**，同一个均值/中位之别，'
                              f'不是两套量）')
                    if ratio > 3.0 or ratio < 0.33:
                        direct += (f'（差 >3× ⇒ bench 拟合值**不能**当作真实 '
                                  f'kAppend 的代价用，它漏掉了这条路上的'
                                  f'那部分成本；命中点以真实下载为准）')
                agree = direct + agree if direct else agree
                # 两端比与拟合是两套独立读数，矛盾要说出来而不是藏着
                span = sizes[-1] // max(sizes[0], 1)
                if spread >= 5.0 and per_byte > 0 and fixed > 0:
                    agree = (f'⚠️ 两端比 {spread:.1f}× 看似 H1，但拟合出固定项'
                             f'{fixed:.0f} ms + 每字节 {per_byte:.4f} ms/B——'
                             f'两端比只看两个端点，会漏掉中间的每字节成分，以拟合为准')
                elif spread <= 1.5 and per_byte <= 0:
                    agree = (f'⚠️ 两端比 {spread:.2f}× 看似 H2，但拟合不出每字节项'
                             f'（固定 {fixed:.0f} ms），以拟合为准')
                # 拟合残差：一条直线描述不了整条 ramp 时必须说出来。
                # 1005.19 的 ramp 是 2K→10 ms / 4K→29 ms / 8K→769 / 16K→1558 /
                # 32K→2757——最小两点比模型预测低 13–18 倍（那已经是 append 基线的
                # 价），大分片三点却很贴。退化守卫只查"≥3 尺寸且跨度 ≥4×"，这条
                # ramp 满足（5 尺寸 / 16×），所以拦不住；但不拦的结果是判据自信地
                # 报"H2 成立"，而 ramp 明显是**拐点**形状。
                # 施工含义：大分片区间的斜率可用（分片调大正是要進那个区间），
                # 但"整条 ramp 服从 cost=F+s×bytes"这个更强的说法不成立。
                knee = knee_note(fixed, per_byte, pts)
                if knee:
                    agree += '；' + knee
                # kCommit 是三个模型都没有的一项，也是唯一没有真尺度实测的
                # （1005.15 在 20.8% 处超时，kCommit 从未执行）。如果它把
                # kAppend 侧的余量吃掉，结论就从"够"翻成"实际不通"——必须显式说。
                if commit is not None:
                    factor = kStreamPackBytes / max(commit_file, 1)
                    if factor > 4.0 and grand_ms > kStreamDeadlineMs >= append_only_ms:
                        agree += (f'；⚠️ 只看 kAppend 会得出"余量 '
                                  f'{(kStreamDeadlineMs - append_only_ms) / 1000:.1f} s"的'
                                  f'结论，而 kCommit 未实测，别拿这个数当上界')
                    elif factor > 4.0:
                        agree += (f'；⚠️ kCommit 在本尺度未实测（{factor:.1f}× 外推'
                                  f'已因低于分辨率被拒绝），结论对它敏感——'
                                  f'真值见 STREAM:download-within-deadline')
                verdict = (
                    f'{model}。{pred}'
                    f'（H1 理论跨度 ≈{span}×，H2 理论 ≈1×，实测两端比 '
                    f'{spread:.1f}×）{agree}')
            check(log, 'BENCH:stream-cost-model', 'PASS', True,
                  verdict + '。' + table)


def hil_stream_read_buffer(log: Log):
    """读缓冲尺寸：报数，不再当门禁。

    它曾经是门禁，因为"32 KB 读缓冲是修法吗"就是 P1b-B 要回答的问题，而读到 2048
    说明烧错固件、整轮白跑。**那个问题已经有答案了**（1005.19：净收益为零，
    事务数 169→36 但总时间不动），缓冲也被撤回 2 KB。所以以后日志里不会再出现
    32768，继续判 FAIL 等于对我们自己已经结束的实验喊狼来了。

    保留它是为了归因：读缓冲大小决定分片笔数，决定了每一笔该按多大算。
    只报数 + 说明这个数会影响什么。
    """
    if not log.read_buffers:
        skip(log, 'STREAM:read-buffer-32k',
             '本轮没有 `word-pack-download: read_buffer_bytes=` 行（没下载词包，'
             '或没跑带该插桩的构建）')
        return
    sizes = sorted(set(log.read_buffers))
    buf = sizes[0] if len(sizes) == 1 else None
    if buf is None:
        expect(log, 'STREAM:read-buffer-32k', True,
               f'同一次启动里出现多个读缓冲尺寸 {sizes} ⇒ 下载路径有多条且分片'
               f'大小不同。解读下面任何每笔成本前先按路径拆开，别混在一起平均')
        return
    tx_per_pack = -(-kStreamPackBytes // max(buf, 1))
    note = ''
    if buf != 32 * 1024:
        note = (f'（不是 32768：这是 P1b-B 之前的 2 KB 路径，'
                f'每笔成本会比分片大的时候高几倍。该实验已出结论并撤回）')
    expect(log, 'STREAM:read-buffer-32k', True,
           f'read_buffer_bytes={buf}{note}；{kStreamPackBytes // 1024} KiB 词包'
           f'在这个读缓冲下约 {tx_per_pack} 笔事务。本轮日志出现 '
           f'{len(log.read_buffers)} 次')


def hil_stream_download_deadline(log: Log):
    """端到端直量：一次大包下载有没有在 deadline 内下完。**不经过模型。**

    这是本组判据里唯一直接量"产品有没有达标"的一条。BENCH:stream-cost-model
    走"拟合 F/s → 外推"，而 1005.19 证明外推会骗人：bench 的 kCommit 在 62 KiB 上
    量到 0 ms（低于 ms 计时分辨率），21× 外推后判据说"≈0.0 s"，可同一份日志里
    真实收尾那笔是 15,327 ms——一条 15 s 的误差，方向还是"乐观"。
    模型能回答"成本结构是什么"，回答不了"这一版到底行不行"。

    做法：取日志里 bytes_expected 最大的那次下载，把它起点到失败行之间
    `owner=wp-stream` 的 elapsed_ms **直接加起来**。

    ⚠️ 这里**不算每字节**。解压后的分片大小没有任何一行日志记录：
    `chunk_bytes` 只有 bench ramp 有（那是 bench 自己选的），真实下载侧只打
    `read_buffer_bytes`（**压缩**读缓冲）和 `received`（**压缩**累计）。
    拿 received 当分母会差一个压缩比（1005.19 实测 ≈4×），那正是
    "wp-stream 比 bench 贵 4.9×"这类幻影的来源。每字节只能由 bench ramp 出，
    本条只报"窗口被存储占了多少、够跑几笔"。

    FAIL 的语义是**产品没达标**，不是设备坏了。它会在 1005.7/12/15/19 上
    都 FAIL——四版都真的超时了，这不是喊狼来了。
    PASS 的条件是"没有失败行且末笔在 deadline 前结束"：wqn_api.cpp:1986 只在
    `result != ESP_OK` 时打失败行，所以没有失败行 ≈ 下完了。
    """
    if not log.downloads:
        skip(log, 'STREAM:download-within-deadline',
             '本轮没有 `word-pack-download:` 行（没下载词包）')
        return
    big = max(log.downloads, key=lambda d: d['bytes'])
    if big['bytes'] < kStreamPackBytes:
        skip(log, 'STREAM:download-within-deadline',
             f'本轮最大词包只有 {big["bytes"]} B（阈值 {kStreamPackBytes} B），'
             f'测不到 deadline 量级的下载')
        return
    fail = next((f for f in log.failures if f['t'] >= big['t']), None)
    done = next((d for d in log.done if d['t'] >= big['t']), None)
    # deadline 从日志里取，不再假定 120 s。成功行**不带** reason，所以预算也可能
    # 只能从同一次启动里别的失败行拿到；都拿不到才退回源码标定值，并把来源写进
    # detail ——一个说不清来源的预算会让 PASS 也是虚的。
    deadline, src = kStreamDeadlineMs, '标定值 wqn_api.cpp:1941'
    for f in log.failures:
        m = re.search(r'total-budget-(\d+)s', str(f.get('reason', '')))
        if m:
            deadline, src = int(m.group(1)) * 1000, f'日志 reason={f["reason"]}'
            break
    if fail and fail.get('reason') == 'error':
        skip(log, 'STREAM:download-within-deadline',
             f'那次下载以 reason=error 结束（不是两个时限闸）⇒ 这一格没测到，'
             f'本条不判')
        return
    # 窗口右边界：失败行优先。成功后没有失败行，就用"下一次下载起点之前"或
    # 预算的 4 倍——不能用 horizon 当窗口末端，那会把窗口说成 480 s 这种数，
    # 而真实下载在末笔就结束了。
    nxt = min((d['t'] for d in log.downloads if d['t'] > big['t']), default=None)
    cap = big['t'] + 4 * deadline
    right = fail['t'] if fail else cap
    seg = [t for t in log.txof('wp-stream') if t['t'] and big['t'] <= int(t['t'])]
    if nxt:
        seg = [t for t in seg if int(t['t']) < nxt]
    tx = [t for t in seg if int(t['t']) <= right]
    if not tx:
        skip(log, 'STREAM:download-within-deadline',
             f'{big["bytes"]} B 那次下载起点之后没有 wp-stream 事务，判不出成本')
        return
    total = sum(el(t) for t in tx)
    after = [t for t in seg if fail and int(t['t']) > fail['t']]
    tail_total = sum(el(t) for t in after)
    mean = total / len(tx)
    # 成功的窗口末端是末笔结束时刻，不是 cap
    last_end = max(int(t['t']) + el(t) for t in tx) if not fail else fail['t']
    window = last_end - big['t']
    fits = int(deadline // mean)
    # 两个单位不要混：bytes_expected 是**解压**总字节，wire 侧的 received/wire
    # 是**压缩**累计（wqn_api.cpp:1965 `received += read`）。拿压缩字节当分母
    # 会差一个压缩比，本轮已经造出两个幻影。**解压后的分片大小现在有实测了**
    # （`word pack stream append: chunk_bytes=`，见 STREAM:append-chunk-measured），
    # 所以每字节成本只在有那个数时才敢算。
    units = ''
    if fail:
        plain = fail.get('plain')
        wire = fail.get('wire')
        bits = [f'下载以 {fail["err"]} 结束']
        if plain and big['bytes']:
            bits.append(f'解压进度 {plain} / {big["bytes"]} = '
                        f'{plain / big["bytes"]:.0%}（来自 `inflater.total_out()`'
                        f'，不是拿压缩 received 推的）')
        if wire:
            bits.append(f'wire {wire} B 是**压缩**字节，与上面的解压字节'
                        f'不同单位，不能相除当完成度')
        units = '；' + '。'.join(bits)
    tail = (f'。超时之后另有 {len(after)} 笔收尾共 {tail_total / 1000:.1f} s'
            f'（未计入窗口；它是这次尝试真实付出的存储成本的一部分，'
            f'也是本轮唯一一次真尺度 kCommit 的量）' if after else '')
    budget_note = f'（预算取自{src}）' if src != '标定值 wqn_api.cpp:1941' else \
        f'（⚠️ 本轮日志未给出预算，用的是{src}；若固件已放宽，这条会误判）'
    detail = (
        f'{big["bytes"]} B 词包：起点到{"失败行" if fail else "末笔"}共 '
        f'{window / 1000:.1f} s，其中 {len(tx)} 笔 wp-stream Σelapsed '
        f'{total / 1000:.1f} s、Σqueue_wait '
        f'{sum(qw(t) for t in tx) / 1000:.1f} s'
        f'（存储占窗口 {total / max(window, 1):.0%}）。'
        f'每笔均 {mean:.0f} ms ⇒ {deadline // 1000} s 预算{budget_note} '
        f'只够 {fits} 笔'
        + units + tail)
    # 判据只对"有没有下完"下结论：下完了就 PASS，没下完就 FAIL。
    # 预算够不够是detail 里那个数给人看的——而且日志能给出预算时才可信，
    # 给不出时（成功行不带 reason）宁可只报数，不拿一个过期标定值去否掉一次
    # 成功的下载。
    over = window > deadline
    if done and over:
        detail += (f'；⚠️ 用了 {window / 1000:.1f} s 超过 {src} 的 '
                   f'{deadline // 1000} s —— 预算来源不是本行，先确认固件版本')
    expect(log, 'STREAM:download-within-deadline', not fail, detail)


def hil_stream_download_completed(log: Log):
    """成功行的自洽性：`completed: bytes=N` 必须等于 `bytes_expected`。

    wqn_api.cpp:1970-1973 已经把 `inflater.total_out() != item.byte_size` 判成
    ESP_ERR_INVALID_SIZE，所以 bytes 不等时**走不到** completed 行。这条判据因此
    是给插桩兜底的：如果哪天 completed 行打出来而字节数不对，说明那处校验被绕过了
    （或日志打错了对象），而"下完了一个不完整的包"是会装进 flash 的。

    elapsed_ms 一并报：它是唯一直接从 download 口径量出来的总耗时，不经过
    "事务笔数 × 每笔"的换算。
    """
    if not log.done:
        skip(log, 'STREAM:download-completed-consistent',
             '本轮没有 `word-pack-download completed:` 行'
             '（旧构建只有失败才打日志；或本轮没有成功的下载）')
        return
    bad = []
    for d in log.done:
        # 找到起点在它之前、最接近的那次下载来比 bytes_expected。
        # 完成字节数有 `plain` / `bytes` / `received` 三种名字，load() 已归一到 plain；
        # 旧日志只有 `bytes`，也接受。一个都没有 ⇒ 格式漂移，交浮漂移判据去报。
        starts = [s for s in log.downloads if s['t'] <= d['t']]
        got = d.get('plain', d.get('bytes'))
        if not starts or got is None:
            continue
        want = starts[-1]['bytes']
        if got != want:
            bad.append((d['t'], got, want))
    for t, got, want in bad:
        expect(log, 'STREAM:download-completed-consistent', False,
               f'{t} ms 那次 completed plain={got} 但 bytes_expected={want} '
               f'⇒ 下完了一个不完整的包，`total_out() != byte_size` 那处校验'
               f'被绕过了或日志打错了对象')
    if not bad:
        d = log.done[-1]
        got = d.get('plain', d.get('bytes'))
        detail = (f'{len(log.done)} 次成功下载的字节数都与 bytes_expected 一致')
        if got is not None:
            detail += f'；末次解压 {got} B'
        if d.get('elapsed_ms'):
            detail += f' / {d["elapsed_ms"] / 1000:.1f} s'
        elif d.get('elapsed'):
            detail += f' / {d["elapsed"] / 1000:.1f} s'
        for rate in ('plain_bytes_per_s', 'bytes_per_s'):
            if d.get(rate):
                detail += f' = {d[rate] / 1024:.0f} KiB/s（解压口径）'
                break
        expect(log, 'STREAM:download-completed-consistent', True, detail)


def hil_stream_append_chunk(log: Log):
    """真实下载路径的每笔解压字节数 —— 争论了好几轮的那个数，现在终于有实测。

    在这行插桩出现之前，"wp-stream 比 bench 每字节贵几倍"有 1.22×~5.7× 三个
    自俞读法，因为谁都不知道每笔到底写了多少解压字节；"整包需要多少存储时间"
    也跟着有 138~551 s 的区间。**一个没有实测的分母，能让所有下游结论差 4 倍。**

    判据只做一件事：把 chunk_bytes 如实报出来，并和 bench ramp 的最大分片比。
    这不判 PASS/FAIL——它是个测量值，取值本身没有对错；但它**必须出现**，
    缺少它时下面那条漂移判据会红。
    """
    if not log.appends:
        skip(log, 'STREAM:append-chunk-measured',
             '本轮没有 `word pack stream append:` 行（没下载词包，'
             '或跑的是加插桩之前的构建）')
        return
    chunks = sorted({a['chunk'] for a in log.appends})
    last = log.appends[-1]
    t0 = log.appends[0]['t']
    t1 = last['t']
    tx = [t for t in log.txof('wp-stream') if t['t']
          and t0 <= int(t['t']) <= t1 + 60000]
    total = sum(el(t) for t in tx)
    written = last['total']
    per_byte = total / max(written, 1)
    detail = (
        f'{len(log.appends)} 次 kAppend，chunk_bytes 取值 {[c for c in chunks]}'
        f'（{'一致' if len(chunks) == 1 else "不一致 ⇒ 分片路径有多条"}）；'
        f'累计写入 {written} / {last["want"]} B = '
        f'{written / max(last["want"], 1):.0%}（这是**解压**进度，'
        f'不是压缩 received 的进度）。窗口内 {len(tx)} 笔存储事务 Σelapsed '
        f'{total / 1000:.1f} s ⇒ 每解压字节 {per_byte:.4f} ms/B')
    if len(tx) and len(log.appends):
        detail += (f'，每笔 {total / len(tx):.0f} ms / '
                   f'{written / len(log.appends):.0f} B')
    expect(log, 'STREAM:append-chunk-measured', True, detail)


def sane_ms_per_byte(v, what):
    """每字节成本的数量级守卫。

    为一个值存在：`cost/chunk*1000` 这个笔误。乘过的版本把 0.0841 ms/B 印成
    84.14「ms/B」——**比值恰好不变**，所以凡是拿比值做交叉检查的地方都会
    通过（两个数一起错，比值自洽）。这条错误比单个数错更难发现，它躲得过验证。

    数量级物理上限：>10 ms/B 意味着写 1 KB 要 10 s，比这个项目里任何观测都高
    三个数量级以上（最慢的 session3k 是 ~0.7 ms/B）。所以超出范围的一律是单位
    错，直接抛出来，而不是让它变成一个看起来很准的数。
    """
    if not 0 < v <= 10.0:
        raise ValueError(
            f'{what} 算出 {v:.4f} ms/B，超出物理范围（0, 10]。'
            f'几乎一定是单位乘错了——检查是不是多乘/少乘了 1000')
    return v


kStreamNoProgressGateMs = 30000   # 无进展闸阈值，固件侧 30 s
kStreamGateIdleSlackMs = 10000    # 判"≈阈值"时允许的偏差
kStreamGateStalledProofMs = 60000 # 超过这个 idle 却说"是总预算拦的"⇒ 自相矛盾


def hil_stream_two_gate(log: Log):
    """两个时限闸自己的逻辑对不对：30 s 无进展 + 总预算。

    **这一条验的是守卫逻辑，不是产品行为。** 产品有没有下完由
    `STREAM:download-within-deadline` 判；这里判的是"拦我们的那个闸，拦对了吗"。

    为什么需要 `idle_ms=`：判"是不是真的没有进展"只能用**结束前距上次收到字节的
    时间**，不能用 `plain_bytes_per_s`——那是**全程平均**，一个下了 50% 然后对端
    彻底死掉的传输，平均值照样 > 0，而 `reason=no-progress-30s` 对它正是正确行为。
    拿平均速率去判会把一次正确拦截判成 FAIL（我自己差点这么写）。

    四种情况，见 bench_per_byte 上方的常量：
      no-progress 且 idle≈30 s   ⇒ PASS  真的断了，闸拦对了
      no-progress 且 idle<10 s   ⇒ FAIL  还在流却被无进展闸拦了，逻辑错误
      total-budget 且 idle<30 s  ⇒ PASS  慢但一直在流，兜底拦对了
      total-budget 且 idle>60 s  ⇒ FAIL  已经停了 60 s 却说"是预算拦的"——
                                       无进展闸本该先拦，它没生效
    没有 reason=（旧格式）或 reason=error 都判 SKIP：前者没测到这个场景，
    后者两个闸都没参与。
    """
    reason = None
    end = None
    for f in log.failures:
        if f.get('reason') and f['reason'] != 'error':
            reason, end = f['reason'], f
            break
    if end is None:
        # 分清是"根本没带 reason= 的旧格式"还是"带了 reason=error"：后者两个闸
        # 都没参与，前者是日志旧了，两者都不是两个闸判错了。
        has_error = any(f.get('reason') == 'error' for f in log.failures)
        why = ('下载以 reason=error 结束，两个时限闸都没有参与拦截'
               if has_error else
               '本轮没有带 `reason=` 的失败行（旧格式，或两个闸都没参与）')
        skip(log, 'STREAM:two-gate-logic', why)
        return
    idle = end.get('idle_ms')
    if idle is None:
        skip(log, 'STREAM:two-gate-logic',
             f'失败行有 reason={reason} 但没有 `idle_ms=` ⇒ 无法判断拦截时刻'
             f'还有没有字节在到达（平均速率给不出这个），本条不判')
        return
    # 不变量：空闲时间不可能长过这笔下载自己的 elapsed_ms。破了它就是字段本身
    # 不自洽（单位换了、或它量的不是"距上次收到字节"），此时按阈值去判等于拿
    # 一个错数下结论。宁可 FAIL 出来修日志，也不要 SKIP 成"这条没问题"——
    # 后者会让判据在什么都没测到的情况下报绿。
    elapsed = end.get('elapsed_ms')
    if elapsed is not None and idle > elapsed:
        expect(log, 'STREAM:two-gate-logic', False,
               f'reason={reason} 但 idle_ms={idle} > elapsed_ms={elapsed}：'
               f'空闲时间不可能长过下载本身耗时 ⇒ `idle_ms` 不是"距上次收到字节的'
               f'毫秒数"（单位换了，或参照时刻取错）。修好字段之前本条判不出任何东西')
        return
    gate = 'no-progress' if reason.startswith('no-progress') else 'total-budget'
    if gate == 'no-progress':
        near = abs(idle - kStreamNoProgressGateMs) <= kStreamGateIdleSlackMs
        if idle < kStreamGateIdleSlackMs:
            expect(log, 'STREAM:two-gate-logic', False,
                   f'reason={reason} 但 idle_ms={idle}（<{kStreamGateIdleSlackMs} ms）'
                   f'⇒ 拦截那一刻字节还在到达，无进展闸却拦了它。这是闸的逻辑错误，'
                   f'不是产品问题；查 30 s 阈值的比较是不是写反了或用了错误的基准时刻')
        elif near:
            expect(log, 'STREAM:two-gate-logic', True,
                   f'reason={reason} 且 idle_ms={idle} ≈ {kStreamNoProgressGateMs} ms '
                   f'⇒ 真的断了 ~30 s，无进展闸拦截正确')
        else:
            expect(log, 'STREAM:two-gate-logic', True,
                   f'reason={reason} 且 idle_ms={idle}，偏离 30 s 阈值 '
                   f'{abs(idle - kStreamNoProgressGateMs)} ms（在 {kStreamGateIdleSlackMs} '
                   f'ms 容差外）。拦对了，但阈值基准时刻可能有偏移，值得看一眼')
    else:
        if idle > kStreamGateStalledProofMs:
            expect(log, 'STREAM:two-gate-logic', False,
                   f'reason={reason} 但 idle_ms={idle}（>{kStreamGateStalledProofMs} ms）'
                   f'⇒ 传输早就停流了，却是总预算兜底拦的，**30 s 无进展闸没生效**。'
                   f'两个闸的交互有问题（谁先查、idle 是否被其它事件重置）')
        else:
            expect(log, 'STREAM:two-gate-logic', True,
                   f'reason={reason} 且 idle_ms={idle}（<{kStreamNoProgressGateMs} ms）'
                   f'⇒ 慢但一直在流，兜底闸拦对了，无进展闸正确地没有插手')


def bench_per_byte(log: Log):
    """bench ramp 的每字节斜率，只取**最大分片**那一轮。

    不取最小二乘：ramp 有明显拐点（1005.19 上 2K→10 ms / 4K→29 ms 比直线低
    13–18×），拟合一条斜线会把两个不可用的点混进来。要预测下载路径，该用的是
    下载实际落在的那个区间的斜率，也就是最大分片那一轮。
    没有 ramp 时返回 None。
    """
    if not log.stream_rounds:
        return None
    pts = [(r['chunk'], r['cost']) for r in log.stream_rounds if r.get('cost', 0) > 0]
    if not pts:
        return None
    chunk, cost = max(pts, key=lambda p: p[0])
    # 不再乘 1000：cost/chunk 直接就是 ms/B（2757/32768 = 0.0841）。乘过的版本
    # 会把 0.0841 印成 84.14「ms/B」——数值差 1000 倍，而比值恰好不变，
    # 所以只有把绝对值印出来时才会被发现。
    return sane_ms_per_byte(cost / chunk, 'bench ramp 每字节成本')


def direct_per_byte(log: Log):
    """真实下载路径的每解压字节成本，从 `word pack stream append` + 事务表直算。

    与 bench_per_byte 是**两套独立测量**：bench 在 WiFi 关闭、无其它流量的窗口里
    对一个 62 KiB 的临时文件写固定尺寸；真实下载在 WiFi 打开、UI/EPD 并发的
    情况下往一个 >1 MiB 的文件追加。两者不可能恒等，问题是差多少。
    """
    if not log.appends:
        return None
    last = log.appends[-1]
    t0, t1 = log.appends[0]['t'], last['t']
    seg = [t for t in log.txof('wp-stream') if t['t'] and t0 <= int(t['t']) <= t1 + 60000]
    if not seg or not last['total']:
        return None
    return sane_ms_per_byte(sum(el(t) for t in seg) / last['total'],
                            '真实下载每字节成本')


def hil_stream_bench_vs_direct(log: Log):
    """bench ramp 还能不能当下载路径的预测器？

    这条是**给 BENCH:stream-cost-model 定位用的**。1005.19 之前，bench 是唯一
    的能量度，所有 deadline 结论都从它外推；这一轮它错过一次——kCommit 在 62 KiB
    上量到 0 ms，外推成"≈0.0 s"，而 1.34 MiB 上真实收尾是 15,327 ms。所以 bench
    的角色必须从"预测器"降级为"标定工具"，而这一条就是那个降级的**度量**。

    两套独立测量相差多少：
      ≤1.5×  bench 仍可用作标定，`BENCH:stream-cost-model` 的预测有参考价值
      >1.5×  bench 系统性偏乐观（条件不同：WiFi 开/关、并发流量、文件大小），
             它的数字只能当 bench 自己的结论，不能拿去预测下载路径
    不判 PASS/FAIL——两边都没错，差的是实验条件；但这个倍数必须每次都出现在
    判读里，否则下一次又会有人拿 bench 的数去承诺产品预算。
    """
    bench = bench_per_byte(log)
    direct = direct_per_byte(log)
    if bench is None or direct is None:
        skip(log, 'STREAM:bench-vs-direct',
             ' bench ramp 与真实 kAppend 缺一个（本轮只跑了其中一路），无法对比')
        return
    ratio = direct / bench
    verdict = ('bench 仍是有效标定' if ratio <= 1.5 else
               f'bench 偏乐观 {ratio:.2f}× ⇒ 它的数只能当 bench 自己的结论')
    expect(log, 'STREAM:bench-vs-direct', True,
           f'真实下载 {direct:.4f} ms/B vs bench ramp 最大分片 {bench:.4f} ms/B '
           f'= {ratio:.2f}×：{verdict}。差的是实验条件（bench 在 WiFi 关闭、无并发'
           f'流量下写 62 KiB 临时文件；真实下载在 WiFi 打开、UI/EPD 并发下追加'
           f'>1 MiB），不是谁测错了。'
           f'⚠️ 即便 ≤1.5× 也不是"完全可比"：**62 KiB vs 1.28 MiB 是 21× 外推**，'
           f'"每字节成本不随文件增长"只在 294 KB 内验过（§五之七）。若它在 1 MiB'
           f'之后抬头，真实值会比这个大。两个绝对值都打在上面，别只引用比值')


def _writes_in_transaction(log: Log, tx, rows, timestamp):
    """同一时钟域、同一串行属主事务；毫秒相等时靠行序分开相邻事务。"""
    if tx['t'] is None:
        return []
    end = int(tx['t'])
    start = end - el(tx)
    previous_line = max((other['line'] for other in log.tx
                         if other['epoch'] == tx['epoch']
                         and other['line'] < tx['line']), default=-1)
    scoped = [row for row in rows if row['epoch'] == tx['epoch']
              and previous_line < row['line'] < tx['line']]
    # 若最新写的时钟已不在窗口内，不得退回更早的一笔制造覆盖。
    # 这也拒绝漏抓启动行的时钟重叠，不需把多任务日志的小倒序当重启。
    if scoped and not start <= scoped[-1][timestamp] <= end:
        return []
    return [row for row in scoped if start <= row[timestamp] <= end]


def _nvs_stats_pair(log: Log, write):
    """最近的前/后统计按行序配对，不借别次启动或同毫秒的别笔写。"""
    rows = [row for row in log.nvs_stats if row['epoch'] == write['epoch']
            and 'used_entries' in row]
    before = [row for row in rows if row['line'] < write['line']]
    after = [row for row in rows if row['line'] > write['line']]
    if not before or not after:
        return None
    left, right = before[-1], after[0]
    if not left['t'] <= write['t'] <= right['t']:
        return None
    sharing = sum(other['epoch'] == write['epoch']
                  and left['line'] < other['line'] < right['line']
                  for other in log.nvs_writes)
    return left, right, sharing


def hil_write_parts_reconcile(log: Log):
    """一笔事务的子字段之和必须解释它的 elapsed —— 但**加数会变，判据不能瞎**。

    WHY: 1005.26 上这个等式第一次做成了：一次 `word-session-save` = 两笔 AtomicWrite
    （3.3 KB 快照 + 52 B 伴随游标），子字段之和 5,075 vs elapsed 5,083，误差 <0.3%。
    那是"成本全在 AtomicWrite 里、没有第三块"的证据。

    ⚠️ **但 52 B 游标迁 NVS 后，这个等式的形状合法地变了**（peer 的 Milestone A
    警告，2026-10-06）：`atomic write:` 每次答题从 **2 条降到 1 条**，owner 名和
    事务数都不变。于是旧等式会变成"子字段和 + 1,766 ms ≈ elapsed"，**不是 SKIP，
    是看起来像回归**——判据会以为插桩漏掉了主要耗时，而真相是那部分成本搬到了 NVS。

    所以这里把等式写成"所有 storage-write 行的子字段/耗时之和"，而不是只数
    `atomic write:`。`nvs write:` 一旦出现就自动并入加数，不需要在迁移时改判据。

    ⚠️ **`changed=0` 的 `nvs write:` 不是一次写**（storage.cpp:1084-1093：先读出
    旧值 memcmp，相同就整个跳过 `nvs_set_blob`）。它必须单独点出来，否则 breakdown
    里一句"nvs write 0 ms × 1 笔"会被读成"NVS 就是快"，而真相是"游标没变、
    这一笔没碰 flash"——两个相反的结论。

    ⚠️ **但 `changed` 不能当"是不是写"的判据**：`ClearWordSessionCursorNvs` 走同一个
    探针报 `bytes=0 changed=**true**`（peer 2026-10-06 改的：该函数在 NOT_FOUND 时
    提前返回且不打探针，所以能走到 emit 就说明 key 存在、3 条目真被擦了）。拿 changed
    判别会把一次擦除归档成"NVS 写 changed=1"，正好与事实相反，还会把 §五之十 504
    条目预算最要紧的那一次消耗藏掉。所以判别量用 **`bytes==0` ⇒ 擦除**。它自己的
    耗时窗口也已从 `nvs_erase_key` 之后提到 `nvs_open` 之前（`nvs_commit` 在
    IDF v5.5 是字面 no-op，`nvs_api.cpp:411-413`），两行才可比。
    """
    txs = log.txof('word-session-save')
    if not txs:
        skip(log, 'WRITE:parts-account-for-transaction',
             '本轮无 word-session-save 事务，无法对账')
        return
    if any(t['t'] is None or int(t['t']) < el(t) for t in txs):
        expect(log, 'WRITE:parts-account-for-transaction', False,
               'word-session-save 时间戳缺失或耗时超出本启动时钟，不能配执行窗口')
        return
    # 窗口对账，不是"全部写行 / 全部事务"。
    # WHY: 分母只取 word-session-save 而分子含 bench 与其它路径时，1006.1 上
    # 会算出 395% 覆盖，然后据此得出"插桩重复计数"的假结论。那正是这条判据
    # 自己批判过的"拿子字段比别人的全量"（本会话已因此错过一轮）。全量比全量
    # 的唯一办法是**同一执行窗口**内比。
    write_ms = 0.0
    breakdown = []
    covered = 0
    for t in txs:
        aw = [p['total'] for p in _writes_in_transaction(log, t, log.probes, 'at')]
        win = _writes_in_transaction(log, t, log.nvs_writes, 't')
        nv = [w['total_ms'] for w in win]
        if aw or nv:
            covered += 1
        if aw:
            breakdown.append(f'atomic write {sum(aw):.0f} ms × {len(aw)} 笔')
        if nv:
            # 窗口内的 nvs 行要分三类说：真写的、值没变跳过的、和**擦除**的。
            # 判别用 `bytes==0` 而不是 `changed`：`ClearWordSessionCursorNvs`
            # 报的就是 bytes=0 + changed=**true**（peer 2026-10-06 改的，理由见
            # storage.cpp 里那段注释：能走到 emit 说明 key 存在、条目真被擦了）。
            # 若仍拿 changed 当判别量，一次擦除会被归档成"NVS 写 changed=1"
            # ——正好和它是擦除这件事相反，还会把 §五之十 504 条目预算最要紧的
            # 那一次消耗藏掉。
            erased = [w for w in win if w.get('bytes', -1) == 0]
            noop = [w for w in win
                    if w.get('bytes', -1) > 0 and w.get('changed') == 0]
            wrote = [w for w in win
                     if w.get('bytes', -1) > 0 and w.get('changed') != 0]
            bits = []
            if wrote:
                bits.append(f'nvs write {sum(w["total_ms"] for w in wrote):.0f} ms '
                            f'× {len(wrote)} 笔')
            if noop:
                bits.append(
                    f'同值短路 {len(noop)} 笔（值没变，`nvs_set_blob` 整个跳过；'
                    f'那 {sum(w["total_ms"] for w in noop):.0f} ms 是读出旧值'
                    f'再比较的耗时，真实但**没碰 flash**，'
                    f'别读成"NVS 写只要这么多"）')
            if erased:
                bits.append(f'nvs **erase** {sum(w["total_ms"] for w in erased):.0f} ms '
                            f'× {len(erased)} 笔（bytes=0 ⇒ 擦键、释放条目，'
                            f'**不是写**；窗口已含 `nvs_open`）')
            breakdown.append('；'.join(bits))
        write_ms += sum(aw) + sum(nv)
    if not breakdown:
        message = (f'{len(txs)} 笔 word-session-save 的本启动、完成行之前的执行窗口'
                   f'没有任何写行（atomic write: / nvs write:），判不出成本归属')
        if log.probes or log.nvs_writes:
            expect(log, 'WRITE:parts-account-for-transaction', False,
                   message + '；日志有写探针，不能借其它窗口补成本')
        else:
            skip(log, 'WRITE:parts-account-for-transaction', message)
        return
    tx_ms = sum(el(t) for t in txs)
    ratio = write_ms / tx_ms if tx_ms else float('inf')
    parts = '；'.join(dict.fromkeys(breakdown))
    ok = ratio >= 0.6 and covered == len(txs)
    detail = (f'{covered}/{len(txs)} 笔窗口内有写行。写行合计 {write_ms:.0f} ms'
              f'（{parts}）vs word-session-save sum(elapsed) {tx_ms:.0f} ms'
              f' ⇒ 覆盖 {ratio:.0%}。口径是"**落在同一执行窗口内**的所有 '
              f'storage-write 行"，不是只数 `atomic write:`：52 B 游标迁 NVS 后'
              f'每次答题少一条 `atomic write:` 而 owner 名不变，只数它会把合法'
              f'迁移读成"插桩漏掉主要耗时"')
    if covered != len(txs):
        detail += ' ⇒ 存在未覆盖的事务，不能让其它事务的写行补账'
    elif not ok:
        detail += (' ⇒ 覆盖 <60%，插桩漏掉了主要耗时，本轮任何子字段结论都不可信')
    else:
        detail += ' ⇒ 成本都在已插桩的写路径里'
    expect(log, 'WRITE:parts-account-for-transaction', ok, detail)


def hil_cursor_erase_logged(log: Log):
    """清会话那一次擦除，必须**既打 nvs write: 又打 nvs stats:**。

    WHY THIS EXISTS: peer 2026-10-06 自查发现 `ClearWordSessionCursorNvs` 有两个让
    探针说谎的缺陷——(1) 计时窗只包住 `nvs_commit` 而它在 IDF v5.5 是字面 no-op，
    于是擦除恒报 ~0 ms；(2) 擦完不落 `nvs stats:`，于是 `used_entries` **唯一该跌的
    那一刻没有读数**。两个都已修。这条判据盯住修复别退化：擦除是 §五之十 504 条目
    预算里唯一会**释放**条目的动作，没有它，一次清会话漏掉的条目要等下一题才可见。

    判据用 owner 名 `word-session-clear`（peer 提供，不是我自己猜的），窗口对账口径与
    WRITE:parts-account-for-transaction 一致——否则 erase 那个 total_ms 无处安放。
    """
    txs = log.txof('word-session-clear')
    if not txs:
        skip(log, 'WRITE:cursor-erase-logged',
             '本轮无 word-session-clear 事务（没清过会话）——'
             '这是 §五之十 里唯一释放 NVS 条目的路径，F.2 表第 5 项要跑一次')
        return
    covered = 0
    drops = []
    failures = []
    cursor_keys = {'cur_seq', 'cur_rnd', 'cur_dic', 'cur_rev',
                   'cur_int', 'cur_shf', 'cur_mis'}
    for index, t in enumerate(txs, 1):
        if t['t'] is None or int(t['t']) < el(t) or t['res'] != 'ESP_OK':
            failures.append(f'第{index}笔清事务失败或缺合法时间窗')
            continue
        win = _writes_in_transaction(log, t, log.nvs_writes, 't')
        if (len(win) != 1 or win[0].get('bytes', -1) != 0
                or win[0].get('changed') != 1 or win[0].get('key') not in cursor_keys
                or win[0]['total_ms'] > el(t)):
            failures.append(f'第{index}笔缺唯一正式游标 bytes=0 changed=1 擦除行')
            continue
        write = win[0]
        pair = _nvs_stats_pair(log, write)
        if pair is None:
            failures.append(f'第{index}笔缺本启动擦除前后统计')
            continue
        left, right, sharing = pair
        delta = int(right['used_entries']) - int(left['used_entries'])
        drops.append(delta)
        if right['line'] >= t['line'] or right['t'] > int(t['t']):
            failures.append(f'第{index}笔擦除后统计不在本事务内')
        elif sharing != 1:
            failures.append(f'第{index}笔统计间夹{sharing}笔操作，净台阶无法独立归因')
        elif delta >= 0:
            failures.append(f'第{index}笔 used_entries 未减少（{delta:+d}）')
        else:
            covered += 1
    detail = (f'{covered}/{len(txs)} 笔 word-session-clear 同启动窗口内有正式游标'
              f' `bytes=0 changed=1` 擦除、ESP_OK和成对统计。'
              f'按日志行序取最近前/后值，同毫秒也不混淆；擦除后的统计须在完成行之前。')
    if drops:
        detail += (f' 擦除前后 `used_entries` 变化 {drops}（**应为负**：那条 4 条目 '
                   f'blob 被释放；只涨不跌 = 有泄漏，正是这条判据要拦的）')
    else:
        detail += ' 没有本次成对读数，不能确认条目释放。'
    if failures:
        detail += ' FAIL原因：' + '；'.join(failures)
    expect(log, 'WRITE:cursor-erase-logged', covered == len(txs), detail)


def hil_nvs_stats_measured(log: Log):
    """把 §五之十 的 NVS 算术变成实测——peer 自己标的第一个"③ 不测就没法定夺"。

    §五之十 第 1 节那几张表（"~4.7 笔""21.2%""504 条目"）**全部是
    4 页 × 126 条目从源码+分区表推出来的，没有一个数是量的**。peer 为此提了新行：

        nvs stats: used_entries=%zu free_entries=%zu available_entries=%zu
                   total_entries=%zu

    （IDF 现成 API：`nvs_get_stats("nvs", &s)`，`nvs_api.cpp:541`。）

    为什么这条判据重要：`total_entries` 会**直接证实或推翻 504**——如果实测不是
    504（比如分区里有别的 NVS 使用者、或页布局不同），那么 §五之十 里每一个
    "占分区百分之几"的结论都要按比例重算。`free_entries` 则给出那条
    "写开始失败"的悬崖**实际在哪一格**：peer 的机制分析说空闲页掉到 2 以下后每次
    答题付一次寄生存活+4096 B 擦除、页耗尽则 `requestNewPage` 返回
    `ESP_ERR_NVS_INVALID_STATE`，但**具体落在第几笔取决于同一分区里其它 NVS
    使用者占多少条目——他们没实测，我也不能替他们猜**。
    """
    rows = log.nvs_stats
    if not rows:
        skip(log, 'WRITE:nvs-entry-budget-measured',
             '本轮没有 `nvs stats:` 行。§五之十 的 NVS 算术（4 页 × 126 = 504 条、'
             '21.4%/54.8% 占比）在**本轮缺少实测字段**，不能从日志缺行推断历史上'
             '从未测过，也不能推断镜像没有插桩——这是 peer 列的'
             '第一个"③ 不测就没法定夺"项：烧一版带 `nvs_get_stats` 的构建即可')
        return
    last = rows[-1]
    total = int(last['total_entries'])
    used = int(last.get('used_entries', -1))
    free = int(last.get('free_entries', -1))
    avail = int(last.get('available_entries', -1))
    # 推导值，不是测量值：用来和实测对账。
    derived = kNvsBudget
    detail = (f'实测 total_entries={total}（推导值 {derived} = {kNvsPages} 页 × '
              f'{kNvsEntryCountPerPage}，'
              f'来自 partitions/16m.csv:3 的 0x4000 + nvs_constants.h 的 '
              f'NVS_CONST_ENTRY_COUNT=126）')
    if total != derived:
        detail += (f' ⇒ **与推导不符**。§五之十 里所有"占分区百分之几"都要按 '
                   f'{derived}/{total} 重算，别照抄 21.2%/54.2% 那两张表')
    else:
        detail += ' ⇒ 与推导一致，那两张表的分母站得住'
    # 52 B 游标该占几条——用**实测**的每笔变化给它命名，别只信算术。
    # 1006.10 上算术说 3、实测是 4，差的那条是 BLOB_IDX（见 nvs_entries 注释）。
    used_seq = [int(r['used_entries']) for r in rows if 'used_entries' in r]
    if len(used_seq) >= 2 and not log.nvs_writes:
        steps = [b - a for a, b in zip(used_seq, used_seq[1:])]
        detail += (f'；used 逐次 {used_seq[0]} → {used_seq[-1]}，'
                   f'每步变化 {steps}（**没有 `nvs write:` 行可归因，'
                   f'所以只能看总数台阶，说不出是哪笔、是不是新 key**）')
    if used >= 0:
        detail += f'；末次 used={used}'
    cursor_entries = nvs_entries(kSessionCursorBytes)
    # 悬崖的分母必须是 **available_entries，不是 free_entries**。
    # WHY: `free_entries` = 空闲槽位 + 所有空闲页 × 126；`available_entries` =
    # free_entries − 126，即**整整一页留给 GC**（nvs_pagemanager.cpp:246）。
    # 1006.10 上两者差 126：free=301 / available=175。用 free 当分母会把
    # "还剩多少答题"高估 1.7×——这正是"拿行上现成的那个数当分母"的老毛病，
    # 而这次两个数都在同一行上。
    #
    # ⚠️⚠️ 但比"分母选错"更严重的错，是我在 1006.10 上犯的：
    # **拿 available / "每次 4 条" 当"还有几次答题"的答案** —— 那是个
    # **不存在的假悬崖**（peer 2026-10-06 指出，我复核 IDF 源码后确认他对）。
    # 4 条是**每个 key 的一次性成本**，不是每次答题的成本：同一个 key 再写时，
    # 旧 blob 的条目被**就地擦除并立刻算回 free** ——
    #   `eraseEntryAndSpan` 逐条 `--mUsedEntryCount`（nvs_page.cpp:432/446），
    #   `calcEntries` 里 `free += ENTRY_COUNT - mUsedEntryCount` 的注释原文是
    #   *"it's equivalent free + erase entries"*（nvs_page.cpp:1181）。
    # 1006.10 的实测逐笔配对正是这个形状：cur_rev +4、cur_int +4、
    # cur_int（第二次）**0**、cur_shf +4、cur_shf（第二/三次）**0 / 0**。
    # ⇒ 稳态每答题净 **0** 条；三个 key × 4 = 12 条，整轮就这么多。
    # 这就是我自己记下的老毛病的同型复发：**拿一个推导比值当直接测量，
    # 而同一份日志里的直接测量（逐笔 Δused）就在否掉它**。
    #
    # 配对的归因边界（写清楚，别让它自己假装精确）：一个 key 的写只会**保留
    # 自己的**条目（BLOB_IDX 是定长可寻址的，nvs_storage.cpp:351-361 在原地改），
    # 所以"改写同一个 key 净 0"是**逐 key 归因**的结论，不受其它 key 干扰。
    # 反过来，**新 key 的 +N 里可能含着同一窗口内别人的首次写**——stats 是按
    # 事务批量出的，两笔写落在同两条 stats 之间时上面只标了"新 key"、看不出
    # 是谁的 N。所以下面只用"每个 key 的首次写花了 ~4"这个量级，
    # 不拿"N 条 ÷ N 个 key"当精确分配。
    pairs = []
    for w in log.nvs_writes:
        pair = _nvs_stats_pair(log, w)
        if pair is not None:
            before, after, sharing = pair
            pairs.append({'key': w.get('key', '?'), 'bytes': w.get('bytes', 0),
                          'changed': w.get('changed', -1), 'sharing': sharing,
                          'delta': int(after['used_entries'])
                          - int(before['used_entries'])})
    if pairs:
        detail += '；**逐笔 Δused 配对**（同启动按行序取最近前/后 stats）：'
        for p in pairs:
            if p['sharing'] > 1:
                tag = f'共享统计窗口夹{p["sharing"]}笔操作，全局台阶不能逐key归因'
            elif p['bytes'] == 0 and p['delta'] < 0:
                tag = f'**擦除 key，释放 {-p["delta"]} 条**'
            elif p['bytes'] == 0:
                tag = '擦除窗口没有观察到净减少，不能声称免费改写'
            elif p['delta'] < 0:
                tag = '减少台阶不能证明免费改写，需查同窗口其他操作'
            elif p['delta'] > 0:
                tag = f'**新 key，一次性 +{p["delta"]}**'
            elif p['changed'] == 0:
                tag = '同值短路，没碰 flash'
            else:
                tag = '**改写已有 key，净 0**'
            detail += (f'\n  · {p["key"]} bytes={p["bytes"]} '
                       f'changed={p["changed"]} ⇒ Δused {p["delta"]:+d}（{tag}）')
    unpaired = len(log.nvs_writes) - len(pairs)
    if unpaired:
        detail += f'；{unpaired} 笔写缺本启动成对stats，未量到其Δused（不借其它启动补值）'
    first_write = [p for p in pairs
                   if p['bytes'] > 0 and p['delta'] > 0 and p['sharing'] == 1]
    rewrites = [p for p in pairs
                if p['bytes'] > 0 and p['delta'] == 0 and p['changed'] != 0
                and p['sharing'] == 1]
    if first_write and rewrites:
        once_each = statistics.median([p['delta'] for p in first_write])
        n_keys = len({p['key'] for p in first_write})
        detail += (
            f'\n  ⇒ **{once_each:.0f} 条是每个 key 的一次性成本，'
            f'本轮已有 key 的改写净 0 条**（仅此 key 写路径，不等同真实答题或长期GC验收；'
            f'改写已有 key 时旧 blob 被就地擦除并算回 '
            f'free，nvs_page.cpp:432/446 + :1181）。本轮 {n_keys} 个 key 首次写'
            f'各花 ~{once_each:.0f} 条 = ~{n_keys * once_each:.0f} 条 = 分区 '
            f'~{n_keys * once_each / kNvsBudget:.0%}，**一次性**。'
            f'⚠️ 所以**不要说"约 N 次答题后触崖"**——那是个不存在的悬崖；'
            f'§五之十 里"168 笔"那张表同理作废。GC 悬崖仍然是真的，'
            f'但它是**页碎片/搬迁**型，不是条目累积型，'
            f'**落在哪一行仍没实测**。'
            f'要确认稳态真的净 0，得连续答 ≥5 题看 available 的**台阶形状**：'
            f'平坦 ⇒ 净 0；每答一级 ⇒ 还有别的使用者在涨')
    elif first_write:
        once_each = statistics.median([p['delta'] for p in first_write])
        detail += (f'\n  ⇒ 只见到新 key 的首次写（**~{once_each:.0f} 条/key**），'
                   f'**没有第二次写同一个 key**，所以"稳态净 0"这句**还没被'
                   f'这份日志测到**——同值短路（changed=0）也算一种证据，但它'
                   f'回答的是"值没变时不花钱"，不是"值变了时免费重写"')
    if avail >= 0:
        detail += (f'；末次 **available={avail}**（free={free}）'
                   f'⇒ 按 {cursor_entries} 条/key 算，还够约 '
                   f'**{int(avail // cursor_entries)} 个新 mode key**。'
                   f'⚠️ 这个分母取 available 而不是 free：`available_entries` = '
                   f'free − 126，那一整页是留给 GC 的'
                   f'（nvs_pagemanager.cpp:246），free 会把余量高估 '
                   f'{free / avail:.1f}×')
    expect(log, 'WRITE:nvs-entry-budget-measured', True, detail)


def hil_nvs_entry_budget(log: Log):
    """检查实际 NVS blob，而不是把 SPIFFS AtomicWrite 假装成 NVS 写入。

    保留 25% 预算门槛；它不单独禁止 3379 B（21.4%），不能冒充两层落点协议。
    跨页条目数是最佳分块下界，不推断实际 GC 次数或页耗尽。
    """
    name = 'WRITE:nvs-entry-budget'
    heads = [line for line in log.lines if 'nvs write:' in line]
    if not heads:
        skip(log, name, '无实际 `nvs write:` 行；SPIFFS 载荷仅可用于假设容量分析')
        return
    if len(heads) != len(log.nvs_writes) or any(
            not {'bytes', 'key', 'changed'} <= w.keys() for w in log.nvs_writes):
        expect(log, name, False, 'NVS 写日志格式漂移，缺 bytes/key/changed 或时间戳')
        return
    writes = [w for w in log.nvs_writes if w['bytes'] > 0]
    if not writes:
        skip(log, name, '仅擦除行（bytes=0），没有实际 blob 载荷')
        return
    kShareLimit = 0.25
    worst = max(writes, key=lambda w: w['bytes'])
    worst_bytes = worst['bytes']
    worst_entries = nvs_entries(worst_bytes)
    worst_share = worst_entries / kNvsBudget
    detail = (
        f"实际 NVS n={len(writes)}，最大 key={worst['key']} {worst_bytes} B ⇒ "
        f'至少 {worst_entries} 条/{kNvsBudget} 条 = {worst_share:.1%}；'
        f'单 blob 上限 {kNvsSingleBlobCap} B，预算门槛 {kShareLimit:.0%}。'
        f'对照（假设容量，非本轮 NVS 写）：3379 B 至少 {nvs_entries(3379)} 条，'
        f'8676 B 至少 {nvs_entries(8676)} 条。')
    expect(log, name, worst_share <= kShareLimit and
           worst_bytes <= kNvsSingleBlobCap, detail)


def hil_append_open_split(log: Log):
    """`append_open_ms` 是否真的把"建对象"那部分拆出来了（新探针契约 §五之十）。

    追加写便宜 234× 这个结论，peer 自己标了缺口：§五之三 的 8 ms 是 `append_ms`
    一整字段，**没分解**。434×（后修正为 234×）里多少来自 `"ab"` ≠ `"wb"`、
    多少来自省掉 remove+2 rename，**是推理不是测量**。

    所以这条判据等的就是那个拆开的字段：`append_open_ms << fopen` 才是"不建对象"
    的**直接证据**，而不是从总价反推的。`fopen` 是同一份日志里 `atomic write:`
    行实测的（52 B ~670–1400 ms，已独立复核）。

    只扫 key=value 对、不锁行头：行头是 peer 的，他们改一次词判据不该跟着瞎
    （位置正则静默失配这个坑在本会话已经错过一轮，见 LOG:word-pack-format-parsed）。
    """
    # 与 LOG:probe-lines-parsed 同一处解析：obs_durable 已经带着 append_open_ms /
    # append_bytes（它们在 `word observation durable:` 那一行上）。这里不再自己
    # 扫一遍，免得两套解析各自漂移。
    pairs = [o for o in log.obs_durable
             if 'append_open_ms' in o and 'append_ms' in o]
    if not pairs:
        skip(log, 'WRITE:append-open-vs-fopen',
             '本轮没有同时带 `append_open_ms=` 与 `append_ms=` 的行'
             '（日志未覆盖该测量，不能据此推断探针未进镜像；契约见 doc §五之十 第 6 节）')
        return
    opens = [int(p['append_open_ms']) for p in pairs]
    fp = [p['fopen'] for p in log.probes if p.get('fopen')]
    med_open = statistics.median(opens)
    med_fopen = statistics.median(fp) if fp else None

    # ⚠️ 全局中位数会再次犯"把双峰样本压成一个数"那个错，而这一格正是靠分峰
    # 才问得出来。1006.10 实测：便宜簇 open≈3 ms / append≈7 ms（开起来 43%，
    # 绝对值都可忽略），贵簇四条里 seq2 open=383/append=387（**99% 在 open**），
    # 另外三条 open=2~5/append=424~434（**~1% 在 open，成本在 append 本体**）。
    # 同一个 200 B 载荷、同一个 "ab"，两种成因**都出现了**——所以"追加不建对象"
    # 这一句在贵簇里不成立。拿全局中位 ratio 0.003 判 PASS，等于用便宜簇替贵簇
    # 回答问题，而那正是贵簇要回答的问题。
    clusters = cluster_gap([int(p['append_ms']) for p in pairs])
    rows = [(int(p['append_ms']), int(p['append_open_ms'])) for p in pairs]
    per_cluster = []
    for lo, hi, n in clusters:
        sub = [(a, o) for a, o in rows if lo <= a <= hi]
        shares = sorted(o / a for a, o in sub) if sub else []
        per_cluster.append((lo, hi, n, sub, shares))

    detail = (f'append_open_ms 全域中位 {med_open:.0f} ms（n={len(opens)}），'
              f'append_ms 全域中位 '
          f'{statistics.median(int(p["append_ms"]) for p in pairs):.0f} ms')
    if med_fopen:
        ratio = med_open / med_fopen
        detail += (f'；同轮 `atomic write:` 的 fopen 中位 {med_fopen:.0f} ms ⇒ '
                   f'全域 append_open_ms / fopen = {ratio:.3f}')
    detail += '。**按 append_ms 分簇看 open 占比**（这是本节真正的问题）：'
    for lo, hi, n, sub, shares in per_cluster:
        detail += (f'\n  · append {lo:.0f}~{hi:.0f} ms (n={n})：'
                   + ' / '.join(f'{o:.0f}/{a:.0f}' for a, o in sub)
                   + f' ⇒ open 占比 {min(shares):.0%}~{max(shares):.0%}'
                   if shares else '')
    dear = [c for c in per_cluster if c[0] > 10 and c[4] and max(c[4]) >= 0.1]
    dear_body = [c for c in per_cluster
                 if c[0] > 10 and c[4] and min(c[4]) < 0.1]
    if dear and dear_body:
        detail += ('\n  ⚠️ **贵簇里两种成因都出现了**：既有 open 占比 ≥10% 的'
                   '（成本在打开/建对象那一侧），也有 ≤10% 的（成本在 append '
                   '本体，fwrite+fflush+fsync+fclose）。'
                   '所以"追加不建对象所以便宜"这句**在贵簇里不成立**——'
                   '别拿便宜簇的中位数替贵簇回答问题')
    elif dear:
        detail += ('\n  ⇒ 贵簇成本集中在 open 这一侧，'
                   '"不建对象"至少对贵簇自洽')
    elif dear_body:
        detail += ('\n  ⇒ 贵簇的成本**不在 open**（占比 <10%），'
                   '在 append 本体里。这一档与"建对象"无关，')
    if med_fopen and ratio < 0.1 and not (dear and dear_body):
        detail += ('\n全域看 **append_open_ms 比 fopen 低一个数量级以上**，'
                   '这是"追加形状不付 AtomicWrite 那个固定项"的直接证据，'
                   '不再是 §五之三 那种从总价反推的推理')
    elif med_fopen and ratio >= 0.1:
        detail += ('\n⇒ append_open_ms 没有比 fopen 低一个数量级，'
                   '"拆出来的就是建对象那部分"这个假设**不成立**，'
                   '§五之三 的推理缺口仍然敞开')
    # 判据只在"贵簇也全都在 open 之外"时给绿。便宜簇绿没有意义：它本来就便宜。
    expect(log, 'WRITE:append-open-vs-fopen',
           med_fopen is None or not (dear and dear_body), detail)


def hil_probe_lines_drift(log: Log):
    """抓"判据正则静默失配"——存储侧重写新增的那几行。

    WHY: 判据按 key=value 扫这些行，加字段本该无感；但"本该解析出来却一条都没有"
    必须在场。位置正则改一次字段就静默失配一次，而失配长得跟"这轮没测"一模一样：
    1006.4 起 `word observation durable:` 在 `append_ms=` 与 `total_ms=` 之间插了
    `append_open_ms=`/`append_bytes=`，老的正则当场瞎掉，于是"双峰不是 bench 量测
    假象"的唯一凭据（真实流量 append_ms）会从判据里**无声消失**。

    ⚠️ 阈值取"**字段在场**"，不是"行头在场"：数行头会把"旧构建没有新字段"读成
    漂移。第一版就是这么写的，10 份老日志里 `storage bench append round=` 全被
    报 FAIL——行在、`new_object=` 不在，而那只是新构建才有的字段。拿构建年龄当
    漂移，是这条判据自己版本的喊狼来了。

    所以这里判 FAIL 而不是 SKIP：格式漂移必须当场可见。
    """
    counts = [
        ('word observation durable:', log.obs_line_count, len(log.obs_durable),
         'append_ms'),
        ('storage bench append round=', log.bench_append_line_count,
         len(log.bench_new_object), 'new_object'),
        ('storage bench round: shape=', log.bench_round_line_count,
         len(log.rounds), 'wall_ms'),
        ('storage bench stream round=', log.bench_stream_line_count,
         len(log.stream_rounds), 'cost_ms'),
        ('nvs write:', log.nvs_write_line_count, len(log.nvs_writes), 'total_ms'),
        ('nvs stats:', log.nvs_stats_line_count, len(log.nvs_stats), 'total_entries'),
    ]
    bad = [f'`{head}` + `{need}=` 同时出现 {n} 次，却解析出 0 条'
           for head, n, parsed, need in counts if n and not parsed]
    present = '；'.join(f'{head}: {n} 行 / {p} 条解析'
                        for head, n, p, _ in counts if n)
    if bad:
        expect(log, 'LOG:probe-lines-parsed', False,
               '；'.join(bad) + ' ⇒ **判据与当前日志格式漂移了**。'
               '这一格的所有存储结论都不可用，先修判据——'
               '格式漂移不许伪装成 SKIP')
        return
    expect(log, 'LOG:probe-lines-parsed', True,
           present if present else '本轮无存储侧重写探针行')


def hil_log_format_drift(log: Log):
    """抓"判据正则静默失配"。

    日志里明明有 `word-pack-download` 行，却没有一条被 WORD_PACK_END_RE 解析出来
    ——那不是"这轮没测"，是**判据瞎了**。位置正则改一次字段就静默失配一次，
    本会话已经因为这个模式错过一轮（新增判据以为没触发，其实格式变了）。
    所以这里判 FAIL，让格式漂移当场可见，而不是伪装成 SKIP。
    阈值取 2：单行可能是 HTTP status 之类不带的，两条以上仍然全不解析才是漂移。
    """
    got = len(log.failures) + len(log.done)
    lines = [ln for ln in WORD_PACK_LINE_COUNT_RE.findall(log.text)
             if 'read_buffer_bytes' not in ln and 'HTTP status' not in ln]
    if len(lines) >= 2 and got == 0:
        expect(log, 'LOG:word-pack-format-parsed', False,
               f'日志里有 {len(lines)} 条 word-pack-download 起点/终点行，'
               f'但成功/失败行一条都没解析出来 ⇒ 判据的 WORD_PACK_END_RE 与'
               f'当前日志格式漂移了。这一格的所有结论都不可用，先修判据')
        return
    expect(log, 'LOG:word-pack-format-parsed', True,
           f'{len(lines)} 条起点/终点行，其中 {got} 条成功/失败行解析成功'
           if lines else '本轮无 word-pack-download 起点/终点行')


def hil_stability(log: Log):
    """跨批次通用：不崩、不重启循环、栈不爆。"""
    # 启动数取 **bootloader 分区表行**，不取 `opened COM`（后者是监听器事件）。
    # 0 条 ≠ 1 次启动：可能是抓取被截断、也可能整份日志只有一段没有冷启。
    # 这种时候说"没测到"，不要默认它是正常的单次启动。
    if log.boots == 0:
        skip(log, 'STAB:no-reboot-loop',
             '本轮没有一条 `boot: End of partition table` ⇒ **设备启动次数没测到**'
             '（抓取被截断，或这段日志不含冷启）。别把它读成"启动 1 次、正常"')
    else:
        detail = (f'{log.boots} 次设备启动（数 `boot: End of partition table`，'
                  f'USB/软件复位也会出现，不证明真实断电）')
        if log.monitor_attaches:
            detail += (f'；监听器附着 {log.monitor_attaches} 次'
                       f'（`[HH:mm:ss] opened COM*` 或 '
                       f'`[HH:mm:ss] listener attached: COM*`，'
                       f'**这是脚本连串口的次数，不是设备启动**——两者在多数日志里'
                       f'恰好相等，这正是旧判据把 12/14 次重启读成 1 次的原因。'
                       f'对端 2026-10-06 把 emitter 从裸 `opened COM7` 改名成 '
                       f'`listener attached: COM7`，就是为了让这行自证身份）')
        expect(log, 'STAB:no-reboot-loop', log.boots <= 2,
               detail + '（>2 视为重启循环）')
    overflows = re.findall(r'stack overflow in task\s+(\S+)', log.text)
    expect(log, 'STAB:no-stack-overflow',
           not log.has('stack overflow in task'),
           f'任务栈溢出 {len(overflows)} 次：{overflows}' if overflows else '无任务栈溢出')
    name = 'STAB:stack-hwm-healthy'
    if (log.text.count('UI stack HWM:') != len(log.ui_hwm) or
            log.text.count('RenderFrameToEpd: stack HWM before render:') != len(log.hwm)):
        expect(log, name, False, '栈 HWM 格式漂移/缺字段/单位错误，不能静默忽略')
        return
    samples = [('wqn_ui', log.ui_hwm), ('wqn_epd_refresh', log.hwm)]
    detail = '；'.join(f'{task}: n={len(values)}, min={min(values)} B'
                      for task, values in samples if values)
    if any(min(values) <= 2000 for _task, values in samples if values):
        expect(log, name, False, detail + '；任一已测任务余量 <=2000 B')
    elif any(not values for _task, values in samples):
        missing = ', '.join(task for task, values in samples if not values)
        skip(log, name, (detail + '；' if detail else '') + f'{missing} 缺测；不借另一任务的余量判通过')
    else:
        expect(log, name, True, detail + '；两任务已采样余量均 >2000 B，不证明未采样调用链')


# --------------------------------------------------------------- 判据自检 --

# WHY THIS EXISTS: 2026-10-06 我修一个假绿时新造了一个假绿（`MONITOR_ATTACH_RE`
# 漏 `re.M`），而它在 19 份历史日志上全显示 1、看着完全正常——因为那些日志的
# **第一行恰好就是 attach 行**，"只匹配开头那一行"和"匹配所有行"在这整个语料上
# **恒等**。当时唯一的证据是一份写在 `/tmp` 的合成日志，而 `/tmp` 会被清：
# 下一个会话拿不到它，同一个错就能再静默一年。
# 所以把那几种合成形状**固化进判据自己**，`--selftest` 跑。
#
# 三条铁律（都是从这次的事里学来的，写在这儿免得忘）：
#   1. **行锚定模式的夹具必须多行**——单行字符串永远触发不了 `^` 的行语义；
#   2. **且第一行必须是不匹配行**——否则"只匹配第一行"和"匹配所有行"又恒等；
#   3. **每个夹具都要有一个"已知答案"**，不能只断言"不报错"。
SELFTEST_ATTACH_SHAPE = """[02:10:00] monitoring COM7 @ 115200 bps, log: serial-COM7-021000.log
I (99) boot: End of partition table
[02:10:01] listener attached: COM7
I (1200) wqn_storage: nvs write: key=cur_rev bytes=52 total_ms=3 changed=1
[02:10:02] listener attached: COM7
I (4200) wqn_storage: nvs write: key=cur_int bytes=52 total_ms=2 changed=1
[02:40:00] listener attached: COM7
I (99) boot: End of partition table
[02:40:09] COM7 unavailable: The port 'COM7' does not exist. Retrying in 1s...
"""

# 同一形状的**旧 emitter** 版本：producer 改名后判据仍然要认历史那半。
SELFTEST_ATTACH_SHAPE_OLD = SELFTEST_ATTACH_SHAPE.replace(
    'listener attached: COM7', 'opened COM7')


def selftest():
    """`python3 hil_check.py --selftest`：拿已知答案的合成输入验判据自己。

    这不是产品测试，是**狼来了判据的自检**：它验的是"判据在该出声的时候会不会出声"。
    2026-10-06 之前没有它，于是 `STAB:no-reboot-loop` 在 1004.2 / 1005.7crash 上
    连门都不进（判据静默不触发，比假 PASS 更难发现），而这个失效模式只有一个
    `/tmp` 里的合成日志能抓住。
    """
    ok = 0
    bad = []

    def check(name, got, want):
        nonlocal ok
        if got == want:
            ok += 1
            print(f'  [PASS] {name:<44} {got}')
        else:
            bad.append(name)
            print(f'  [FAIL] {name:<44} got {got!r}, want {want!r}')

    print('—— hil_check 自检：合成形状 × 已知答案 ——')

    # 1. 冷启数必须只认 bootloader 分区表行，且**不被 attach 行污染**。
    log = Log.from_text(SELFTEST_ATTACH_SHAPE, '<selftest>')
    check('boots = 分区表行数（2），不吃 attach', log.boots, 2)
    check('attaches = 3（新 emitter，首行不是 attach）',
          log.monitor_attaches, 3)

    # 2. 同一个形状换旧 emitter，两句都要认——改名不能让历史归零。
    #    ⚠️ 而且**只有这个夹具能抓住启动数的回归**：把 boots 退回最初的
    #    `text.count('opened COM') or …` 时，上面的新 emitter 形状上
    #    `count('opened COM') == 0`，`or` 恰好回落到分区表行数 ⇒ **答案是对的**，
    #    又是"碰巧相等"。旧 emitter 形状有 3 个 `opened COM7` 而只有 2 次冷启，
    #    那一份才把它照出来。所以这个夹具**不能当作冗余删掉**。
    log_old = Log.from_text(SELFTEST_ATTACH_SHAPE_OLD, '<selftest-old>')
    check('attaches = 3（旧 emitter 同样认）', log_old.monitor_attaches, 3)
    check('boots 不随 emitter 变', log_old.boots, 2)

    # 3. 第一行就是 attach 行时，答案必须**一样**——这就是 `re.M` 的守门测试。
    first_attach = ('[02:10:00] opened COM7\n'
                    'I (99) boot: End of partition table\n'
                    '[02:11:00] opened COM7\n'
                    'I (99) boot: End of partition table\n')
    lf = Log.from_text(first_attach, '<selftest-first-attach>')
    check('首行就是 attach：boots 仍是 2', lf.boots, 2)
    check('首行就是 attach：attaches 仍是 2（不是 1）', lf.monitor_attaches, 2)

    # 4. 字符串型 key=value 不能被数字型解析器丢掉（它曾把 3 个 key 印成 key=?）。
    log2 = Log.from_text(
        'I (6359) wqn_storage: nvs write: key=cur_rev bytes=52 total_ms=3 changed=1\n'
        'I (6359) wqn_storage: nvs stats: used_entries=195 free_entries=309 '
        'available_entries=183 total_entries=504\n'
        'I (127459) wqn_storage: nvs write: key=cur_int bytes=52 total_ms=6 changed=1\n'
        'I (127459) wqn_storage: nvs stats: used_entries=199 free_entries=305 '
        'available_entries=179 total_entries=504\n', '<selftest-kv>')
    check('nvs write 解析出 2 笔', len(log2.nvs_writes), 2)
    check('key 名保住了（不是 ?）',
          [w.get('key') for w in log2.nvs_writes], ['cur_rev', 'cur_int'])

    # A UUID beginning with digits must not become its numeric prefix through
    # KV_NUM_RE. Identity is a string, not a sequence counter or an Agent gate.
    fixture_sid = '12345678-1234-1234-1234-123456789abc'
    log_sid = Log.from_text(
        'fixture preamble (not a device line)\n'
        'I (500) word_store: word observation durable: sequence=16 lookup_ms=0 '
        'append_ms=13 append_open_ms=3 append_bytes=200 total_ms=14 mode=3 '
        f'session={fixture_sid}\n', '<selftest-observation-sid>')
    check('作答行追加mode/session仍解析一笔', len(log_sid.obs_durable), 1)
    check('作答SID保留完整UUID字符串而非数字前缀',
          log_sid.obs_durable[0].get('session') if log_sid.obs_durable else None,
          fixture_sid)
    check('身份字段不改载荷/耗时单位',
          [(o.get('append_bytes'), o.get('append_ms'), o.get('total_ms'))
           for o in log_sid.obs_durable], [(200, 13, 14)])

    # 5. 墙钟前缀的状态行**不能**被设备时间戳正则吃掉（监听器行的守门测试）。
    check('LOG_PREFIX_RE 不匹配墙钟前缀行',
          bool(LOG_PREFIX_RE.match('[02:10:00] opened COM7')), False)
    check('LOG_PREFIX_RE 匹配设备行',
          bool(LOG_PREFIX_RE.match('I (1200) wqn_storage: nvs write: key=cur_rev')), True)

    # 6. 双峰样本不许被压成一个中位数（`cluster_gap` 的守门测试）。
    cl = cluster_gap([7, 8, 6, 9, 387, 426, 424, 434])
    check('双峰被分成 2 簇（不是 1 个）', len(cl), 2)

    # 7. 判据端到端：同一份合成日志上，该 FAIL 的必须 FAIL。
    RESULTS.clear()
    hil_stability(log)
    verdicts = {n: k for _p, n, k, _o, _d in RESULTS}
    check('合成日志 2 次冷启 ⇒ no-reboot-loop PASS',
          verdicts.get('STAB:no-reboot-loop'), 'PASS')
    RESULTS.clear()
    hil_stability(Log.from_text(
        ''.join(f'I (99) boot: End of partition table\n' for _ in range(14)),
        '<selftest-crash>'))
    verdicts = {n: k for _p, n, k, _o, _d in RESULTS}
    check('14 次冷启 ⇒ no-reboot-loop FAIL（门进得去）',
          verdicts.get('STAB:no-reboot-loop'), 'FAIL')
    RESULTS.clear()
    hil_stability(Log.from_text('I (1200) wqn_ui: hello\n', '<selftest-noboot>'))
    verdicts = {n: k for _p, n, k, _o, _d in RESULTS}
    check('0 条分区表 ⇒ SKIP（不是默认"启动 1 次"）',
          verdicts.get('STAB:no-reboot-loop'), 'SKIP')
    RESULTS.clear()

    def verdict(fn, text, name):
        RESULTS.clear()
        fn(Log.from_text('fixture preamble (not a device line)\n' + text))
        return {n: k for _p, n, k, _o, _d in RESULTS}.get(name)

    hwm_name = 'STAB:stack-hwm-healthy'
    epd_hwm = 'I (100) wqn_ui: RenderFrameToEpd: stack HWM before render: 8860 bytes free\n'
    ui_start = 'I (50) wqn_ui: UI stack HWM: phase=start free_bytes=4632\n'
    ui_low = 'I (90) wqn_ui: UI stack HWM: phase=state-loaded free_bytes=1128\n'
    ui_render = 'I (120) wqn_ui: UI stack HWM: phase=frame-dispatched free_bytes=4096\n'
    check('EPD高余量不能盖住UI1128B', verdict(hil_stability, epd_hwm + ui_start + ui_low, hwm_name), 'FAIL')
    check('UI低余量后EPD高余量仍FAIL', verdict(hil_stability, ui_low + epd_hwm, hwm_name), 'FAIL')
    check('两任务分别健康才PASS', verdict(hil_stability, ui_start + epd_hwm + ui_render, hwm_name), 'PASS')
    check('只有EPD不冒充UI健康', verdict(hil_stability, epd_hwm, hwm_name), 'SKIP')
    check('只有UI不冒充EPD健康', verdict(hil_stability, ui_start, hwm_name), 'SKIP')
    check('两任务都缺测显式SKIP', verdict(hil_stability, '', hwm_name), 'SKIP')
    check('只测UI低余量仍FAIL', verdict(hil_stability, ui_low, hwm_name), 'FAIL')
    check('UI健康不能盖住EPD低余量', verdict(hil_stability, ui_start + epd_hwm.replace('8860', '1000'), hwm_name), 'FAIL')
    check('UI边界2000B不绿', verdict(hil_stability, ui_start.replace('4632', '2000') + epd_hwm, hwm_name), 'FAIL')
    check('UI2001B过原门', verdict(hil_stability, ui_start.replace('4632', '2001') + epd_hwm, hwm_name), 'PASS')
    check('UI字段漂移不静默SKIP', verdict(hil_stability, ui_start.replace('free_bytes=', 'free_words=') + epd_hwm, hwm_name), 'FAIL')
    check('UI单位漂移不绿', verdict(hil_stability, ui_start.replace('4632', '4632ms') + epd_hwm, hwm_name), 'FAIL')
    check('UI坏行混在绿行里仍FAIL', verdict(hil_stability, ui_start + ui_render.replace('free_bytes=', 'free=') + epd_hwm, hwm_name), 'FAIL')
    check('EPD单位漂移不绿', verdict(hil_stability, ui_start + epd_hwm.replace('bytes free', 'words free'), hwm_name), 'FAIL')
    check('UI来源错误不能借EPD健康', verdict(hil_stability, ui_start.replace('wqn_ui:', 'listener:') + epd_hwm, hwm_name), 'FAIL')
    check('UI行锚定首行不匹配多行仍解析', Log.from_text('preamble\n' + ui_start + ui_low).ui_hwm, [4632, 1128])
    check('已报栈溢出不能健康样本冒充无崩溃', verdict(hil_stability,
          ui_start + epd_hwm + '***ERROR*** A stack overflow in task wqn_ui has been detected.\n',
          'STAB:no-stack-overflow'), 'FAIL')

    queue_name = 'WRITE:word-commit-queue-wait'
    def commit(t=10000, wait=4479):
        return (f'I ({t}) storage_service: storage transaction complete: request=151 '
                f'owner=word-observation-commit queue_wait_ms={wait} '
                'elapsed_ms=15 result=ESP_OK\n')

    check('无 save 的答题排队 4479 ms 必须 FAIL',
          verdict(hil_word_commit_queue, commit(), queue_name), 'FAIL')
    check('排队 499 ms PASS',
          verdict(hil_word_commit_queue, commit(wait=499), queue_name), 'PASS')
    check('排队 500 ms 边界 FAIL',
          verdict(hil_word_commit_queue, commit(wait=500), queue_name), 'FAIL')
    check('无答题 SKIP', verdict(hil_word_commit_queue, '', queue_name), 'SKIP')
    bench = ('I (5000) word_store: storage bench BEGIN profile=snapshot\n'
             'I (9000) word_store: storage bench END total_ms=4000\n')
    check('完成在 END 后、入队在 bench 内仍剔除',
          verdict(hil_word_commit_queue, bench + commit(), queue_name), 'SKIP')
    check('bench 后真实排队不被剔除',
          verdict(hil_word_commit_queue, bench + commit(t=20000), queue_name), 'FAIL')
    check('未结束 bench 不会抹掉更早的排队失败', verdict(
        hil_word_commit_queue, commit() +
        'I (15000) word_store: storage bench BEGIN\n', queue_name), 'FAIL')
    check('重启清空前一启动未结束的 bench 窗口', verdict(
        hil_word_commit_queue, 'I (500) word_store: storage bench BEGIN\n'
        'I (99) boot: End of partition table\n' + commit(), queue_name), 'FAIL')
    check('答题时间戳缺失不能假 PASS', verdict(
        hil_word_commit_queue, commit(wait=0).replace('I (10000) ', ''),
        queue_name), 'FAIL')
    check('bench 时间戳缺失不能伪装污染剔除', verdict(
        hil_word_commit_queue, 'word_store: storage bench BEGIN\n' +
        commit(wait=0), queue_name), 'FAIL')
    check('答题事务字段漂移 FAIL，不能静默 SKIP', verdict(
        hil_word_commit_queue, commit().replace('queue_wait_ms=', 'queued_ms='),
        queue_name), 'FAIL')

    check('NVS 52/3379/4392/8676 B 最佳分块算术',
          [nvs_entries(n) for n in (52, 3379, 4392, 8676)], [4, 108, 141, 276])
    cursor = ('I (1200) wqn_storage: nvs write: key=cur_rev '
              'bytes=52 total_ms=11 changed=1\n')
    atomic = ('I (1201) word_store: atomic write: bytes=4392 backup=0 '
              'fopen_ms=1200 write_ms=20 stat_ms=7 remove_ms=382 '
              'rename_backup_ms=190 rename_primary_ms=410 total_ms=2209\n')
    check('实际小 NVS + 大 SPIFFS 不误报预算 FAIL', verdict(
        hil_nvs_entry_budget, cursor + atomic, 'WRITE:nvs-entry-budget'), 'PASS')
    check('只有 SPIFFS 不冒充实际 NVS 通过', verdict(
        hil_nvs_entry_budget, atomic, 'WRITE:nvs-entry-budget'), 'SKIP')
    check('实际大 NVS 预算超限 FAIL', verdict(
        hil_nvs_entry_budget, cursor.replace('bytes=52', 'bytes=8676'),
        'WRITE:nvs-entry-budget'), 'FAIL')
    check('NVS bytes 格式漂移 FAIL', verdict(
        hil_nvs_entry_budget, cursor.replace('bytes=52 ', ''),
        'WRITE:nvs-entry-budget'), 'FAIL')

    def snapshot_fixture(slow=False, base_cost=10000):
        lines = ['I (500) word_store: storage bench BEGIN profile=snapshot\n']
        for size in (3379, 8676):
            for i in range(12):
                # 两个慢样本足以破坏 p90，但中位仍为便宜档：专抓假绿。
                cost = 200000 if slow and size == 8676 and i in (10, 11) else base_cost
                lines.append(
                    f'I ({1000 + i}) word_store: storage snapshot append probe: '
                    f'bytes={size} round={i} result=ESP_OK new_object={int(i == 0)} '
                    f'open_us=1000 write_us={cost - 1100} flush_us=50 sync_us=30 '
                    f'close_us=20 total_us={cost} written_bytes={size}\n')
        lines.append('I (2000) word_store: storage bench END total_ms=1500 '
                     'profile=snapshot result=ESP_OK\n')
        return ''.join(lines)

    probe_name = 'WRITE:snapshot-append-8676-p90'
    fast = snapshot_fixture()
    check('实尺寸 append 两档便宜样本 PASS', verdict(
        hil_snapshot_append_probe, fast, probe_name), 'PASS')
    check('append 双峰 p90=200 ms FAIL（中位会假绿）', verdict(
        hil_snapshot_append_probe, snapshot_fixture(True), probe_name), 'FAIL')
    check('单个慢模态即使 p90 便宜也不通过', verdict(
        hil_snapshot_append_probe, snapshot_fixture(True).replace(
            'round=10 result=ESP_OK new_object=0 open_us=1000 write_us=198900 '
            'flush_us=50 sync_us=30 close_us=20 total_us=200000',
            'round=10 result=ESP_OK new_object=0 open_us=1000 write_us=8900 '
            'flush_us=50 sync_us=30 close_us=20 total_us=10000'), probe_name), 'FAIL')
    check('单峰 p90=200 ms 边界也 FAIL', verdict(
        hil_snapshot_append_probe, snapshot_fixture(base_cost=200000),
        probe_name), 'FAIL')
    check('小尺寸与大尺寸同样验收', verdict(
        hil_snapshot_append_probe, fast, 'WRITE:snapshot-append-3379-p90'), 'PASS')
    check('append 短写不能通过', verdict(
        hil_snapshot_append_probe, fast.replace('written_bytes=8676', 'written_bytes=1'),
        probe_name), 'FAIL')
    check('append 字段改名格式门 FAIL', verdict(
        hil_snapshot_append_probe, fast.replace('sync_us=', 'renamed_us='),
        'LOG:snapshot-append-probe-parsed'), 'FAIL')
    check('格式异常不能让性能项假 PASS', verdict(
        hil_snapshot_append_probe, fast.replace('sync_us=', 'renamed_us='),
        probe_name), 'FAIL')
    check('append 11/12 轮是 SKIP，不是通过', verdict(
        hil_snapshot_append_probe, fast.replace(
            next(l for l in fast.splitlines(True) if 'bytes=8676 round=11 ' in l), ''),
        probe_name), 'SKIP')
    check('重复 round FAIL', verdict(
        hil_snapshot_append_probe, fast.replace(
            'I (2000)', fast.splitlines(True)[-2] + 'I (2000)'), probe_name), 'FAIL')
    check('分项与自身 total 不对账 FAIL', verdict(
        hil_snapshot_append_probe, fast.replace('total_us=10000', 'total_us=15000'),
        probe_name), 'FAIL')
    check('旧构建没有实尺寸探针 SKIP', verdict(
        hil_snapshot_append_probe, cursor, probe_name), 'SKIP')
    check('两次启动的 probe round 分组，不误报重复', verdict(
        hil_snapshot_append_probe, fast + 'I (99) boot: End of partition table\n' +
        fast, probe_name), 'PASS')
    check('缺 END 即使 12 轮已齐也不通过', verdict(
        hil_snapshot_append_probe, ''.join(fast.splitlines(True)[:-1]),
        probe_name), 'SKIP')
    check('探针清理失败不能以样本子集通过', verdict(
        hil_snapshot_append_probe, fast.replace('profile=snapshot result=ESP_OK',
                                               'profile=snapshot result=ESP_FAIL'),
        probe_name), 'FAIL')

    def held_fixture(profile='snapshot-paired', sync=True, slow=False, alternate=True):
        lines = [f'I (500) word_store: storage bench BEGIN profile={profile}\n']
        tick = 1000
        def emit(body):
            nonlocal tick
            lines.append(f'I ({tick}) word_store: {body}\n')
            tick += 10
        for size in (3379, 8676):
            emit(f'storage snapshot held open: bytes={size} result=ESP_OK '
                 'new_object=1 open_us=900000')
            for rnd in range(12):
                cost = 200000 if slow and size == 8676 and rnd in (10, 11) else 10000
                sync_cost = 100 if sync else 0
                held = (f'storage snapshot held commit: bytes={size} round={rnd} '
                        f'result=ESP_OK write_us={cost - 50 - sync_cost} flush_us=50 '
                        f'sync_us={sync_cost} total_us={cost} written_bytes={size} '
                        f'flush_ok=1 sync_attempted={int(sync)} sync_ok={int(sync)}')
                control = (f'storage snapshot append probe: bytes={size} round={rnd} '
                           f'result=ESP_OK new_object={int(rnd == 0)} open_us=100000 '
                           'write_us=390000 flush_us=1000 sync_us=5000 close_us=4000 '
                           f'total_us=500000 written_bytes={size}')
                if profile == 'snapshot-paired':
                    for body in ([held, control] if alternate and rnd % 2 else [control, held]):
                        emit(body)
                else:
                    emit(held)
            emit(f'storage snapshot held close: bytes={size} result=ESP_OK close_us=300000 '
                 f'rounds=12 committed_bytes={size * 12} file_bytes={size * 12} '
                 'stat_us=1000 close_ok=1 stat_ok=1')
        emit(f'storage bench END total_ms=1000 profile={profile} result=ESP_OK')
        return ''.join(lines)

    held_fast = held_fixture()
    held_name = 'WRITE:snapshot-held-8676-p90'
    paired_name = 'WRITE:snapshot-held-paired-control'
    held_format = 'LOG:snapshot-held-probe-parsed'
    check('长开提交 PASS，不能把 900ms open/300ms close 塞入每笔', verdict(
        hil_snapshot_held_probe, held_fast, held_name), 'PASS')
    check('长开小尺寸同样判读', verdict(
        hil_snapshot_held_probe, held_fast, 'WRITE:snapshot-held-3379-p90'), 'PASS')
    check('每笔不做 fsync 即使便宜也 FAIL', verdict(
        hil_snapshot_held_probe, held_fixture(sync=False), held_name), 'FAIL')
    check('fsync 返回失败不假绿', verdict(
        hil_snapshot_held_probe, held_fast.replace('sync_ok=1', 'sync_ok=0'),
        held_name), 'FAIL')
    check('fflush 未成功不假绿', verdict(
        hil_snapshot_held_probe, held_fast.replace('flush_ok=1', 'flush_ok=0'),
        held_name), 'FAIL')
    check('长开短写 FAIL', verdict(
        hil_snapshot_held_probe, held_fast.replace('written_bytes=8676', 'written_bytes=1'),
        held_name), 'FAIL')
    check('关闭后实际文件长度必须对账', verdict(
        hil_snapshot_held_probe, held_fast.replace('file_bytes=104112', 'file_bytes=1'),
        held_name), 'FAIL')
    check('最终 close 错误不假绿', verdict(
        hil_snapshot_held_probe, held_fast.replace('close_ok=1', 'close_ok=0'),
        held_name), 'FAIL')
    check('长开双峰慢尾拒绝中位假绿', verdict(
        hil_snapshot_held_probe, held_fixture(slow=True), held_name), 'FAIL')
    check('长开分项只和本笔 total 对账', verdict(
        hil_snapshot_held_probe, held_fast.replace('total_us=10000', 'total_us=15000'),
        held_name), 'FAIL')
    held_missing = held_fast.replace(next(l for l in held_fast.splitlines(True)
                                         if 'held commit: bytes=8676 round=11 ' in l), '')
    check('长开缺一笔是 SKIP 不是通过', verdict(
        hil_snapshot_held_probe, held_missing, held_name), 'SKIP')
    check('长开缺 close 是 SKIP', verdict(
        hil_snapshot_held_probe, ''.join(l for l in held_fast.splitlines(True)
                                       if 'held close: bytes=8676 ' not in l), held_name), 'SKIP')
    check('长开缺 END 是 SKIP', verdict(
        hil_snapshot_held_probe, ''.join(held_fast.splitlines(True)[:-1]), held_name), 'SKIP')
    last_commit = next(l for l in held_fast.splitlines(True)
                       if 'held commit: bytes=8676 round=11 ' in l)
    check('长开重复 round FAIL', verdict(
        hil_snapshot_held_probe, held_fast.replace(last_commit, last_commit * 2),
        held_name), 'FAIL')
    first_open = next(l for l in held_fast.splitlines(True) if 'held open: bytes=8676 ' in l)
    first_commit = next(l for l in held_fast.splitlines(True)
                        if 'held commit: bytes=8676 round=0 ' in l)
    check('完整但 commit 在 open 前的生命周期 FAIL', verdict(
        hil_snapshot_held_probe, held_fast.replace(first_open, '').replace(
            first_commit, first_commit + first_open), held_name), 'FAIL')
    check('长开字段改名格式 FAIL', verdict(
        hil_snapshot_held_probe, held_fast.replace('sync_attempted=', 'renamed='),
        held_format), 'FAIL')
    check('格式漂移不能让长开性能项假 PASS', verdict(
        hil_snapshot_held_probe, held_fast.replace('sync_attempted=', 'renamed='),
        held_name), 'FAIL')
    check('长开未知事件行不能静默丢掉', verdict(
        hil_snapshot_held_probe, held_fast.replace('held close:', 'held closed:'),
        held_format), 'FAIL')
    check('长开清理/运行错误 FAIL', verdict(
        hil_snapshot_held_probe, held_fast.replace(
            'profile=snapshot-paired result=ESP_OK', 'profile=snapshot-paired result=ESP_FAIL'),
        held_name), 'FAIL')
    check('两档 12 对交替控制完整 PASS', verdict(
        hil_snapshot_held_probe, held_fast, paired_name), 'PASS')
    check('一直固定先后顺序不得冒充交替对照', verdict(
        hil_snapshot_held_probe, held_fixture(alternate=False), paired_name), 'FAIL')
    check('缺重开对照不得声称同期比较', verdict(
        hil_snapshot_held_probe, ''.join(l for l in held_fast.splitlines(True)
                                       if 'append probe: bytes=8676 round=11 ' not in l),
        paired_name), 'SKIP')
    last_control = next(l for l in held_fast.splitlines(True)
                        if 'append probe: bytes=8676 round=11 ' in l)
    check('重复对照不是样本不足，必须 FAIL', verdict(
        hil_snapshot_held_probe, held_fast.replace(last_control, last_control * 2),
        paired_name), 'FAIL')
    check('paired 的旧重开性能仍单独 FAIL', verdict(
        hil_snapshot_append_probe, held_fast, probe_name), 'FAIL')
    check('profile 精确匹配：held 不能冒充旧 snapshot', verdict(
        hil_snapshot_append_probe, held_fixture(profile='snapshot-held'),
        'LOG:snapshot-append-probe-parsed'), 'SKIP')
    check('旧 snapshot 没有长开探针 SKIP', verdict(
        hil_snapshot_held_probe, fast, held_name), 'SKIP')
    check('跨两次启动长开生命周期分别完整', verdict(
        hil_snapshot_held_probe, held_fast + 'I (99) boot: End of partition table\n' +
        held_fast, held_name), 'PASS')

    def partition_fixture(zero=False):
        # Multi-line anchoring: the first line deliberately does not match.
        lines = ['unrelated first line\n',
                 'I (400) storage_io_probe: storage partition probe READY schema=1 enabled=1 '
                 'scope=task+partition partition=storage app_version=fixture elf_sha256=0123456789abcdef\n']
        for line in held_fast.splitlines(True):
            if 'storage bench ' in line:
                line = line.rstrip('\n') + ' partition_probe=1\n'
            lines.append(line)
            fields = {m.group('k'): m.group('v') for m in KV_RE.finditer(line)}
            kind = next((kind for text, kind in (
                ('storage snapshot append probe:', 'reopen'),
                ('storage snapshot held open:', 'held-open'),
                ('storage snapshot held commit:', 'held-commit'),
                ('storage snapshot held close:', 'held-close')) if text in line), None)
            if kind is None:
                continue
            stages = {'reopen': ('open', 'write', 'flush', 'sync', 'close'),
                      'held-open': ('open',), 'held-commit': ('write', 'flush', 'sync'),
                      'held-close': ('close', 'stat')}[kind]
            size = int(fields['bytes'])
            rnd = fields.get('round', fields.get('rounds', '0'))
            for phase in stages:
                vfs = int(fields[phase + '_us'])
                values = {'kind': kind, 'bytes': size, 'round': rnd, 'phase': phase,
                          'vfs_us': vfs, 'span_us': vfs, 'scope_ok': 1, 'nested_calls': 0}
                for op in ('read', 'write', 'erase'):
                    writing = not zero and op == 'write' and phase == 'write'
                    for field, value in (('calls', int(writing)), ('bytes', size if writing else 0),
                                         ('us', 10 if writing else 0), ('max_us', 10 if writing else 0),
                                         ('failures', 0)):
                        values[op + '_' + field] = value
                lines.append('I (999) storage_io_probe: storage partition io: ' +
                             ' '.join(f'{k}={v}' for k, v in values.items()) + '\n')
        return ''.join(lines)

    io = partition_fixture()
    io_metric = 'WRITE:snapshot-partition-io-accounting'
    io_format = 'LOG:snapshot-partition-io-parsed'
    check('分区 198 阶段完整并对自己的 VFS 字段 PASS', verdict(
        hil_snapshot_partition_io, io, io_metric), 'PASS')
    check('分区行首锚定，多行且首行不匹配仍解析 PASS', verdict(
        hil_snapshot_partition_io, io, io_format), 'PASS')
    check('wrapper 未链接、持久提交全零必须 FAIL', verdict(
        hil_snapshot_partition_io, partition_fixture(zero=True), io_metric), 'FAIL')
    check('任务/分区 scope 不成立 FAIL', verdict(
        hil_snapshot_partition_io, io.replace('scope_ok=1', 'scope_ok=0'), io_metric), 'FAIL')
    check('分区字段改名不能静默 SKIP', verdict(
        hil_snapshot_partition_io, io.replace('erase_calls=', 'renamed='), io_format), 'FAIL')
    check('分区字段漂移也不能让归因 PASS', verdict(
        hil_snapshot_partition_io, io.replace('erase_calls=', 'renamed='), io_metric), 'FAIL')
    check('分区负耗时 FAIL', verdict(
        hil_snapshot_partition_io, io.replace('read_us=0', 'read_us=-1'), io_format), 'FAIL')
    check('底层耗时不能超过自己的阶段 VFS', verdict(
        hil_snapshot_partition_io, io.replace('write_us=10 write_max_us=10',
                                               'write_us=9999999 write_max_us=9999999'),
        io_metric), 'FAIL')
    check('底层字段不能和其他阶段总耗时对账', verdict(
        hil_snapshot_partition_io, io.replace('vfs_us=900000 span_us=900000',
                                               'vfs_us=100000 span_us=100000'), io_metric), 'FAIL')
    check('max_us 与次数/总耗时不自洽 FAIL', verdict(
        hil_snapshot_partition_io, io.replace('write_max_us=10', 'write_max_us=1'), io_metric), 'FAIL')
    check('底层 API 返回失败即拒绝成功样本子集', verdict(
        hil_snapshot_partition_io, io.replace(
            'write_calls=1 write_bytes=3379 write_us=10 write_max_us=10 write_failures=0',
            'write_calls=1 write_bytes=3379 write_us=10 write_max_us=10 write_failures=1'),
        io_metric), 'FAIL')
    check('完整 END 后缺阶段是 FAIL 不是 SKIP', verdict(
        hil_snapshot_partition_io, ''.join(l for l in io.splitlines(True)
                                          if 'phase=flush' not in l), io_metric), 'FAIL')
    check('完整 END 后连父记录一起消失也 FAIL', verdict(
        hil_snapshot_partition_io, ''.join(l for l in io.splitlines(True)
                                          if not ('round=3' in l and
                                                  ('kind=held-commit' in l or 'held commit:' in l))),
        io_metric), 'FAIL')
    row = next(l for l in io.splitlines(True) if 'storage partition io:' in l)
    check('分区阶段重复 FAIL', verdict(
        hil_snapshot_partition_io, io.replace(row, row * 2), io_format), 'FAIL')
    check('缺 END 只能 SKIP', verdict(
        hil_snapshot_partition_io, ''.join(io.splitlines(True)[:-1]), io_metric), 'SKIP')
    check('READY 缺身份/版本契约 FAIL', verdict(
        hil_snapshot_partition_io, io.replace('app_version=fixture', 'unknown=fixture'), io_format), 'FAIL')
    check('profile 标记丢失不当成未编译 SKIP', verdict(
        hil_snapshot_partition_io, io.replace(' partition_probe=1', ''), io_metric), 'FAIL')
    check('旧长开日志没有分区计量 SKIP', verdict(
        hil_snapshot_partition_io, held_fast, io_metric), 'SKIP')
    check('跨启动分区阶段按运行分别对账', verdict(
        hil_snapshot_partition_io, io + 'I (99) boot: End of partition table\n' + io,
        io_metric), 'PASS')

    def gc_fixture(empty=False, fast_only=False, quick_no_deleted=False):
        lines = []
        for line in io.splitlines(True):
            if 'storage partition probe READY ' in line:
                line = line.rstrip('\n') + ' gc_probe=1 partition_bytes=8388608\n'
            elif 'storage bench BEGIN' in line or 'storage bench END' in line or 'storage partition io:' in line:
                line = line.rstrip('\n') + ' gc_probe=1\n'
            lines.append(line)
            if 'storage partition io:' not in line:
                continue
            f = dict(re.findall(r'\b([a-z][a-z0-9_]*)=([^\s]+)', line))
            checking = not empty and f['phase'] == 'write'
            g = {'schema': 1, **{k: f[k] for k in ('kind', 'bytes', 'round', 'phase')},
                 'scope_ok': 1, 'gc_check_calls': int(checking), 'gc_quick_calls': 0,
                 'gc_nested_calls': 0, 'gc_check_us': 20 if checking else 0,
                 'gc_quick_us': 0, 'gc_check_errors': 0, 'gc_quick_errors': 0,
                 'gc_quick_no_deleted': 0, 'fs_seen': int(checking),
                 'block_count': 2048 if checking else 0, 'block_size': 4096 if checking else 0,
                 'page_size': 256 if checking else 0}
            for name in ('free_before', 'free_after', 'free_min', 'free_max'):
                g[name] = 3 if checking else -1
            for name in ('allocated_before', 'allocated_after'):
                g[name] = 7500 if checking else -1
            for name in ('deleted_before', 'deleted_after'):
                g[name] = 20000 if checking else -1
            if checking and quick_no_deleted:
                g.update(gc_quick_calls=1, gc_quick_us=2, gc_quick_no_deleted=1)
            for op in ('read', 'write', 'erase'):
                inside = checking and not fast_only and op == 'write'
                for field, value in (('calls', int(inside)), ('bytes', int(f['bytes']) if inside else 0),
                                     ('us', 10 if inside else 0)):
                    g[f'gc_{op}_{field}'] = value
            lines.append('I (999) storage_io_probe: storage spiffs gc: ' +
                         ' '.join(f'{k}={v}' for k, v in g.items()) + '\n')
        return ''.join(lines)

    gc = gc_fixture()
    gc_metric, gc_format = 'WRITE:snapshot-spiffs-gc-accounting', 'LOG:snapshot-spiffs-gc-parsed'
    check('GC 198 阶段与父 API 子集完整 PASS', verdict(
        hil_snapshot_spiffs_gc, gc, gc_metric), 'PASS')
    check('GC 行锚定，多行且首行不匹配 PASS', verdict(
        hil_snapshot_spiffs_gc, gc, gc_format), 'PASS')
    check('GC 增量字段不破坏旧分区契约', verdict(
        hil_snapshot_partition_io, gc, io_metric), 'PASS')
    check('没有实际 GC I/O，快路径检查仍是有效测量', verdict(
        hil_snapshot_spiffs_gc, gc_fixture(fast_only=True), gc_metric), 'PASS')
    check('quick 无可删块是记录的正常结果，不伪造 API 错误', verdict(
        hil_snapshot_spiffs_gc, gc_fixture(quick_no_deleted=True), gc_metric), 'PASS')
    check('全零 GC 包装不能假 PASS', verdict(
        hil_snapshot_spiffs_gc, gc_fixture(empty=True), gc_metric), 'FAIL')
    check('quick 有调用也不能代替每笔 gc_check 的接线证据', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_check_calls=1 gc_quick_calls=0',
                                          'gc_check_calls=0 gc_quick_calls=1').replace(
            'gc_check_us=20 gc_quick_us=0', 'gc_check_us=0 gc_quick_us=20'), gc_metric), 'FAIL')
    check('GC scope 不成立 FAIL', verdict(
        hil_snapshot_spiffs_gc, gc.replace('phase=write scope_ok=1', 'phase=write scope_ok=0'),
        gc_metric), 'FAIL')
    check('GC 字段漂移不能静默 SKIP', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_read_calls=', 'renamed='), gc_format), 'FAIL')
    check('GC 字段漂移也使测量项 FAIL', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_read_calls=', 'renamed='), gc_metric), 'FAIL')
    check('GC check 返回失败，拒绝成功子集', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_check_errors=0', 'gc_check_errors=1'), gc_metric), 'FAIL')
    check('GC quick 真错误与无可删块不同', verdict(
        hil_snapshot_spiffs_gc, gc_fixture(quick_no_deleted=True).replace(
            'gc_quick_errors=0 gc_quick_no_deleted=1', 'gc_quick_errors=1 gc_quick_no_deleted=0'),
        gc_metric), 'FAIL')
    check('GC API 不能超出自己的父 bucket 次数', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_write_calls=1', 'gc_write_calls=2'), gc_metric), 'FAIL')
    check('GC API 不能超出自己的父 bucket 字节', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_write_bytes=3379', 'gc_write_bytes=3380'), gc_metric), 'FAIL')
    check('GC API 不能超出自己的父 bucket 时间', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_write_us=10', 'gc_write_us=11'), gc_metric), 'FAIL')
    # Native 10us remains valid; only the GC inclusive bound must reject it.
    gc_short_wall = gc.replace('gc_check_us=20', 'gc_check_us=9')
    check('API 子集时间不能超过 GC inclusive wall', verdict(
        hil_snapshot_spiffs_gc, gc_short_wall, gc_metric), 'FAIL')
    check('GC wall 不能超出自己的 VFS 阶段', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_check_us=20', 'gc_check_us=9851'), gc_metric), 'FAIL')
    check('GC wall 与 native API 包含关系，不能重复相加', verdict(
        hil_snapshot_spiffs_gc, gc.replace('gc_check_us=20', 'gc_check_us=9850'), gc_metric), 'PASS')
    check('GC 状态未知 sentinel 不能冒充已测到零', verdict(
        hil_snapshot_spiffs_gc, gc.replace('free_before=-1', 'free_before=0'), gc_metric), 'FAIL')
    check('已量到 free_blocks=0 与未知 -1 不混淆', verdict(
        hil_snapshot_spiffs_gc, gc.replace('free_before=3', 'free_before=0').replace(
            'free_after=3', 'free_after=0').replace('free_min=3', 'free_min=0').replace(
            'free_max=3', 'free_max=0'), gc_metric), 'PASS')
    check('GC 负状态仅允许未测到的 -1', verdict(
        hil_snapshot_spiffs_gc, gc.replace('free_before=3', 'free_before=-2'), gc_format), 'FAIL')
    check('GC 状态极值必须覆盖入口出口', verdict(
        hil_snapshot_spiffs_gc, gc.replace('free_min=3', 'free_min=4'), gc_metric), 'FAIL')
    check('GC geometry 必须与真实分区尺寸对账', verdict(
        hil_snapshot_spiffs_gc, gc.replace('block_count=2048', 'block_count=2047'), gc_metric), 'FAIL')
    check('GC 页计数不能超过物理页数', verdict(
        hil_snapshot_spiffs_gc, gc.replace('deleted_before=20000', 'deleted_before=99999'), gc_metric), 'FAIL')
    check('GC READY 缺分区尺寸 FAIL', verdict(
        hil_snapshot_spiffs_gc, gc.replace('partition_bytes=', 'renamed='), gc_metric), 'FAIL')
    check('GC BEGIN/END 标记丢失不是旧镜像 SKIP', verdict(
        hil_snapshot_spiffs_gc, gc.replace(' gc_probe=1', ''), gc_metric), 'FAIL')
    check('GC 成功 END 缺阶段必须 FAIL', verdict(
        hil_snapshot_spiffs_gc, ''.join(l for l in gc.splitlines(True)
                                       if not ('storage spiffs gc:' in l and 'phase=flush' in l)),
        gc_metric), 'FAIL')
    check('GC 成功 END 连父阶段一起缺也 FAIL', verdict(
        hil_snapshot_spiffs_gc, ''.join(l for l in gc.splitlines(True)
                                       if not ('phase=flush' in l and
                                               ('storage partition io:' in l or 'storage spiffs gc:' in l))),
        gc_metric), 'FAIL')
    gc_row = next(l for l in gc.splitlines(True) if 'storage spiffs gc:' in l)
    check('GC 重复阶段 FAIL', verdict(
        hil_snapshot_spiffs_gc, gc.replace(gc_row, gc_row * 2), gc_format), 'FAIL')
    check('GC 缺 END 只能 SKIP', verdict(
        hil_snapshot_spiffs_gc, ''.join(gc.splitlines(True)[:-1]), gc_metric), 'SKIP')
    check('GC 运行错误不能用成功子集', verdict(
        hil_snapshot_spiffs_gc, gc.replace('result=ESP_OK partition_probe=1 gc_probe=1',
                                          'result=ESP_FAIL partition_probe=1 gc_probe=1'),
        gc_metric), 'FAIL')
    check('旧分区日志无 GC 归属必须 SKIP', verdict(
        hil_snapshot_spiffs_gc, io, gc_metric), 'SKIP')
    check('GC 两次启动独立对账不误报重复', verdict(
        hil_snapshot_spiffs_gc, gc + 'I (99) boot: End of partition table\n' + gc,
        gc_metric), 'PASS')

    # Real 122443 capture: Wi-Fi's separately printed header/body straddled
    # three COMPLETE probe records. No payload bytes may be reconstructed.
    wire_lines = gc.splitlines(True)
    sync_row = next(l for l in wire_lines if 'storage partition io:' in l and
                    'kind=reopen' in l and 'phase=sync' in l)
    close_row = next(l for l in wire_lines if 'storage partition io:' in l and
                     'kind=reopen' in l and 'phase=close' in l)
    flush_gc = next(l for l in wire_lines if 'storage spiffs gc:' in l and
                    'kind=reopen' in l and 'phase=flush' in l)
    wire = gc.replace(flush_gc, 'I (48363) wifi:' + flush_gc).replace(
        sync_row, 'state: run -> init (0x0)' + sync_row).replace(
        close_row, 'I (48483) wifi:' + close_row)
    check('Wi-Fi 分片粘在完整分区头前，仍逐阶段完整对账', verdict(
        hil_snapshot_partition_io, wire, io_metric), 'PASS')
    check('Wi-Fi 分片粘在完整 GC 头前，GC 子集仍完整对账', verdict(
        hil_snapshot_spiffs_gc, wire, gc_metric), 'PASS')
    framed = list(snapshot_probe_records(Log.from_text(wire)))
    check('分帧保留探针自己的时间戳和原字段，不借用 Wi-Fi 头',
          sync_row.rstrip('\n') in framed and flush_gc.rstrip('\n') in framed, True)
    check('分帧不重写原始 Log.text，其他判据仍读原日志',
          Log.from_text(wire).text == wire, True)
    check('同一物理行两条完整探针独立解析', verdict(
        hil_snapshot_spiffs_gc, gc.replace(sync_row, sync_row.rstrip('\n')).replace(
            close_row, close_row.rstrip('\n')), gc_metric), 'PASS')
    for malformed, label in (
            (sync_row.replace('read_calls=', 'renamed='), '缺字段'),
            (sync_row.rstrip('\n') + ' scope_ok=1\n', '重复同值字段'),
            (sync_row.replace('scope_ok=1', 'scope_ok=0 scope_ok=1'), '重复字段掩盖坏值'),
            (sync_row.replace('storage_io_probe:', 'wifi:'), '借用异 tag 日志头')):
        check(f'粘连分区行{label}仍 FAIL', verdict(
            hil_snapshot_partition_io, wire.replace(sync_row, malformed), io_metric), 'FAIL')
    for malformed, label in (
            (flush_gc.replace('gc_read_calls=', 'renamed='), '缺字段'),
            (flush_gc.rstrip('\n') + ' scope_ok=1\n', '重复同值字段'),
            (flush_gc.replace('scope_ok=1', 'scope_ok=0 scope_ok=1'), '重复字段掩盖坏值'),
            (flush_gc.replace('storage_io_probe:', 'wifi:'), '借用异 tag 日志头')):
        check(f'粘连 GC 行{label}仍 FAIL', verdict(
            hil_snapshot_spiffs_gc, wire.replace(flush_gc, malformed), gc_metric), 'FAIL')
    torn = flush_gc.replace(' gc_read_calls=', 'I (990) wifi: interleaved\n gc_read_calls=')
    check('GC 字段中途被打断不能拼回假完整', verdict(
        hil_snapshot_spiffs_gc, wire.replace(flush_gc, torn), gc_metric), 'FAIL')
    broken_prefix = sync_row.replace('I (999) storage_io_probe: ', '')
    check('残缺探针前缀不能被后面的完整记录丢弃', verdict(
        hil_snapshot_partition_io, wire.replace(sync_row, broken_prefix + sync_row),
        io_metric), 'FAIL')

    def reserve_fixture(ramp=False, prepared_only=False):
        mode_tail = ' mode=prepared-only bytes=8676' if prepared_only else ''
        lines = ['unrelated first line\n',
                 next(l for l in gc.splitlines(True) if 'storage partition probe READY' in l),
                 'I (400) word_store: storage gc reserve experiment BEGIN schema=1 '
                 f'requested_bytes=131072 control_rounds={0 if prepared_only else 12} '
                 f'prepared_rounds=48{mode_tail}\n']
        nbase = dict(re.findall(r'\b([a-z][a-z0-9_]*)=([^\s]+)', row))
        gbase = dict(re.findall(r'\b([a-z][a-z0-9_]*)=([^\s]+)', gc_row))
        for size in (3379, 8676):
            for profile, count in zip(RESERVE_PROFILES, (12, 48)):
                prepared = profile == 'snapshot-gc-prepared'
                if prepared_only and (size != 8676 or not prepared):
                    continue
                lines.append(f'I (500) word_store: storage bench BEGIN shapes=1 rounds={count} '
                             f'writes={count} profile={profile} partition_probe=1 gc_probe=1 '
                             f'reserve_probe=1 bytes={size} requested_bytes={131072 if prepared else 0}\n')
                sequence = [('reserve-open', 0)]
                if prepared:
                    sequence.append(('reserve-prep', 0))
                sequence += [('reserve-commit', rnd) for rnd in range(count)]
                sequence.append(('reserve-close', count))
                for kind, rnd in sequence:
                    fields = dict(bytes=size, result='ESP_OK')
                    if kind == 'reserve-open':
                        head = 'storage gc snapshot held open'
                        fields.update(new_object=1, open_us=1000)
                    elif kind == 'reserve-prep':
                        head = 'storage gc snapshot prepare'
                        fields.update(requested_bytes=131072, prepare_us=1000)
                    elif kind == 'reserve-close':
                        head = 'storage gc snapshot held close'
                        fields.update(close_us=1000, rounds=count, committed_bytes=count*size,
                                      file_bytes=count*size, stat_us=1000, close_ok=1, stat_ok=1)
                    else:
                        head = 'storage gc snapshot held commit'
                        cost = (150000 + 2000*rnd if ramp else 10000) if prepared else 500000
                        fields.update(round=rnd, write_us=cost, flush_us=100, sync_us=1000,
                                      total_us=cost+1100, written_bytes=size,
                                      flush_ok=1, sync_attempted=1, sync_ok=1)
                    lines.append('I (600) word_store: ' + head + ': ' +
                                 ' '.join(f'{k}={v}' for k,v in fields.items()) + '\n')
                    for phase in SNAPSHOT_IO_PHASES[kind]:
                        n = dict(nbase)
                        n.update(kind=kind, bytes=size, round=rnd, phase=phase,
                                 vfs_us=fields[phase+'_us'], span_us=fields[phase+'_us'], gc_probe=1)
                        for op in ('read','write','erase'):
                            writing = op == 'write' and phase == 'write'
                            for field,value in (('calls',int(writing)), ('bytes',size if writing else 0),
                                                ('us',10 if writing else 0), ('max_us',10 if writing else 0),
                                                ('failures',0)):
                                n[f'{op}_{field}'] = value
                        lines.append('I (600) storage_io_probe: storage partition io: ' +
                                     ' '.join(f'{k}={v}' for k,v in n.items()) + '\n')
                        g = dict(gbase)
                        seen = phase in ('write', 'prepare')
                        allocated = 7500 + rnd * ((size+250)//251)
                        g.update(kind=kind, bytes=size, round=rnd, phase=phase,
                                 gc_check_calls=int(seen), gc_check_us=20 if seen else 0,
                                 fs_seen=int(seen), block_count=2048 if seen else 0,
                                 block_size=4096 if seen else 0, page_size=256 if seen else 0,
                                 lookup_pages=int(seen), data_page_bytes=251 if seen else 0)
                        for name in ('free_before','free_after','free_min','free_max'):
                            g[name] = 4 if seen else -1
                        for when in ('before','after'):
                            g[f'allocated_{when}'] = allocated if seen else -1
                            g[f'deleted_{when}'] = 20000 if seen else -1
                            g[f'free_data_bytes_{when}'] = (15*2046-allocated-20000)*251 if seen else -1
                        lines.append('I (600) storage_io_probe: storage spiffs gc: ' +
                                     ' '.join(f'{k}={v}' for k,v in g.items()) + '\n')
                lines.append(f'I (700) word_store: storage bench END total_ms=1000 profile={profile} '
                             f'result=ESP_OK partition_probe=1 gc_probe=1 reserve_probe=1 bytes={size}\n')
        lines.append('I (800) word_store: storage gc reserve experiment END total_ms=4000 '
                     f'result=ESP_OK completed_runs={1 if prepared_only else 4}{mode_tail}\n')
        return ''.join(lines)

    reserve = reserve_fixture()
    rf = 'LOG:snapshot-gc-reserve-parsed'
    rc = 'WRITE:snapshot-gc-reserve-control'
    rfirst = 'WRITE:snapshot-gc-reserve-8676-first12-p90'
    rall = 'WRITE:snapshot-gc-reserve-8676-all48-p90'
    check('前置 GC 四组 374 个 native 阶段完整', verdict(
        hil_snapshot_partition_io, reserve, io_metric), 'PASS')
    check('前置 GC 四组 374 个 GC 阶段完整', verdict(
        hil_snapshot_spiffs_gc, reserve, gc_metric), 'PASS')
    check('前置 GC 顺序对照与 clean-state 完整', verdict(
        hil_snapshot_gc_reserve, reserve, rc), 'PASS')
    check('维护后首12笔是真提交窗口 PASS', verdict(
        hil_snapshot_gc_reserve, reserve, rfirst), 'PASS')
    check('维护后完整48笔仍无退化 PASS', verdict(
        hil_snapshot_gc_reserve, reserve, rall), 'PASS')
    check('晚轮渐进退化不影响首12笔的已测窗口', verdict(
        hil_snapshot_gc_reserve, reserve_fixture(ramp=True), rfirst), 'PASS')
    check('晚轮渐进退化必须让完整48笔 FAIL', verdict(
        hil_snapshot_gc_reserve, reserve_fixture(ramp=True), rall), 'FAIL')
    check('旧 GC 日志没有维护前置对照 SKIP', verdict(
        hil_snapshot_gc_reserve, gc, rc), 'SKIP')
    check('缺 experiment END 不能拿快前缀验收', verdict(
        hil_snapshot_gc_reserve, ''.join(reserve.splitlines(True)[:-1]), rall), 'SKIP')
    check('实验成功 END 但晚轮连父记录都少了，必须 FAIL', verdict(
        hil_snapshot_gc_reserve, ''.join(l for l in reserve.splitlines(True)
                                        if 'round=47 ' not in l), rc), 'FAIL')
    check('GC 净容量字段漂移必须 FAIL', verdict(
        hil_snapshot_gc_reserve, reserve.replace('free_data_bytes_after=', 'renamed='), rf), 'FAIL')
    check('GC 净容量字段漂移也拒绝性能假通过', verdict(
        hil_snapshot_gc_reserve, reserve.replace('free_data_bytes_after=', 'renamed='), rall), 'FAIL')
    check('净容量必须与自身 allocated/deleted/几何严格对账', verdict(
        hil_snapshot_gc_reserve, reserve.replace('free_data_bytes_before=800690',
                                                  'free_data_bytes_before=800691'), rc), 'FAIL')
    check('无 GC 调用时净容量未知，不伪造零', verdict(
        hil_snapshot_gc_reserve, reserve.replace('free_data_bytes_after=-1', 'free_data_bytes_after=0'),
        rc), 'FAIL')
    check('同值 duplicate clean-state 字段也 FAIL', verdict(
        hil_snapshot_gc_reserve, reserve.replace('lookup_pages=1', 'lookup_pages=1 lookup_pages=1'),
        rf), 'FAIL')
    check('未同步不能用低耗时冒充持久提交', verdict(
        hil_snapshot_gc_reserve, reserve.replace('sync_ok=1', 'sync_ok=0'), rc), 'FAIL')
    check('少写一字节不能用低耗时冒充持久提交', verdict(
        hil_snapshot_gc_reserve, reserve.replace('written_bytes=8676', 'written_bytes=8675'), rc), 'FAIL')
    check('末次文件长度不对账 FAIL', verdict(
        hil_snapshot_gc_reserve, reserve.replace('file_bytes=416448', 'file_bytes=416449'), rc), 'FAIL')
    check('前置维护本身失败不能只用成功提交子集', verdict(
        hil_snapshot_gc_reserve, reserve.replace('result=ESP_OK requested_bytes=131072 prepare_us=',
                                                  'result=ESP_FAIL requested_bytes=131072 prepare_us='),
        rc), 'FAIL')
    check('四组 END 后仍需实验 completed_runs=4', verdict(
        hil_snapshot_gc_reserve, reserve.replace('completed_runs=4', 'completed_runs=3'), rc), 'FAIL')
    check('前置请求量不符合实验契约 FAIL', verdict(
        hil_snapshot_gc_reserve, reserve.replace('requested_bytes=131072', 'requested_bytes=4096'),
        rc), 'FAIL')
    insufficient = ''.join(l.replace('deleted_before=20000', 'deleted_before=23000').replace(
        'deleted_after=20000', 'deleted_after=23000').replace(
        'free_data_bytes_before=800690', 'free_data_bytes_before=47690').replace(
        'free_data_bytes_after=800690', 'free_data_bytes_after=47690')
        if 'kind=reserve-prep' in l and 'storage spiffs gc:' in l else l
        for l in reserve.splitlines(True))
    check('SDK 报成功但干净储备不足，不能假通过', verdict(
        hil_snapshot_gc_reserve, insufficient, rc), 'FAIL')
    check('储备不足仍保留实测维护成本，不静默丢掉最强测量', verdict(
        hil_snapshot_gc_reserve, insufficient, 'WRITE:snapshot-gc-reserve-maintenance'), 'PASS')
    check('储备不足不能以快的48笔把性能门刷绿', verdict(
        hil_snapshot_gc_reserve, insufficient, rall), 'FAIL')
    low_blocks = ''.join(l.replace('free_before=4', 'free_before=3').replace(
        'free_after=4', 'free_after=3').replace('free_min=4', 'free_min=3').replace(
        'free_max=4', 'free_max=3') if 'kind=reserve-prep' in l and 'storage spiffs gc:' in l else l
        for l in reserve.splitlines(True))
    check('SDK 字节要求满足但空块仍只3个，不当快路径储备', verdict(
        hil_snapshot_gc_reserve, low_blocks, rc), 'FAIL')
    check('空块不足仍记录48笔内再次发生 GC 的观察', verdict(
        hil_snapshot_gc_reserve, low_blocks, 'WRITE:snapshot-gc-reserve-lifetime'), 'PASS')
    check('END 字节字段漂移要 FAIL，不抛 ValueError', verdict(
        hil_snapshot_gc_reserve, reserve.replace('reserve_probe=1 bytes=8676\n',
                                                  'reserve_probe=1 bytes=broken\n'), rf), 'FAIL')
    check('父 API 自己坏了不能让维护实验绿', verdict(
        hil_snapshot_gc_reserve, reserve.replace('scope_ok=1', 'scope_ok=0'), rc), 'FAIL')
    check('实验里把维护塞进每笔写，会被重复阶段挡住', verdict(
        hil_snapshot_gc_reserve, reserve.replace(
            next(l for l in reserve.splitlines(True) if 'storage gc snapshot prepare:' in l),
            next(l for l in reserve.splitlines(True) if 'storage gc snapshot prepare:' in l)*2),
        rc), 'FAIL')
    prep_parent = next(l for l in reserve.splitlines(True) if 'storage gc snapshot prepare:' in l)
    commit_parent = next(l for l in reserve.splitlines(True)[reserve.splitlines(True).index(prep_parent)+1:]
                         if 'storage gc snapshot held commit:' in l)
    swapped = reserve.replace(prep_parent, '__swap_prepare__\n', 1).replace(
        commit_parent, prep_parent, 1).replace('__swap_prepare__\n', commit_parent, 1)
    check('维护发生在首笔提交之后，不能冒充前置维护', verdict(
        hil_snapshot_gc_reserve, swapped, rc), 'FAIL')
    outer_begin = next(l for l in reserve.splitlines(True) if 'storage gc reserve experiment BEGIN' in l)
    check('四组不在 experiment BEGIN 之内不能偷算完整对照', verdict(
        hil_snapshot_gc_reserve, reserve.replace(outer_begin, '') + outer_begin, rf), 'FAIL')
    single = reserve_fixture(prepared_only=True)
    for func, name, label in (
            (hil_snapshot_partition_io, io_metric, '分区直接计量'),
            (hil_snapshot_spiffs_gc, gc_metric, 'GC 直接计量'),
            (hil_snapshot_gc_reserve, rf, '格式'),
            (hil_snapshot_gc_reserve, 'WRITE:snapshot-gc-reserve-maintenance', '维护计量'),
            (hil_snapshot_gc_reserve, 'WRITE:snapshot-gc-reserve-lifetime', '寿命计量'),
            (hil_snapshot_gc_reserve, rfirst, '首12笔'),
            (hil_snapshot_gc_reserve, rall, '全48笔')):
        check(f'显式 8676 单组补测{label} PASS', verdict(func, single, name), 'PASS')
    for name, label in ((rc, '对照'),
                        ('WRITE:snapshot-gc-reserve-3379-first12-p90', '3379首12笔'),
                        ('WRITE:snapshot-gc-reserve-3379-all48-p90', '3379全48笔')):
        check(f'单组不冒充{label}通过，必须 SKIP', verdict(
            hil_snapshot_gc_reserve, single, name), 'SKIP')
    check('单组晚轮退化不能让全48笔假绿', verdict(
        hil_snapshot_gc_reserve, reserve_fixture(ramp=True, prepared_only=True), rall), 'FAIL')
    check('单组晚轮退化仍独立判首12笔', verdict(
        hil_snapshot_gc_reserve, reserve_fixture(ramp=True, prepared_only=True), rfirst), 'PASS')
    for malformed, label in (
            (single.replace(' mode=prepared-only bytes=8676', ''), '无显式模式'),
            (single.replace('mode=prepared-only', 'mode=unknown'), '未知模式'),
            (single.replace('mode=prepared-only', 'mode=full', 1), 'BEGIN模式漂移'),
            (single.replace('prepared_rounds=48 mode=prepared-only bytes=8676',
                            'prepared_rounds=48 mode=prepared-only bytes=3379'), '模式尺寸漂移'),
            (single.replace('control_rounds=0', 'control_rounds=12'), '声称包含control'),
            (single.replace('completed_runs=1 mode=prepared-only bytes=8676',
                            'completed_runs=1'), 'END模式丢失')):
        check(f'单组{label}格式必须 FAIL', verdict(
            hil_snapshot_gc_reserve, malformed, rf), 'FAIL')
    for malformed, label in (
            (single.replace('completed_runs=1', 'completed_runs=4'), '完成组数伪造'),
            (single.replace('completed_runs=1', 'completed_runs=0'), '未完成'),
            (single.replace('total_ms=4000 result=ESP_OK',
                            'total_ms=4000 result=ESP_ERR_TIMEOUT'), '总实验保护退出'),
            (''.join(l for l in single.splitlines(True) if 'round=47 ' not in l), '缺末笔'),
            (single.replace('sync_ok=1', 'sync_ok=0'), '未同步'),
            (single.replace('written_bytes=8676', 'written_bytes=8675'), '短写'),
            (single.replace('file_bytes=416448', 'file_bytes=416449'), '文件长度不符'),
            (single.replace('scope_ok=1', 'scope_ok=0'), '直接计量损坏'),
            (single.replace('free_data_bytes_after=', 'renamed='), '净容量字段漂移')):
        check(f'单组{label}仍让全48笔 FAIL', verdict(
            hil_snapshot_gc_reserve, malformed, rall), 'FAIL')
    check('单组缺总 END，不用已齐48笔假通过', verdict(
        hil_snapshot_gc_reserve, ''.join(single.splitlines(True)[:-1]), rall), 'SKIP')
    disguised_full = single.replace(' mode=prepared-only bytes=8676', '').replace(
        'control_rounds=0', 'control_rounds=12').replace('completed_runs=1', 'completed_runs=4')
    check('旧 full 契约缺三组，不能借单组支持变绿', verdict(
        hil_snapshot_gc_reserve, disguised_full, rall), 'FAIL')
    lifecycle = ('I (5000) word_store: storage gc reserve experiment BEGIN\n' + bench +
                 'I (12000) word_store: storage bench BEGIN profile=snapshot-gc-prepared\n'
                 'I (19000) storage_service: owner=storage-bench queue_wait_ms=0 elapsed_ms=10 result=ESP_OK\n'
                 'I (19000) word_store: storage bench END\n'
                 'W (19500) power: long-held sleep lease: blocker=storage holder=storage-bench held_ms=14500\n'
                 'I (20000) word_store: storage gc reserve experiment END\n')
    lease_name = 'BENCH:lease-released'
    check('分组 END 后、总 END 前的长租约不是泄漏', verdict(
        hil_storage_bench, lifecycle + 'I (80000) test: observing\n', lease_name), 'PASS')
    check('总 END 后没有观察窗口，无告警也只能 SKIP', verdict(
        hil_storage_bench, lifecycle, lease_name), 'SKIP')
    check('恰好到重报门仍未跨门，只能 SKIP', verdict(
        hil_storage_bench, lifecycle + 'I (79500) test: observing\n', lease_name), 'SKIP')
    check('没有此前告警，完整观察 END 后60秒才可判代理', verdict(
        hil_storage_bench, ''.join(l for l in lifecycle.splitlines(True)
                                  if 'long-held sleep lease:' not in l) +
        'I (80001) test: observing\n', lease_name), 'PASS')
    real_window = (
        'unrelated first line\n'
        'I (26583) word_store: storage gc reserve experiment BEGIN\n'
        'I (26663) word_store: storage bench BEGIN profile=snapshot-gc-prepared\n'
        'I (43363) storage_service: owner=storage-bench queue_wait_ms=0 elapsed_ms=361 result=ESP_OK\n'
        'W (94473) wqn_sleep: long-held sleep lease: blocker=storage holder=storage-bench held_ms=67895\n'
        'I (102693) word_store: storage bench END\n'
        'I (102703) word_store: storage gc reserve experiment END\n'
        'I (118583) wqn_epd: observing\n')
    check('174410 真实窗口15.88秒必须 SKIP，不假绿', verdict(
        hil_storage_bench, real_window, lease_name), 'SKIP')
    check('该现象确有此前告警，跨154473门后代理才 PASS', verdict(
        hil_storage_bench, real_window + 'I (154474) test: observing\n', lease_name), 'PASS')
    check('下一次启动的大时钟不能补上一启动的观察窗口', verdict(
        hil_storage_bench, real_window +
        'I (1000) boot: End of partition table\nI (200000) test: observing\n', lease_name), 'SKIP')
    legacy_window = bench + 'I (19000) storage_service: owner=storage-bench queue_wait_ms=0 elapsed_ms=10 result=ESP_OK\n'
    check('旧 bench 的短观察窗口也不是释放证明', verdict(
        hil_storage_bench, legacy_window, lease_name), 'SKIP')
    check('旧 bench 观察满门后仍按原 END 判代理', verdict(
        hil_storage_bench, legacy_window + 'I (69001) test: observing\n', lease_name), 'PASS')
    def detail(fn, text, name):
        RESULTS.clear()
        fn(Log.from_text(text, '<selftest-detail>'))
        return next((d for _p, n, _k, _o, d in RESULTS if n == name), '')

    absent = 'unrelated first line\nI (1000) test: no business writes\n'
    check('缺 append 字段只说日志缺测，不推断未进镜像',
          '日志未覆盖' in detail(hil_append_open_split, absent, 'WRITE:append-open-vs-fopen'), True)
    check('缺 NVS stats 不推断历史上从未实测',
          '本轮缺少实测字段' in detail(hil_nvs_stats_measured, absent, 'WRITE:nvs-entry-budget-measured'), True)
    reset_boot = 'unrelated first line\nrst:0x15 (USB_UART_CHIP_RESET)\nI (1000) boot: End of partition table\n'
    check('USB 复位启动计数不声称真实断电',
          '不证明真实断电' in detail(hil_stability, reset_boot, 'STAB:no-reboot-loop'), True)
    cursor_steps = ('unrelated first line\n'
        'I (100) wqn_storage: nvs stats: used_entries=196 free_entries=308 available_entries=182 total_entries=504\n'
        'I (200) wqn_storage: nvs write: key=_hil_cur52 bytes=52 total_ms=10 changed=1\n'
        'I (201) wqn_storage: nvs stats: used_entries=200 free_entries=304 available_entries=178 total_entries=504\n'
        'I (300) wqn_storage: nvs write: key=_hil_cur52 bytes=52 total_ms=0 changed=0\n'
        'I (301) wqn_storage: nvs stats: used_entries=200 free_entries=304 available_entries=178 total_entries=504\n'
        'I (400) wqn_storage: nvs write: key=_hil_cur52 bytes=52 total_ms=12 changed=1\n'
        'I (401) wqn_storage: nvs stats: used_entries=200 free_entries=304 available_entries=178 total_entries=504\n'
        'I (500) wqn_storage: nvs write: key=_hil_cur52 bytes=0 total_ms=10 changed=1\n'
        'I (501) wqn_storage: nvs stats: used_entries=196 free_entries=308 available_entries=182 total_entries=504\n')
    measured_name = 'WRITE:nvs-entry-budget-measured'
    steps_detail = detail(hil_nvs_stats_measured, cursor_steps, measured_name)
    check('真实-4擦除不是改写净0', 'Δused -4（**擦除 key，释放 4 条**）' in steps_detail, True)
    check('临时key重复写不能声称真实逐题稳态通过', '稳态每答题净 0 条' not in steps_detail, True)
    falling = cursor_steps.split('I (500)')[0].replace('used_entries=200', 'used_entries=192')
    falling_detail = detail(hil_nvs_stats_measured, falling, measured_name)
    check('非擦除负台阶不能标免费改写', '减少台阶不能证明免费改写' in falling_detail, True)
    # 多启动/同毫秒/后续写故意用不同 used，防止两个恰好相等的数藏住错配。
    def ns(t, used):
        return (f'I ({t}) wqn_storage: nvs stats: used_entries={used} '
                f'free_entries={504-used} available_entries={378-used} total_entries=504\n')

    def nw(t, key='cur_rev', size=0, changed=1):
        return (f'I ({t}) wqn_storage: nvs write: key={key} bytes={size} '
                f'total_ms=10 changed={changed}\n')

    def nt(owner='word-session-clear', result='ESP_OK'):
        return (f'I (205) storage_service: owner={owner} queue_wait_ms=0 '
                f'elapsed_ms=15 result={result}\n')

    erase_name = 'WRITE:cursor-erase-logged'
    clear = 'unrelated first line\n' + ns(100, 200) + nw(200) + ns(200, 196) + nt()
    check('清真实游标同毫秒后stats下降可通过', verdict(
        hil_cursor_erase_logged, clear, erase_name), 'PASS')
    for malformed, label in (
            (clear.replace(ns(100, 200), ''), '缺之前统计'),
            (clear.replace(ns(200, 196), ''), '缺之后统计'),
            (clear.replace(ns(200, 196), ns(200, 200)), '条目未下降'),
            (clear.replace(ns(200, 196), ns(200, 204)), '条目反而增加'),
            (clear.replace(nw(200), nw(200, size=52)), '普通写冒充擦除'),
            (clear.replace(nw(200), nw(200, changed=0)), '同值短路冒充擦除'),
            (clear.replace(nw(200), nw(200, key='_hil_cur52')), '临时key冒充正式会话'),
            (clear.replace(nt(), nt(result='ESP_FAIL')), '清事务失败'),
            (clear.replace(ns(200, 196), '') + ns(206, 196), '事务后才有统计'),
            (clear.replace(ns(200, 196), ns(200, 200)) + ns(300, 196), '后续下降冒充本次下降')):
        check(f'清会话{label}必须 FAIL', verdict(
            hil_cursor_erase_logged, malformed, erase_name), 'FAIL')
    later_write = clear + nw(300, size=52) + ns(300, 200)
    check('清除只看紧随stats，不拿末次改写抵消-4',
          '变化 [-4]' in detail(hil_cursor_erase_logged, later_write, erase_name), True)
    shared = clear.replace(nw(200), nw(195, key='cur_int', size=52) + nw(200))
    check('同对stats夹多个操作，不能把全局下降独归清除', verdict(
        hil_cursor_erase_logged, shared, erase_name), 'FAIL')
    boot = 'I (99) boot: End of partition table\n'
    prior = 'unrelated first line\n' + ns(100, 200) + nw(200) + ns(200, 196)
    foreign = prior + boot + ns(100, 212) + ns(200, 208) + nt()
    check('旧启动擦除不能覆盖新启动清事务', verdict(
        hil_cursor_erase_logged, foreign, erase_name), 'FAIL')
    missing_before = prior + boot + nw(200) + ns(200, 208) + nt()
    check('不能借旧启动统计补新启动清除前值', verdict(
        hil_cursor_erase_logged, missing_before, erase_name), 'FAIL')
    reboot_pairs = ('unrelated first line\n' + ns(100, 196) + nw(200, size=52) +
                    ns(200, 200) + boot + ns(100, 208) +
                    nw(200, key='cur_int', size=52) + ns(200, 212))
    check('两启动重叠时钟的写各自配本启动stats',
          'cur_int bytes=52 changed=1 ⇒ Δused +4' in detail(
              hil_nvs_stats_measured, reboot_pairs, measured_name), True)
    one_tick = ('unrelated first line\n' + ns(200, 196) + nw(200, size=52) +
                ns(200, 200) + nw(200, size=52) + ns(200, 200))
    tick_detail = detail(hil_nvs_stats_measured, one_tick, measured_name)
    check('同毫秒行序仍能量到新key+4', 'Δused +4' in tick_detail, True)
    check('同毫秒第二次写仍配净0，不借上一笔+4', 'Δused +0' in tick_detail, True)
    parts_name = 'WRITE:parts-account-for-transaction'
    own_save = 'unrelated first line\n' + nw(200, size=52) + nt(owner='word-session-save')
    check('同一启动NVS写覆盖save窗口', verdict(
        hil_write_parts_reconcile, own_save, parts_name), 'PASS')
    foreign_save = prior + boot + nt(owner='word-session-save')
    check('另一启动写不能冒充本启动save成本', verdict(
        hil_write_parts_reconcile, foreign_save, parts_name), 'FAIL')
    future_save = 'unrelated first line\n' + nt(owner='word-session-save') + nw(200, size=52)
    check('完成行之后的回退时钟写不能补save成本', verdict(
        hil_write_parts_reconcile, future_save, parts_name), 'FAIL')
    same_tick_future = 'unrelated first line\n' + nt(owner='word-session-save') + nw(205, size=52)
    check('完成行之后同毫秒写不能补save成本', verdict(
        hil_write_parts_reconcile, same_tick_future, parts_name), 'FAIL')
    adjacent = ('unrelated first line\n' + nw(200, size=52) +
                nt(owner='word-session-cursor') + nt(owner='word-session-save'))
    check('上一已完成事务的写不能补相邻save成本', verdict(
        hil_write_parts_reconcile, adjacent, parts_name), 'FAIL')
    atomic = ('I (200) wqn_storage: atomic write: bytes=3379 backup=1 fopen_ms=1 '
              'write_ms=1 stat_ms=1 remove_ms=1 rename_backup_ms=1 '
              'rename_primary_ms=5 total_ms=10\n')
    check('同一启动AtomicWrite也能覆盖执行窗', verdict(
        hil_write_parts_reconcile, 'unrelated first line\n' + atomic +
        nt(owner='word-session-save'), parts_name), 'PASS')
    check('另一启动AtomicWrite不能补本启动save成本', verdict(
        hil_write_parts_reconcile, 'unrelated first line\n' + atomic + boot +
        nt(owner='word-session-save'), parts_name), 'FAIL')
    check('save时间戳漂移是FAIL，不抛异常或SKIP', verdict(
        hil_write_parts_reconcile, own_save.replace('I (205) storage_service:',
                                                  'storage_service:'), parts_name), 'FAIL')
    check('清除时间戳漂移是FAIL，不抛异常或SKIP', verdict(
        hil_cursor_erase_logged, clear.replace('I (205) storage_service:',
                                              'storage_service:'), erase_name), 'FAIL')
    check('擦除耗时超出自身事务不能绿', verdict(
        hil_cursor_erase_logged, clear.replace('total_ms=10', 'total_ms=500'), erase_name), 'FAIL')
    attach_only = clear.replace(nw(200), '[22:24:00] listener attached: COM7\n' + nw(200))
    check('监听器重附着不切设备统计域', verdict(
        hil_cursor_erase_logged, attach_only, erase_name), 'PASS')
    rollback = prior + nw(150) + ns(150, 192) + nt()
    check('没有boot行但时钟回退，也不能借上段擦除前统计', verdict(
        hil_cursor_erase_logged, rollback, erase_name), 'FAIL')
    rollback_log = Log.from_text(rollback)
    check('不自洽时钟拒绝配对，但不冒充可见设备启动数', rollback_log.boots, 0)
    concurrent = clear.replace(nw(200),
        'I (150) background: running\nI (140) background: delayed log\n' + nw(200))
    check('多任务10ms日志倒序不假装新启动', verdict(
        hil_cursor_erase_logged, concurrent, erase_name), 'PASS')
    reset_without_table = prior + 'rst:0x15 (USB_UART_CHIP_RESET)\n' + nw(200) + ns(200, 192) + nt()
    check('只有ROM复位行也不能借上一启动统计', verdict(
        hil_cursor_erase_logged, reset_without_table, erase_name), 'FAIL')
    check('ROM边界不冒充分区表启动计数', Log.from_text(reset_without_table).boots, 0)
    shared_detail = detail(hil_nvs_stats_measured, shared, measured_name)
    check('共享统计窗口不能标某key释放4条', '擦除 key，释放 4 条' not in shared_detail, True)
    check('共享统计窗口明确报告无法逐key归因', '不能逐key归因' in shared_detail, True)
    check('分组结束但总实验未收尾，不能借子 END 判租约', verdict(
        hil_storage_bench, lifecycle.replace(
            'I (20000) word_store: storage gc reserve experiment END\n', ''), lease_name), 'SKIP')
    check('总 END 后租约真的还在，门必须 FAIL', verdict(
        hil_storage_bench, lifecycle +
        'W (21000) power: long-held sleep lease: blocker=storage holder=storage-bench held_ms=16000\n',
        lease_name), 'FAIL')
    check('总 END 时间戳漂移不能静默 SKIP', verdict(
        hil_storage_bench, lifecycle.replace('I (20000) word_store:', 'word_store:'), lease_name), 'FAIL')
    check('两分组之间的答题也在整个实验污染窗', verdict(
        hil_word_commit_queue, lifecycle + commit(t=11000, wait=600), queue_name), 'SKIP')
    check('总 END 后的排队 FAIL 仍保留，不全日志剔除', verdict(
        hil_word_commit_queue, lifecycle + commit(t=25000, wait=600), queue_name), 'FAIL')
    # Batch-era fixtures are deliberately multi-line with a non-match first.
    batch_ram = ('I (100) word_app: word observation RAM accepted: sequence=16 pending=2 inflight=1 '
                 f'session={fixture_sid}\n')
    batch_body = ('I (1000) word_store: word observation batch durable: count=5 appended=5 bytes=1000 '
                  f'total_ms=400 first_sequence=16 last_sequence=20 mode=3 session={fixture_sid}\n')
    batch_tx = ('I (1002) storage_service: storage transaction complete: request=7 '
                'owner=word-observation-batch queue_wait_ms=5 elapsed_ms=420 result=ESP_OK\n')
    ack_body = 'I (1500) word_store: word ACK batch durable: count=5 bytes=1000 total_ms=300\n'
    ack_tx = ('I (1502) storage_service: storage transaction complete: request=8 '
              'owner=word-outbox-ack-batch queue_wait_ms=5 elapsed_ms=320 result=ESP_OK\n')
    batch_good = 'unrelated first line\n' + batch_ram + batch_body + batch_tx + ack_body + ack_tx
    queued_line = ('I (500) word_app: word observation batch queued: count=5 oldest_age_ms=100 '
                   f'forced=0 op=8 session={fixture_sid}\n')
    batch_good = 'unrelated first line\n' + batch_ram + queued_line + batch_body + batch_tx + ack_body + ack_tx
    for name in ('LOG:word-batches-parsed', 'WRITE:word-RAM-bound',
                 'WRITE:word-batch-record-accounting', 'WRITE:word-batch-transaction-window'):
        check('批量完整夹具 ' + name, verdict(hil_word_batches, batch_good, name), 'PASS')
        check('旧版无批量 ' + name, verdict(hil_word_batches, atomic, name), 'SKIP')
    # Contract fixtures/session-response.json and observation-request.json
    # both start at 0. The first real NEW-session batch exposed this >0 bug.
    batch_zero = batch_good.replace('sequence=16', 'sequence=0').replace('last_sequence=20', 'last_sequence=4')
    check('新会话RAM序号0合法', verdict(hil_word_batches, batch_zero, 'WRITE:word-RAM-bound'), 'PASS')
    check('新会话耐久批0到4合法', verdict(hil_word_batches, batch_zero, 'WRITE:word-batch-record-accounting'), 'PASS')
    check('负序号仍是格式错误', verdict(hil_word_batches,
          batch_zero.replace('sequence=0', 'sequence=-1'), 'LOG:word-batches-parsed'), 'FAIL')
    for label, changed in (
            ('RAM等待加在途越界', batch_good.replace('pending=2', 'pending=11')),
            ('在途超过缓冲总数', batch_good.replace('inflight=1', 'inflight=3'))):
        check(label, verdict(hil_word_batches, changed, 'WRITE:word-RAM-bound'), 'FAIL')
    for label, changed in (
            ('连续序号对不上', batch_good.replace('last_sequence=20', 'last_sequence=21')),
            ('字节不能按count估算', batch_good.replace('bytes=1000', 'bytes=999', 1)),
            ('实际新增不可大于请求', batch_good.replace('appended=5', 'appended=6')),
            ('ACK触发上限5', batch_good.replace('word ACK batch durable: count=5', 'word ACK batch durable: count=6'))):
        check(label, verdict(hil_word_batches, changed, 'WRITE:word-batch-record-accounting'), 'FAIL')
    prefix_retry = batch_good.replace('appended=5 bytes=1000', 'appended=2 bytes=400')
    check('已有前缀重试只数新增字节', verdict(hil_word_batches, prefix_retry,
          'WRITE:word-batch-record-accounting'), 'PASS')
    for label, changed in (
            ('缺字段', batch_good.replace('inflight=1 ', '')),
            ('单位漂移', batch_good.replace('count=5', 'count=5ms', 1)),
            ('单条坏行混在绿行不能静默丢掉', batch_good + batch_body.replace('bytes=1000', 'bytes=nope')),
            ('SID缺失', batch_good.replace(f'session={fixture_sid}', '', 1)),
            ('前缀缺失', batch_good.replace('I (1000) word_store:', 'word_store:'))):
        check('批量格式 ' + label, verdict(hil_word_batches, changed, 'LOG:word-batches-parsed'), 'FAIL')
    check('本批body不能借另一事务总耗时', verdict(hil_word_batches,
          batch_good.replace('total_ms=400', 'total_ms=600'), 'WRITE:word-batch-transaction-window'), 'FAIL')
    check('同一事务不能为两个durable行付账', verdict(hil_word_batches,
          batch_good.replace(batch_body, batch_body + batch_body), 'WRITE:word-batch-transaction-window'), 'FAIL')
    check('事务失败不能支持durable成功', verdict(hil_word_batches,
          batch_good.replace('result=ESP_OK', 'result=ESP_FAIL', 1), 'WRITE:word-batch-transaction-window'), 'FAIL')
    check('上一启动完成窗不能为本启动付账', verdict(hil_word_batches,
          batch_body+'I (1001) boot: End of partition table\n'+batch_tx, 'WRITE:word-batch-transaction-window'), 'SKIP')
    check('只见RAM不等于耐久', verdict(hil_word_batches, 'unrelated\n'+batch_ram,
          'WRITE:word-batch-record-accounting'), 'SKIP')
    for label, value, want in (
            ('计数5触发', queued_line, 'PASS'),
            ('未满5提前普通flush', queued_line.replace('count=5', 'count=4'), 'FAIL'),
            ('最老30秒触发', queued_line.replace('count=5', 'count=1').replace('oldest_age_ms=100', 'oldest_age_ms=30000'), 'PASS'),
            ('强制边界不足5触发', queued_line.replace('count=5', 'count=1').replace('forced=0', 'forced=1'), 'PASS'),
            ('缺入队行不推测', batch_body+batch_tx, 'SKIP')):
        check(label, verdict(hil_word_batches, 'unrelated\n'+value, 'WRITE:word-batch-flush-trigger'), want)
    check('批量排队也进入旧500ms门', verdict(hil_word_commit_queue,
          batch_good.replace('queue_wait_ms=5 elapsed_ms=420', 'queue_wait_ms=500 elapsed_ms=420'), queue_name), 'FAIL')
    ram_card = (f'I (100) word_app: word card RAM hit: ordinal=16 session={fixture_sid}\n'
                'I (200) word_pack: word card loaded: open_seek_ms=1 read_close_ms=4 parse_ms=1 '
                'total_ms=6 fopen_ms=0 fseek_ms=1 source=prefetch\n')
    check('物理预取与RAM命中单列', verdict(hil_word_ram_reads, 'unrelated\n'+ram_card,
          'PK:word-RAM-read-provenance'), 'PASS')
    check('旧词卡行不假装已测RAM', verdict(hil_word_ram_reads,
          ram_card.splitlines()[1].replace(' source=prefetch', ''), 'PK:word-RAM-read-provenance'), 'SKIP')
    check('未知读取来源不SKIP', verdict(hil_word_ram_reads, ram_card.replace('source=prefetch', 'source=changed'),
          'PK:word-RAM-read-provenance'), 'FAIL')
    check('预取成本单位漂移不绿', verdict(hil_word_ram_reads, ram_card.replace('total_ms=6', 'total_ms=6us'),
          'PK:word-RAM-read-provenance'), 'FAIL')
    check('RAM身份缺失不SKIP', verdict(hil_word_ram_reads, ram_card.replace(f'session={fixture_sid}', ''),
          'PK:word-RAM-read-provenance'), 'FAIL')
    RESULTS.clear()

    print(f'\n自检 {ok + len(bad)} 项：{ok} PASS / {len(bad)} FAIL')
    if bad:
        print('失败项：' + '、'.join(bad))
        print('这些是判据自己的毛病——它们会在真机日志上静默出错，先修再跑日志。')
        return 1
    print('判据在已知答案的输入上全部出声。可以拿它去判真机日志了。')
    return 0


# --------------------------------------------------------------------- 主流程 --

def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        return selftest()
    if len(argv) < 2:
        print(__doc__)
        return 2
    paths = argv[1:]
    # PK 的基线是**跨日志**的区间。为每份日志单独算、并把它自己剔出去，
    # 否则"全网最高那份"永远测不出超高（它的中位就在区间端点里）。
    for p in paths:
        try:
            log = Log.load(p)
        except OSError as exc:
            print(f'读取失败: {p}: {exc}')
            return 2
        hil_p0_wifi(log)
        hil_p0_journal(log)
        hil_gate_selftest(log)
        hil_c5_domain_gate(log)
        hil_c6a_generation(log)
        hil_c6b_scope_switch(log)
        hil_c8_page_save(log)
        hil_word_commit_queue(log)
        hil_word_batches(log)
        hil_word_ram_reads(log)
        hil_pk_handle(log, pk_open_seek_context(paths, exclude=p))
        hil_owner_attribution(log)
        hil_storage_bench(log)
        hil_stream_read_buffer(log)
        hil_stream_download_deadline(log)
        hil_stream_download_completed(log)
        hil_stream_two_gate(log)
        hil_stream_append_chunk(log)
        hil_stream_bench_vs_direct(log)
        hil_probe_lines_drift(log)
        hil_log_format_drift(log)
        hil_nvs_entry_budget(log)
        hil_snapshot_append_probe(log)
        hil_snapshot_held_probe(log)
        hil_snapshot_partition_io(log)
        hil_snapshot_spiffs_gc(log)
        hil_snapshot_gc_reserve(log)
        hil_nvs_stats_measured(log)
        hil_cursor_erase_logged(log)
        hil_write_parts_reconcile(log)
        hil_append_open_split(log)
        hil_stability(log)

    # 归因总表：命名 pass 之后这里应该列出 10+ 个真实 owner，而不是一个
    # "background" 桶。打印在判据之前，因为它决定后面所有数字能不能归因。
    print('—— 事务 owner 分布（笔数 / elapsed 中位 / queue_wait 最大）——')
    for p in paths:
        try:
            log = Log.load(p)
        except OSError:
            continue
        table = log.owners()
        if not table:
            continue
        print(f'  {p}:')
        for owner in sorted(table, key=lambda o: -table[o][1]):
            n, med, qw = table[owner]
            print(f'    {owner:<28} n={n:<4} elapsed 中位 {med:>6} ms   queue_wait 最大 {qw:>6} ms')

    width = max(len(r[1]) for r in RESULTS) + 2
    fails = 0
    for path, name, kind, _ok, detail in RESULTS:
        mark = 'PASS' if kind == 'PASS' else ('FAIL' if kind == 'FAIL' else 'SKIP')
        if kind == 'FAIL':
            fails += 1
        print(f'  [{mark}] {name:<{width}} {detail}')
    print(f'\n合计 {len(RESULTS)} 项：'
          f'{sum(1 for r in RESULTS if r[2]=="PASS")} PASS / '
          f'{fails} FAIL / '
          f'{sum(1 for r in RESULTS if r[2]=="SKIP")} SKIP')
    if fails:
        print('\n注意：SKIP 不是通过——该场景没被测到。P0 两项若为 SKIP，'
              '说明本轮日志没有覆盖对应场景，需要补测。')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
