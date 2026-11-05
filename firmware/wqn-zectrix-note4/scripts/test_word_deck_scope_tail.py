#!/usr/bin/env python3
"""Host regression for the default-deck scope-change recoverable tail.

Extracts the two production functions verbatim (ApplyDeckScopeChangeLocked,
RecoverDeckScopeTransaction) from main/storage.cpp and runs them against a
mocked NVS with fault injection at each raw-write boundary. This verifies the
ORDER and IDEMPOTENCY of the new sequential-cursor reset inside the ONE
StorageService transaction, and that a failure at the cursor write or marker
clear leaves the marker present for boot recovery to replay.

This is a function-level fixture, NOT flash durability, StorageService
concurrency, physical power-loss or HIL.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f'production function boundary drift: {start!r}/{end!r}')
    a = source.index(start)
    b = source.index(end, a)
    return source[a:b]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/storage.cpp')
    args = parser.parse_args()
    compiler = shutil.which('g++')
    if compiler is None:
        raise SystemExit('FAIL: g++ unavailable; fixtures were not run')
    source = args.source.read_text(encoding='utf-8')

    # ApplyDeckScopeChangeLocked sits between its signature and the next struct.
    apply = section(
        source,
        'esp_err_t ApplyDeckScopeChangeLocked(uint32_t target_generation, const std::string& deck_id)',
        '\nstruct DeckScopeChangeContext {')

    # RecoverDeckScopeTransaction ends just before the anonymous namespace
    # closes and the next public function begins. Trim that enclosing brace;
    # the function body itself is kept verbatim.
    recover_end = '\n\nesp_err_t ChangeDefaultWordDeckForeground'
    recover = section(
        source,
        'esp_err_t RecoverDeckScopeTransaction(void*)',
        recover_end)
    recover = recover.split('\n}  // namespace')[0]

    template = (root / 'scripts/testdata/word_deck_scope_tail.cpp').read_text(
        encoding='utf-8')
    pieces = {
        '@@APPLY@@': apply,
        '@@RECOVER@@': recover,
    }
    for marker, body in pieces.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-scope-tail-') as directory:
        testdir = Path(directory)
        unit = testdir / 'scope_tail.cpp'
        binary = testdir / 'scope_tail'
        unit.write_text(template, encoding='utf-8')
        subprocess.run(
            [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
             str(unit), '-o', str(binary)],
            check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
