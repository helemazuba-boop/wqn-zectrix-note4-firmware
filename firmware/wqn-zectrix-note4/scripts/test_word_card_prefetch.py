#!/usr/bin/env python3
"""Compile production prefetch reducers/cache read and ACK mailbox on a host.

Only card I/O, review selection and ESP services are seams; no flash/device.
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
    parser.add_argument('--worker', type=Path, default=root / 'main/ui/persist_worker.cpp')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    worker = args.worker.read_text(encoding='utf-8')
    header = (root / 'main/word_app.h').read_text(encoding='utf-8')
    pieces = {
        '@@STATE@@': section(header, 'struct WordCardPrefetchState {', '\nstruct WordAppState {'),
        '@@CACHE_READ@@': section(source, 'esp_err_t LoadCurrentReviewWord(', '\n// Decide where the card'),
        '@@REDUCERS@@': section(source, 'bool SameWordCardPrefetchIndex(', '\nWordAppSnapshot BuildWordAppSnapshot('),
        '@@CLEANUP@@': section(worker, 'void ClearCommandPayload(', '\nvoid PersistWorkerTask('),
        '@@MAILBOX@@': section(worker, 'bool TakePersistResultToApply(', '\nbool IsPersistKindBusy('),
    }
    fixture = (root / 'scripts/testdata/word_card_prefetch.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-card-prefetch-') as directory:
        unit, binary = Path(directory) / 'prefetch.cpp', Path(directory) / 'prefetch'
        unit.write_text(fixture, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
