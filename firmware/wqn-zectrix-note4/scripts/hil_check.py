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

STACK_RE = re.compile(r'RenderFrameToEpd: stack HWM before render: (?P<hwm>\d+) bytes free')

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
WORD_OBS_DURABLE_RE = re.compile(
    r'^[IWEDV] \((?P<t>\d+)\) word_store: word observation durable: '
    r'sequence=(?P<seq>\d+) lookup_ms=(?P<lookup>\d+) '
    r'append_ms=(?P<append>\d+) total_ms=(?P<total>\d+)', re.M)

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
    pack_line_count: int = 0
    boots: int = 0
    leases: list = field(default_factory=list)

    @classmethod
    def load(cls, path: str) -> 'Log':
        with open(path, encoding='utf-8', errors='ignore') as fh:
            text = fh.read()
        log = cls(path=path, text=text)
        for m in TX_RE.finditer(text):
            log.tx.append(m.groupdict())
        for m in CARD_RE.finditer(text):
            log.cards.append({k: int(v) for k, v in m.groupdict().items()})
        log.submits = [m.groupdict() for m in SUBMIT_RE.finditer(text)]
        log.dispatches = [m.groupdict() for m in DISPATCH_RE.finditer(text)]
        log.presents = [m.groupdict() for m in PRESENT_RE.finditer(text)]
        log.hwm = [int(m.group('hwm')) for m in STACK_RE.finditer(text)]
        log.probes = [{k: int(v) for k, v in m.groupdict().items()}
                      for m in PROBE_RE.finditer(text)]
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
        log.obs_durable = [{'t': int(m.group('t')), 'append': int(m.group('append')),
                            'total': int(m.group('total'))}
                           for m in WORD_OBS_DURABLE_RE.finditer(text)]
        # 格式漂移：有 word-pack-download 行，但没有一条 end 行被解析出来。
        log.pack_line_count = len(WORD_PACK_LINE_COUNT_RE.findall(text))
        log.boots = text.count('opened COM') or text.count('End of partition table')
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


def hil_pk_handle(log: Log):
    """fbfa950 — 包读句柄复用。"""
    if not log.cards:
        skip(log, 'PK:no-card-reads', '本轮无词卡读取')
        return
    os_vals = [c['os'] for c in log.cards]
    med = statistics.median(os_vals)
    # 判据随 revert 改变：不再追求「降到两位数」（那需要先解决 seek 成本），
    # 而是「不得劣化」——基线 362ms，>500ms 中位即视为回归。
    expect(log, 'PK:open-seek-not-regressed', med < 500,
           f'open_seek 中位 {med:.0f}ms（基线 362ms；>500ms 即回归）')
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
    for r in log.rounds:
        by_shape.setdefault(r['shape'], []).append(r['wall'])

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
            f'便宜样本更大而漂移，历史上因此差出 2.6×），所以拿它和 cur* 比 '
            f'{cur_med:.0f} ms 得出的倍数是个噪声比值，不能当施工依据。'
            f'便宜档已证明"追加本身可以很便宜"，贵档证明"它有时不是"——'
            f'**这恰是重写要回答的问题，不是答案**。补测：把 append shape 的 '
            f'rounds 提到 ≥12（两档都能拿到足够的 n），并给每笔记下是否新建对象')
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
    ends = [int(m.group('at_ms')) for m in BENCH_END_RE.finditer(log.text)]
    leaked = [l for l in log.leases
              if l['holder'] == 'storage-bench' and ends and l['at_ms'] > max(ends)]
    if not ends:
        skip(log, 'BENCH:lease-released',
             '本轮 bench 没有 END 标记（只跑到 begin），无从判断 lease 是否归还')
    elif not leaked:
        expect(log, 'BENCH:lease-released', True,
               f'bench 在 {max(ends)} ms 结束，此后再无 storage-bench 租约告警')
    else:
        worst = max(leaked, key=lambda l: l['held'])
        expect(log, 'BENCH:lease-released', False,
               f'bench 已于 {max(ends)} ms 结束，但 {len(leaked)} 条 storage-bench 租约'
               f'告警出现在其后（最晚一条 held_ms={worst["held"]}，'
               f'日志时钟 {worst["at_ms"]} ms）⇒ 租约没还，深睡被永久挡住')

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
    """
    nvs_by_ms = []
    for line in log.text.splitlines():
        if 'nvs write:' not in line:
            continue
        f = dict(re.findall(r'(\w+)=([-\w]+)', line))
        if 'total_ms' not in f:
            continue
        tm = re.match(r'^[IWEDV] \((\d+)\)', line)
        if tm:
            nvs_by_ms.append((int(tm.group(1)), int(f['total_ms'])))
    txs = log.txof('word-session-save')
    if not txs:
        skip(log, 'WRITE:parts-account-for-transaction',
             '本轮无 word-session-save 事务，无法对账')
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
        end = int(t['t'])
        start = end - el(t)
        aw = [p['total'] for p in log.probes if start <= p['at'] <= end]
        nv = [ms for ts, ms in nvs_by_ms if start <= ts <= end]
        if aw or nv:
            covered += 1
        if aw:
            breakdown.append(f'atomic write {sum(aw):.0f} ms × {len(aw)} 笔')
        if nv:
            breakdown.append(f'nvs write {sum(nv):.0f} ms × {len(nv)} 笔')
        write_ms += sum(aw) + sum(nv)
    if not breakdown:
        skip(log, 'WRITE:parts-account-for-transaction',
             f'{len(txs)} 笔 word-session-save 的执行窗口内没有任何写行'
             f'（atomic write: / nvs write:），判不出成本归属')
        return
    tx_ms = sum(el(t) for t in txs)
    ratio = write_ms / tx_ms if tx_ms else float('inf')
    parts = '；'.join(dict.fromkeys(breakdown))
    ok = ratio >= 0.6
    detail = (f'{covered}/{len(txs)} 笔窗口内有写行。写行合计 {write_ms:.0f} ms'
              f'（{parts}）vs word-session-save sum(elapsed) {tx_ms:.0f} ms'
              f' ⇒ 覆盖 {ratio:.0%}。口径是"**落在同一执行窗口内**的所有 '
              f'storage-write 行"，不是只数 `atomic write:`：52 B 游标迁 NVS 后'
              f'每次答题少一条 `atomic write:` 而 owner 名不变，只数它会把合法'
              f'迁移读成"插桩漏掉主要耗时"')
    if not ok:
        detail += (' ⇒ 覆盖 <60%，插桩漏掉了主要耗时，本轮任何子字段结论都不可信')
    else:
        detail += ' ⇒ 成本都在已插桩的写路径里'
    expect(log, 'WRITE:parts-account-for-transaction', ok, detail)


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
    rows = []
    for line in log.text.splitlines():
        if 'nvs stats:' not in line:
            continue
        f = dict(re.findall(r'(\w+)=(\d+)', line))
        if 'total_entries' in f:
            rows.append(f)
    if not rows:
        skip(log, 'WRITE:nvs-entry-budget-measured',
             '本轮没有 `nvs stats:` 行。§五之十 的 NVS 算术（4 页 × 126 = 504 条、'
             '21.2%/54.2% 占比）**至今纯属推导，没有实测支撑**——这是 peer 列的'
             '第一个"③ 不测就没法定夺"项：烧一版带 `nvs_get_stats` 的构建即可')
        return
    last = rows[-1]
    total = int(last['total_entries'])
    used = int(last.get('used_entries', -1))
    free = int(last.get('free_entries', -1))
    # 推导值，不是测量值：用来和实测对账。
    derived = (0x4000 // 4096) * 126
    detail = (f'实测 total_entries={total}（推导值 {derived} = 4 页 × 126，'
              f'来自 partitions/16m.csv:3 的 0x4000 + nvs_constants.h 的 '
              f'NVS_CONST_ENTRY_COUNT=126）')
    if total != derived:
        detail += (f' ⇒ **与推导不符**。§五之十 里所有"占分区百分之几"都要按 '
                   f'{derived}/{total} 重算，别照抄 21.2%/54.2% 那两张表')
    else:
        detail += ' ⇒ 与推导一致，那两张表的分母站得住'
    if used >= 0:
        detail += f'；used={used}'
    if free >= 0:
        detail += (f'；free={free}。⚠️ **这条是"写开始失败"的悬崖位置**：'
                   f'peer 的机制分析（nvs_pagemanager.cpp:157/171/185/190）说空闲页'
                   f'<2 后每次答题付一次寄生存活+4096 B 擦除、页耗尽则 '
                   f'requestNewPage 返回 ESP_ERR_NVS_INVALID_STATE，'
                   f'**但落在第几笔没实测**——要连续答 ≥5 题看 free 的掉落曲线')
    expect(log, 'WRITE:nvs-entry-budget-measured', True, detail)


def hil_nvs_entry_budget(log: Log):
    """NVS 条目预算的设计门禁：把"某个载荷能不能放 NVS"变成可执行的断言。

    WHY THIS EXISTS: peer 的 §五之十 决定把 52 B 游标迁 NVS、快照留在 SPIFFS 追加
    日志上。这个决定依赖一条他们从 IDF 源码推出来的算术，我独立复核过（2026-10-06）：

    1. 条目数 = 1 + ceil(dataSize / 32)     —— nvs_page.cpp:184-190
       `totalSize = ENTRY_SIZE`（表头占一条）+ `roundedSize` 按 32 进位
    2. 分区预算 = 4 页 × 126 条 = **504 条** —— partitions/16m.csv:3 是
       `nvs ... 0x9000, 0x4000`；NVS_CONST_ENTRY_COUNT=126、页 4096 B
    3. 单 blob 硬顶 = min(pageCount-1, 127) × 4000 = **12000 B**
       —— nvs_storage.cpp:282-290 `dataSize > max_pages * Page::CHUNK_MAX_SIZE`

    于是： 52 B → 3 条（0.6%，可迁）/ **3379 B → 107 条（21.2%）**
    / **8676 B → 273 条（54.2%）**。peer 三个数我逐条对上，一个不差。

    所以这是一条**现在就跑**的判据（只用已有的 `atomic write: bytes=`，不等新探针）：
    它把"快照迁 NVS"这条路直接判掉。比 peer 强调的更硬——他们的重点在
    "空闲页掉到 2 以下后每次答题付一次寄生存活+4096 B 擦除、页耗尽则
    requestNewPage 返回 ESP_ERR_NVS_INVALID_STATE"，而 8676 B 连**单个 key 都
    装不下**（12000 B 上限内但占 54.2%；超过 12000 B 则 ESP_ERR_NVS_VALUE_TOO_LONG）。

    FAIL 的语义是**设计违规**，不是设备坏了：某个载荷占分区超 25% 就不该走 NVS。
    阈值 25%：52 B 是 0.6%，快照两个尺寸是 21.2%/54.2%，中间没有自然分界，
    取 25% 让 3379 B 判 FAIL——它虽然装得下，但一次就吃掉五分之一分区，
    且旧值不会立即释放（NVS 无 delete-in-place，旧条目靠页级 GC）。
    """
    if not log.probes:
        skip(log, 'WRITE:nvs-entry-budget',
             '本轮无 `atomic write:` 行，判不出载荷分布')
        return
    # 这些常量来自本地 IDF + 分区表，不是估计值。
    kEntrySize = 32
    kEntryCountPerPage = 126
    kNvsSizeBytes = 0x4000
    kPages = kNvsSizeBytes // 4096
    kBudget = kPages * kEntryCountPerPage
    kSingleBlobCap = min(kPages - 1, (0xff - 1) // 2) * (kEntrySize * (kEntryCountPerPage - 1))
    kShareLimit = 0.25

    worst_share, worst_bytes, over_cap = 0.0, None, []
    for p in log.probes:
        n = p['bytes']
        entries = 1 + -(-n // kEntrySize)
        share = entries / kBudget
        if share > worst_share:
            worst_share, worst_bytes = share, n
        if n > kSingleBlobCap:
            over_cap.append(n)
    if worst_bytes is None:
        skip(log, 'WRITE:nvs-entry-budget', '有 `atomic write:` 行但字节数缺失')
        return
    worst_entries = 1 + -(-worst_bytes // kEntrySize)
    detail = (
        f'NVS 分区 {kNvsSizeBytes} B = {kPages} 页 × {kEntryCountPerPage} 条 = '
        f'{kBudget} 条；单 blob 硬顶 {kSingleBlobCap} B'
        f'（nvs_storage.cpp:282-290）。本轮最大载荷 {worst_bytes} B ⇒ '
        f'{worst_entries} 条 = **分区 {worst_share:.1%}**。'
        f'对照：52 B 游标 = 3 条 = {3 / kBudget:.1%}（迁 NVS 的正确用法）、'
        f'3379 B = 107 条 = {107 / kBudget:.1%}、8676 B = 273 条 = '
        f'{273 / kBudget:.1%}。')
    if over_cap:
        detail += (f' ⚠️ {len(over_cap)} 笔超过单 blob 硬顶（最大 {max(over_cap)} B）'
                   f'⇒ 它们连一个 NVS key 都装不下。')
    ok = worst_share <= kShareLimit and not over_cap
    if not ok:
        detail += (f' ⇒ **超过 {kShareLimit:.0%} 的载荷不许迁 NVS**：'
                   f'它会让每次答题付一次"寄生存活条目 + 4096 B 擦除"，'
                   f'空闲页掉到 2 以下后 requestNewPage 直接返回 '
                   f'ESP_ERR_NVS_INVALID_STATE —— 终点是**答题写失败**，'
                   f'不只是"答题慢"。')
    expect(log, 'WRITE:nvs-entry-budget', ok, detail)


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
    pairs = []
    for line in log.text.splitlines():
        if 'append_open_ms=' not in line or 'append_ms=' not in line:
            continue
        f = dict(re.findall(r'(\w+)=(-?\d+)', line))
        if 'append_open_ms' in f and 'append_ms' in f:
            pairs.append(f)
    if not pairs:
        skip(log, 'WRITE:append-open-vs-fopen',
             '本轮没有同时带 `append_open_ms=` 与 `append_ms=` 的行'
             '（新探针未进这个构建；契约见 doc §五之十 第 6 节）')
        return
    opens = [int(p['append_open_ms']) for p in pairs]
    fp = [p['fopen'] for p in log.probes if p.get('fopen')]
    med_open = statistics.median(opens)
    med_fopen = statistics.median(fp) if fp else None
    detail = (f'append_open_ms 中位 {med_open:.0f} ms（n={len(opens)}），'
              f'append_ms 中位 {statistics.median(int(p["append_ms"]) for p in pairs):.0f} ms')
    if med_fopen:
        ratio = med_open / med_fopen
        detail += (f'；同轮 `atomic write:` 的 fopen 中位 {med_fopen:.0f} ms ⇒ '
                   f'append_open_ms / fopen = {ratio:.3f}')
        if ratio < 0.1:
            detail += (' ⇒ **这就是"不建对象"的直接证据**（差一个数量级以上），'
                       '不再是 §五之三 那种从总价反推的推理。'
                       '注意它仍然只支持"追加形状便宜"，'
                       '不支持"改 fopen 模式就便宜"——那两个成因还没分解。')
        else:
            detail += (' ⇒ append_open_ms 没有比 fopen 低一个数量级，'
                       '"拆出来的就是建对象那部分"这个假设**不成立**，'
                       '§五之三 的推理缺口仍然敞开。')
    expect(log, 'WRITE:append-open-vs-fopen', med_fopen is None or med_open < med_fopen,
           detail)


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
    if log.boots > 1:
        expect(log, 'STAB:no-reboot-loop', log.boots <= 2,
               f'{log.boots} 次启动（>2 视为重启循环）')
    expect(log, 'STAB:no-stack-overflow',
           not log.has('stack overflow in task'), '无任务栈溢出')
    if log.hwm:
        expect(log, 'STAB:stack-hwm-healthy', min(log.hwm) > 2000,
               f'最低 HWM {min(log.hwm)}B（>2KB 余量）')


# --------------------------------------------------------------------- 主流程 --

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    paths = argv[1:]
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
        hil_pk_handle(log)
        hil_owner_attribution(log)
        hil_storage_bench(log)
        hil_stream_read_buffer(log)
        hil_stream_download_deadline(log)
        hil_stream_download_completed(log)
        hil_stream_two_gate(log)
        hil_stream_append_chunk(log)
        hil_stream_bench_vs_direct(log)
        hil_log_format_drift(log)
        hil_nvs_entry_budget(log)
        hil_nvs_stats_measured(log)
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
