#!/usr/bin/env python3
"""Summarize bounded mem_diag serial samples without inferring leak ownership."""
import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path
import re


POOLS = ('internal', 'dma', 'psram')
ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')
PAIR = re.compile(r'(\w+)=([^\s]+)')


def parse_log(lines, *, prefer_history=True, with_metadata=False):
    samples, failures = [], []
    session, previous_sequence = 0, None
    exports, active = [], None
    for line in lines:
        line = ANSI.sub('', line)
        match = re.search(r'\bmem_diag:\s+(history_begin|history_end|history_sample|'
                          r'history_alloc_failed|sample|alloc_failed)\s+(.*)', line)
        if not match:
            continue
        kind, fields = match.groups()
        raw = dict(PAIR.findall(fields))
        history = kind.startswith('history_')
        if history:
            kind = kind.removeprefix('history_')
        if kind in ('begin', 'end'):
            try:
                metadata = {key: int(value) for key, value in raw.items()}
            except ValueError:
                continue
            if 'export' not in metadata:
                continue
            if kind == 'begin':
                active = {'begin': metadata, 'end': None, 'samples': [], 'failures': [],
                          'export': metadata['export']}
                exports.append(active)
            elif active is not None and active['export'] == metadata['export']:
                active['end'] = metadata
            continue
        required = (['seq', 't_ms', 'stage', 'op'] +
                    [f'{p}_{s}' for p in POOLS for s in ('free', 'min', 'largest')]
                    if kind == 'sample' else ['seq', 't_ms', 'bytes', 'caps'])
        if any(key not in raw for key in required):
            continue  # Ignore a truncated serial line, including a torn last line.
        try:
            record = {key: (value if key in ('stage', 'function') else
                            int(value, 16 if key == 'caps' else 10))
                      for key, value in raw.items()}
        except ValueError:
            continue
        if history:
            if 'export' not in record:
                continue
            if active is None or active['export'] != record['export']:
                # A listener may attach halfway through a dump. Keep the data
                # but explicitly report the missing beginning as incomplete.
                active = {'begin': None, 'end': None, 'samples': [], 'failures': [],
                          'export': record['export']}
                exports.append(active)
            record['session'] = 0
            active['samples' if kind == 'sample' else 'failures'].append(record)
            continue
        if kind == 'sample':
            if previous_sequence is not None and record['seq'] <= previous_sequence:
                session += 1
            previous_sequence = record['seq']
        record['session'] = session
        (samples if kind == 'sample' else failures).append(record)
    selected = exports[-1] if prefer_history and exports else None
    if selected is not None:
        samples, failures = selected['samples'], selected['failures']
    result = (samples, failures, selected) if with_metadata else (samples, failures)
    return result


def summarize(samples, failures, history=None):
    grouped = defaultdict(list)
    for sample in samples:
        grouped[sample['session']].append(sample)
    lines = []
    if history is not None:
        begin, end = history['begin'], history['end']
        received = len(samples) + len(failures)
        expected = sum(begin.get(k, 0) for k in ('samples', 'events', 'failures')) if begin else None
        complete = (begin is not None and end is not None and end.get('emitted') == received
                    and expected == received and end.get('overwritten_during_export', 0) == 0)
        lines.append(f"history_export={history['export']} complete={'yes' if complete else 'no'} "
                     f"records_received={received} records_expected={expected} "
                     f"records_emitted={end.get('emitted', 'unknown') if end else 'unknown'}")
        if begin is not None:
            lines.append('history retention overwritten: samples=%d events=%d failures=%d' % (
                begin.get('sample_overwritten', 0), begin.get('event_overwritten', 0),
                begin.get('failure_overwritten', 0)))
        if end is not None:
            lines.append(f"history overwritten during export={end.get('overwritten_during_export', 0)}")
        if not complete:
            lines.append('Incomplete history export: attach the no-reset monitor and reopen '
                         'Developer / Build and runtime info; retain history_begin through history_end.')
    for session, records in sorted(grouped.items()):
        first, last = records[0], records[-1]
        lines.append(f"session={session} samples={len(records)} "
                     f"time_ms={first['t_ms']}..{last['t_ms']}")
        for pool in POOLS:
            free, largest = f'{pool}_free', f'{pool}_largest'
            lines.append(f"  {pool}: free {first[free]} -> {last[free]} B "
                         f"(delta {last[free] - first[free]:+d}), "
                         f"observed_min_free={min(r[free] for r in records)} B, "
                         f"observed_min_largest={min(r[largest] for r in records)} B, "
                         f"reported_min={last[f'{pool}_min']} B")
        lines.append('  lost: samples=%d failures=%d; failed_allocations=%d' % (
            max(r.get('sample_lost', 0) for r in records),
            max(r.get('failure_lost', 0) for r in records),
            max(r.get('failures', 0) for r in records)))
        pending, deltas = {}, defaultdict(list)
        for record in records:
            stage, operation = record['stage'], record['op']
            if not operation:
                continue
            if stage.endswith('_begin'):
                pending[(stage[:-6], operation)] = record
            elif stage.endswith('_end'):
                name = stage[:-4]
                begin = pending.pop((name, operation), None)
                if begin:
                    deltas[name].append(record['internal_free'] - begin['internal_free'])
        for name, values in sorted(deltas.items()):
            lines.append(f"  {name}: paired_operations={len(values)} "
                         f"internal_free_delta_range={min(values):+d}..{max(values):+d} B")
    if failures:
        lines.append('allocation failures (including failures before the first sample):')
        counts = Counter((r['bytes'], r['caps'], r.get('function', 'unknown'))
                         for r in failures)
        for (size, caps, function), count in sorted(counts.items()):
            lines.append(f'  count={count} bytes={size} caps=0x{caps:08x} function={function}')
    if not samples:
        lines.append('No complete mem_diag samples found; use firmware with RAM diagnostics enabled.')
    lines.append('DMA overlaps internal RAM. Operation deltas include concurrent work; '
                 'remaining allocations and falling free space alone do not prove a leak.')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--csv', type=Path, help='Export sample timeline in bytes.')
    parser.add_argument('--live', action='store_true',
                        help='Analyze live samples instead of the latest retained-history export.')
    args = parser.parse_args()
    with args.log.open(encoding='utf-8-sig', errors='replace') as stream:
        samples, failures, history = parse_log(stream, prefer_history=not args.live, with_metadata=True)
    print(summarize(samples, failures, history))
    if args.csv:
        fields = ['session', 'export', 'seq', 't_ms', 'stage', 'op'] + [
            f'{p}_{s}' for p in POOLS for s in ('free', 'min', 'largest')
        ] + ['failures', 'sample_lost', 'failure_lost']
        with args.csv.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction='ignore')
            writer.writeheader()
            writer.writerows(samples)


if __name__ == '__main__':
    main()
