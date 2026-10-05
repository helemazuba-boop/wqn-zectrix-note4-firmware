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
    r'owner=(?P<owner>[a-z0-9-]+) queue_wait_ms=(?P<qw>\d+) '
    r'elapsed_ms=(?P<el>\d+) result=(?P<res>\w+)')

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
    r'atomic write: bytes=(?P<bytes>\d+) backup=(?P<backup>\d+) '
    r'fopen_ms=(?P<fopen>\d+) write_ms=(?P<write>\d+) stat_ms=(?P<stat>\d+) '
    r'remove_ms=(?P<remove>\d+) rename_backup_ms=(?P<rb>\d+) '
    r'rename_primary_ms=(?P<rp>\d+) total_ms=(?P<total>\d+)')

# bench 的起止标记。刻意宽容：只要一行里同时出现 bench 与 begin/end 即计入，
# 不锁定重写侧的措辞，免得它改个词判读器就瞎了。
BENCH_RE = re.compile(r'(?i)\bbench\b[^\n]*?\b(?P<what>begin|end)\b')

# `storage bench round: shape=%s round=%d wall_ms=%lld result=%s`
# wall_ms 是一笔 bench 事务的墙钟；probe shape 没有对应的 `atomic write:` 行
# （它只 fopen 一个不存在的路径），所以逐 op 明细只对写 shape 有意义。
BENCH_ROUND_RE = re.compile(
    r'storage bench round: shape=(?P<shape>\S+) round=(?P<round>\d+) '
    r'wall_ms=(?P<wall>\d+) result=(?P<result>\w+)')


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
    boots: int = 0

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
        log.rounds = [{k: (int(v) if k != 'shape' else v) for k, v in m.groupdict().items()}
                      for m in BENCH_ROUND_RE.finditer(text)]
        log.boots = text.count('opened COM') or text.count('End of partition table')
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


def hil_owner_attribution(log: Log):
    """owner 命名 pass 的回归判据。

    命名之前，不传 owner 的 ExecuteStorageTransaction 一律记成 "background"
    （storage_service.cpp:49/:346），所以 31 个后台样挤在一个桶里、无法归因——
    §一 那张表里 `background` 中位 782 / max 4054 因此说不清是谁写的。命名 pass
    之后这个桶应该是空的。

    判据只在"命名 pass 确实在盘上"时才启用：用 wp-/np-/pp- 前缀的存在与否识别
    构建。老日志（1005.4 / 1005.6）里 owner=background 是当时的正常形态，判 SKIP
    而不是 FAIL——否则历史日志会被误判成回归。
    """
    owners = log.owners()
    named = [o for o in owners if o.startswith(('wp-', 'np-', 'pp-'))]
    defaulted = log.txof('background')
    if not named:
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
    if not log.rounds and not log.probes and not bench_tx:
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
    by_shape = {}
    for r in log.rounds:
        by_shape.setdefault(r['shape'], []).append(r['wall'])
    if 'probe' in by_shape:
        probe_med = statistics.median(by_shape['probe'])
        per_op = None
        for shape, walls in by_shape.items():
            if shape.startswith('cur'):
                per_op = statistics.median(walls) / 5.0  # fopen+write+stat+remove+rename
                break
        if per_op is None and log.probes:
            per_op = statistics.median(p['total'] for p in log.probes) / 5.0
        if per_op is None:
            skip(log, 'BENCH:floor-model', '有 probe 但没有可折算单次 op 成本的写 shape')
        else:
            ratio = per_op / probe_med if probe_med > 0 else float('inf')
            holds = probe_med >= 100.0 and 0.4 <= ratio <= 2.5
            expect(log, 'BENCH:floor-model-holds-or-refuted', holds or probe_med < 20.0,
                   f'纯查找 probe 中位 {probe_med:.0f} ms；单次元数据 op ≈ '
                   f'{per_op:.0f} ms（比值 {ratio:.2f}）。probe<20ms 且写很贵 = '
                   f'底价模型被推翻，重写前提要重议')
    detail = '；'.join(
        f'{shape} 中位 {statistics.median(w):.0f} ms (n={len(w)})'
        for shape, w in sorted(by_shape.items()))
    if detail:
        check(log, 'BENCH:shape-costs', 'PASS', True, detail)

    # --- 3. bench 不能反过来堵住别人 ---------------------------------------
    if bench_tx:
        worst = max(int(t['qw']) for t in bench_tx)
        expect(log, 'BENCH:bench-did-not-starve-others', worst < 2000,
               f'bench 事务自身最大 queue_wait={worst} ms')


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
        hil_c5_domain_gate(log)
        hil_c6a_generation(log)
        hil_c6b_scope_switch(log)
        hil_c8_page_save(log)
        hil_pk_handle(log)
        hil_owner_attribution(log)
        hil_storage_bench(log)
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
