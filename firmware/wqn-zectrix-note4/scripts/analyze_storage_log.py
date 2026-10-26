#!/usr/bin/env python3
"""Summarize StorageService evidence; never infer UI latency or continuous occupancy."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
import math
from pathlib import Path
import re

ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
PAIR = re.compile(r'(\w+)=([^\s]+)')
EVENTS = {
    'abandoned': 'storage transaction abandoned before execution:',
    'queue_timeout': 'storage transaction timed out in queue:',
    'running_overrun': 'storage transaction overran foreground budget;',
    'low_stack': 'storage task stack low:',
    'invalid_completion': 'invalid storage completion:',
}
LIMITATIONS = [
    'Files and observed reboot segments stay separate; boot IDs can repeat after cold reset.',
    'Identity applies only after a valid FAULT:boot header; retained FAULT:record is not current identity.',
    'queue_wait_ms includes admission/queue waiting; elapsed_ms excludes completion logging and caller wakeup.',
    'Completion samples exclude admission failures, abandoned work and low-stack-only log branches.',
    'Capacity is an observation, not continuous occupancy; no interpolation or occupancy curve is inferred.',
    'Missing serial lines cannot be counted reliably; event counts and sample counts are observed lower bounds.',
    'Nearest-rank P50/P95/P99 require 20/200/1000 samples per group (reporting policy, not a confidence bound).',
    'Percentiles are suppressed for mixed-owner totals and unknown-build segments.',
    'No CPU, electrical power, front-end save latency or persistence-after-power-loss conclusion follows.',
]


def valid_label(value, limit=64):
    return bool(re.fullmatch(r'[\w.-]{1,%d}' % limit, value, flags=re.ASCII))


def statistics(values, *, allow_quantiles=True):
    if not values:
        return {'n': 0, 'max': None}
    ordered = sorted(values)
    result = {'n': len(values), 'max': ordered[-1]}
    for name, percentile, minimum in (('p50', .50, 20), ('p95', .95, 200), ('p99', .99, 1000)):
        if allow_quantiles and len(values) >= minimum:
            result[name] = ordered[math.ceil(len(values) * percentile) - 1]
    return result


def summarize_samples(samples, *, allow_quantiles=False):
    return {
        'count': len(samples),
        'results': dict(Counter(s['result'] for s in samples)),
        'errors_excluding_not_found': sum(s['result'] not in ('ESP_OK', 'ESP_ERR_NOT_FOUND') for s in samples),
        'queue_wait_ms': statistics([s['queue_wait_ms'] for s in samples], allow_quantiles=allow_quantiles),
        'elapsed_ms': statistics([s['elapsed_ms'] for s in samples], allow_quantiles=allow_quantiles),
    }


def parse_log(lines, source='<stream>'):
    segments = []
    current = None
    previous_start = None
    seen = set()

    def new_segment(reason):
        nonlocal current, previous_start, seen
        current = {'source': str(source), 'segment': len(segments) + 1, 'boundary': reason,
                   'build': None, 'boot': None, 'mem_diag': None, 'samples': [],
                   'capacity_observations': [], 'events': [], 'rejected_lines': [], 'duplicate_lines': []}
        segments.append(current)
        previous_start, seen = None, set()

    new_segment('file_start')
    for line_number, line in enumerate(lines, 1):
        line = ANSI.sub('', line)
        if re.search(r'\bESP-ROM:|\brst:0x|\bboot: ESP-IDF', line):
            new_segment('reboot_marker')
        if 'wqn_fault: FAULT:boot ' in line:
            raw = dict(PAIR.findall(line.split('FAULT:boot ', 1)[1]))
            try:
                identity = (raw['build'], int(raw['boot']),
                            int(raw['mem_diag']) if 'mem_diag' in raw else None)
                if not valid_label(identity[0], 40) or identity[1] <= 0 or identity[2] not in (None, 0, 1):
                    raise ValueError()
            except (KeyError, ValueError):
                # A torn identity line cannot leave an earlier identity active.
                new_segment('invalid_identity')
                current['rejected_lines'].append(line_number)
                continue
            old = (current['build'], current['boot'], current['mem_diag'])
            if old != identity and (current['build'] is not None or current['samples'] or
                                    current['events'] or current['capacity_observations']):
                new_segment('identity_change_or_late_attach')
            current.update(zip(('build', 'boot', 'mem_diag'), identity))
            continue
        capacity = 'wqn_storage: STORAGE:capacity ' in line
        boot_capacity = 'wqn_storage: storage SPIFFS ready: ' in line
        if capacity or boot_capacity:
            raw = dict(PAIR.findall(line))
            try:
                if capacity:
                    start, end = int(raw['query_start_us']), int(raw['query_end_us'])
                    total, used = int(raw['total_bytes']), int(raw['used_bytes'])
                    result = raw['result']
                    if start < 0 or end < start or not valid_label(result):
                        raise ValueError()
                else:
                    start = end = None  # IDF log timestamps are not esp_timer timestamps.
                    total, used, result = int(raw['total']), int(raw['used']), 'ESP_OK'
                if total < 0 or used < 0 or (result == 'ESP_OK' and (not total or used > total)):
                    raise ValueError()
                observation = {'line': line_number, 'query_start_us': start, 'query_end_us': end,
                               'total_bytes': total, 'used_bytes': used, 'result': result}
                if result == 'ESP_OK':
                    observation['used_percent'] = round(100 * used / total, 3)
                current['capacity_observations'].append(observation)
            except (KeyError, ValueError):
                current['rejected_lines'].append(line_number)
                # Invalidate the previous observation even when this read is truncated.
                current['capacity_observations'].append({'line': line_number, 'result': 'invalid'})
            continue
        if 'storage_service:' not in line:
            continue
        for kind, marker in EVENTS.items():
            if marker in line:
                current['events'].append({'line': line_number, 'kind': kind})
                break
        if 'storage transaction complete:' not in line:
            continue
        raw = dict(PAIR.findall(line))
        try:
            sample = {key: int(raw[key]) for key in ('request', 'queue_wait_ms', 'elapsed_ms', 'stack_free_bytes')}
            sample.update({key: raw[key] for key in ('owner', 'result')})
            if (sample['request'] <= 0 or any(sample[key] < 0 for key in
                ('queue_wait_ms', 'elapsed_ms', 'stack_free_bytes')) or
                not valid_label(sample['owner']) or not valid_label(sample['result'])):
                raise ValueError()
            if 'exec_start_us' in raw or 'exec_end_us' in raw:
                start, end = int(raw['exec_start_us']), int(raw['exec_end_us'])
                if (raw.get('clock') != 'esp_timer' or start < 0 or end < start or
                        (end - start) // 1000 != sample['elapsed_ms']):
                    raise ValueError()
                sample.update(exec_start_us=start, exec_end_us=end)
        except (KeyError, ValueError):
            current['rejected_lines'].append(line_number)
            continue
        start = sample.get('exec_start_us')
        if start is not None and previous_start is not None and start < previous_start:
            # One storage owner emits completion logs in execution order. Do not
            # use request order: foreground work may overtake older background work.
            new_segment('execution_clock_regression')
        fingerprint = tuple(sorted(sample.items()))
        if fingerprint in seen:
            current['duplicate_lines'].append(line_number)
            continue
        seen.add(fingerprint)
        if start is not None:
            previous_start = start
        sample['line'] = line_number
        observations = current['capacity_observations']
        if observations:
            observation = observations[-1]
            # A concurrent query is not a before-transaction observation.
            end = observation.get('query_end_us')
            if observation['result'] == 'ESP_OK' and (end is None or (start is not None and end <= start)):
                sample['last_capacity_line'] = observation['line']
                if end is not None:
                    sample['capacity_age_us'] = start - end
        current['samples'].append(sample)
    kept = []
    for segment in segments:
        if not any(segment[key] for key in ('samples', 'events', 'capacity_observations', 'rejected_lines')):
            continue
        segment['summary'] = summarize_samples(segment['samples'])
        owners = defaultdict(list)
        for sample in segment['samples']:
            owners[sample['owner']].append(sample)
        identified = segment['build'] not in (None, 'unknown') and segment['boot'] is not None
        segment['owners'] = {name: summarize_samples(samples, allow_quantiles=identified)
                             for name, samples in sorted(owners.items())}
        segment['event_counts'] = dict(Counter(event['kind'] for event in segment['events']))
        kept.append(segment)
    return kept


def analyze(paths):
    report = {'schema_version': 1, 'limitations': LIMITATIONS, 'files': [], 'segments': []}
    for path in paths:
        data = path.read_bytes()
        report['files'].append({'path': str(path), 'sha256': hashlib.sha256(data).hexdigest()})
        report['segments'].extend(parse_log(data.decode('utf-8-sig', errors='replace').splitlines(), path))
    return report


def render(report):
    lines = []
    for segment in report['segments']:
        identity = f"build={segment['build']} boot={segment['boot']} mem_diag={segment['mem_diag']}"
        lines.append(f"{segment['source']} segment={segment['segment']} {identity}")
        for owner, summary in [('ALL', segment['summary']), *segment['owners'].items()]:
            lines.append(f"  {owner}: n={summary['count']} results={summary['results']} "
                         f"queue_ms={summary['queue_wait_ms']} exec_ms={summary['elapsed_ms']}")
        lines.append(f"  capacity_observations={len(segment['capacity_observations'])} "
                     f"events={segment['event_counts']} rejected={len(segment['rejected_lines'])} "
                     f"duplicates={len(segment['duplicate_lines'])}")
    if not lines:
        lines.append('No complete storage samples or diagnostic events found.')
    return '\n'.join(lines + report['limitations'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    report = analyze(args.logs)
    print(render(report))
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    if args.csv:
        fields = ['source', 'segment', 'build', 'boot', 'mem_diag', 'line', 'request', 'owner',
                  'result', 'queue_wait_ms', 'elapsed_ms', 'exec_start_us', 'exec_end_us',
                  'stack_free_bytes', 'last_capacity_line', 'capacity_age_us']
        with args.csv.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for segment in report['segments']:
                context = {key: segment[key] for key in fields[:5]}
                writer.writerows(context | sample for sample in segment['samples'])


if __name__ == '__main__':
    main()
