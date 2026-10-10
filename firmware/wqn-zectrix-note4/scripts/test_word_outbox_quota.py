#!/usr/bin/env python3
"""Test production word outbox byte bounds and cache invalidation using libc files.

CRC uses a host ROM adapter. Faults occur at libc call boundaries, not within
ESP32 flash programming: this is NOT physical power-loss or concurrency HIL.
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
    parser.add_argument('--source', type=Path, default=root / 'main/word_study_store.cpp')
    args = parser.parse_args()
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable; fixtures not run')
    source = args.source.read_text(encoding='utf-8')
    quota = 'constexpr size_t kOutboxMaxRecords =' in source
    batch = 'constexpr size_t kOutboxAppendBatchCapacity =' in source
    pieces = {
        '@@BATCH_ENABLED@@': f'#define FIXTURE_BATCH {int(batch)}\n',
        '@@RECORDS@@': section(source, '#pragma pack(push, 1)\nstruct SessionHeader {',
                              '\n// Every public operation below'),
        '@@UTILITIES@@': section(source, 'uint32_t Crc32(',
                                '\ntemplate <typename T>\nvoid AppendScalar('),
        '@@VALID_MODE@@': section(source, 'bool ValidMode(', '\nbool ValidPurpose('),
        '@@READ@@': section(source, 'uint32_t RecordCrc(',
                           '\nconstexpr size_t kOutboxMaxRecords =' if quota else
                           '\n// [measure] §五之十 §6. `open_ms`'),
        '@@QUOTA@@': ('#define FIXTURE_QUOTA 1\n' + section(
            source, 'constexpr size_t kOutboxMaxRecords =',
            '\n// [measure] §五之十 §6. `open_ms`') if quota else
            '#define FIXTURE_QUOTA 0\nconstexpr size_t kOutboxMaxRecords = 2064;\n'
            'constexpr size_t kOutboxMaxBytes = kOutboxMaxRecords * sizeof(OutboxRecord);\n'),
        '@@APPEND_TO@@': section(source,
                                'constexpr size_t kOutboxAppendBatchCapacity =' if batch else
                                'esp_err_t AppendOutboxRecordTo(',
                                '\n// The rejected journal is forensic data'),
        '@@WRITE@@': section(source,
                            'esp_err_t AppendOutboxRecords(\n' if batch else
                            'esp_err_t AppendOutboxRecord(\n',
                            '\nbool SetSessionCursorOrdinal('),
    }
    template = (root / 'scripts/testdata/word_outbox_quota.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-quota-build-') as directory:
        unit, binary = Path(directory) / 'quota.cpp', Path(directory) / 'quota'
        unit.write_text(template, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
