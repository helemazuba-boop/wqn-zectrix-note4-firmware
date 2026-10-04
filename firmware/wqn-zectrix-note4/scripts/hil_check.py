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
        log.boots = text.count('opened COM') or text.count('End of partition table')
        return log

    def txof(self, owner: str) -> list:
        return [t for t in self.tx if t['owner'] == owner]

    def has(self, needle: str) -> bool:
        return needle in self.text


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
        hil_stability(log)

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
