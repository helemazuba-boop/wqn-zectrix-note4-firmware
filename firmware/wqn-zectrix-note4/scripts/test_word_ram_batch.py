#!/usr/bin/env python3
"""Exercise production RAM acceptance/flush/result reducers with host seams.

No flash, device or rendering. Lifecycle reducers keep their real control flow;
card reads, review bookkeeping and final navigation are isolated host seams.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f'production boundary drift: {start}')
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/word_app.cpp')
    parser.add_argument('--policy', type=Path, default=root / 'main/word_batch_policy.h')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    header = (root / 'main/word_app.h').read_text(encoding='utf-8')
    pieces = {
        '@@POLICY@@': args.policy.read_text(encoding='utf-8').replace('#pragma once', ''),
        '@@SESSION@@': section(header, 'struct WordSessionState {', '\n// One mounted deck'),
        '@@REDUCERS@@': section(source, 'bool TakeWordObservationEffect(',
            '\nbool SameWordCardPrefetchIndex(' if 'bool SameWordCardPrefetchIndex(' in source
            else '\nWordAppSnapshot BuildWordAppSnapshot('),
    }
    fixture = (root / 'scripts/testdata/word_ram_batch.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-ram-batch-') as directory:
        unit, binary = Path(directory) / 'batch.cpp', Path(directory) / 'batch'
        unit.write_text(fixture, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
