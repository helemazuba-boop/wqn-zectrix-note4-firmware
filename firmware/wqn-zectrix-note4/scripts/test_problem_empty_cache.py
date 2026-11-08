#!/usr/bin/env python3
"""Test production problem query/write bodies with temporary files and I/O faults.

The JSON codec is a fixture stub. These tests cover empty-cache invalidation and
libc read/close errors, not ESP32 durability, concurrency or cloud contracts.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f'production function boundary drift: {start}')
    a = source.index(start)
    b = source.index(end, a)
    return source[a:b]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=root / 'main/problem_store.cpp')
    parser.add_argument('--idf-path', type=Path, default=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5')))
    args = parser.parse_args()
    compiler = shutil.which('g++')
    if compiler is None:
        raise SystemExit('FAIL: g++ unavailable; fixtures were not run')
    source = args.source.read_text(encoding='utf-8')
    pieces = {
        '@@STATE@@': section(source, 'constexpr size_t kMaxRejectedRecords',
                            '\nbool IsValidRequestId('),
        '@@VALIDATION@@': section(source, 'bool IsValidRequestId(',
                                 '\nstd::string EncodeObservationLine('),
        '@@IO@@': section(source, '// Reads every journal line (bounded);',
                         '\nstruct CommitContext {'),
        '@@TRANSACTIONS@@': section(source, 'struct CommitContext {',
                                   '\nesp_err_t ExecuteWithStorageLease('),
        # Only reset the production RAM cache between simulated boots if the
        # source has it. The old source must still compile to demonstrate red.
        '@@RESET_CACHE@@': ('g_empty_problem_outbox_known = false;'
                            if 'bool g_empty_problem_outbox_known' in source
                            else '// Original source has no RAM cache.'),
        '@@EVIDENCE@@': (root/'scripts/testdata/problem_outbox_evidence.cpp').read_text(),
        '@@PARKED@@': (root/'scripts/testdata/problem_park_journal.cpp').read_text(),
        '@@PARK_UPLOAD@@': '',
    }
    if 'bool g_problem_park_sync_pending' in source:
        pieces['@@RESET_CACHE@@'] += 'g_problem_park_sync_pending=false;'
    template = (root / 'scripts/testdata/problem_empty_cache.cpp').read_text(
        encoding='utf-8')
    for marker, body in pieces.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-problem-empty-') as directory:
        unit = Path(directory) / 'problem.cpp'
        binary = Path(directory) / 'problem'
        crc = Path(directory) / 'crc.o'
        unit.write_text(template, encoding='utf-8')
        subprocess.run(['gcc', '-I'+str(args.idf_path/'components/esp_rom/include'),
                        '-c', str(args.idf_path/'components/esp_rom/linux/esp_rom_crc.c'),
                        '-o', str(crc)],check=True,timeout=30)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        *(['-DPROBLEM_EVIDENCE_API=1'] if 'struct OutboxReadProof {' in source else []),
                        '-I'+str(args.idf_path/'components/esp_rom/include'),
                        '-fno-builtin', str(unit), str(crc), '-o', str(binary),
                        '-Wl,--wrap=fopen,--wrap=fclose,--wrap=fwrite,--wrap=rename,--wrap=remove,--wrap=fputc,--wrap=fflush,--wrap=fsync'],
                       check=True, timeout=30)
        return subprocess.run([str(binary), directory], timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
