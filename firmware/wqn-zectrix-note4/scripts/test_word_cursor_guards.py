#!/usr/bin/env python3
"""Compile production cursor functions against host-only I/O fault fixtures.

This tests validation, migration ordering and error propagation, NOT NVS flash
durability, StorageService concurrency, physical power loss or upgrade HIL.
The production functions are extracted verbatim, not reimplemented in Python.
"""

import argparse
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
    parser.add_argument('--source', type=Path, default=root / 'main/word_study_store.cpp')
    args = parser.parse_args()
    compiler = shutil.which('g++')
    if compiler is None:
        raise SystemExit('FAIL: g++ unavailable; fixtures were not run')
    source = args.source.read_text(encoding='utf-8')
    pieces = {
        '@@SESSION_GENERATION@@': next(
            (line for line in source.splitlines()
             if line.startswith('uint64_t g_session_mutation_generation =')),
            'uint64_t g_session_mutation_generation = 0;'),
        '@@CRC@@': section(source, 'uint32_t Crc32(const void* bytes, size_t size)',
                           '\nbool CopyFixedText('),
        '@@RECORD@@': section(source, 'constexpr uint32_t kSessionCursorMagic =',
                              '// [word-session-cursor-nvs] NVS keys'),
        '@@READ@@': section(source, 'bool ReadSessionCursorPaused(',
                            '\nesp_err_t LoadSessionFile('),
        '@@WRITE@@': section(source, 'esp_err_t WriteSessionCursor(',
                             '\nbool ReadSessionCursorPaused('),
        '@@CLEAR@@': section(source, 'struct ClearSessionContext {', '\nstruct CommitContext {'),
    }
    template = (root / 'scripts/testdata/word_cursor_guards.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    # Generated compilation units and all test files stay in a unique temporary
    # directory. No real /storage paths or production NVS keys are accessed.
    with tempfile.TemporaryDirectory(prefix='wqn-cursor-guards-') as directory:
        testdir = Path(directory)
        unit = testdir / 'guards.cpp'
        binary = testdir / 'guards'
        unit.write_text(template, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary), directory], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
