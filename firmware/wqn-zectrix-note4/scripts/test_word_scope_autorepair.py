#!/usr/bin/env python3
"""Host regression for deck-scope counts and the default-deck auto-repair FSM.

Extracts the PRODUCTION functions verbatim from main/word_app.cpp:

  * EffectiveScopeDeckId / GetScopedCount  -> memoized scope-aware count
    (scope switch + same-size index change invalidation, gap 5).
  * InstallWordDeckCatalog + the whole DefaultDeckRepair state machine
    (Arm / Cancel / Tick / OnSubmitted / OnAck / Blocking) -> the REAL
    auto intent lifecycle: arm, pump gating with backoff, submit, durable
    ACK attribution, failure + retry, manual/scope priority and generation
    cancellation (gaps 1/2/3/5). These are NOT pure ``Should...`` helpers;
    every transition mutates the self-contained DefaultDeckRepairState POD
    and is exercised end-to-end against a mocked WordAppState.

This is a function-level fixture, NOT StorageService concurrency, physical
power loss, the persist worker, SPIFFS or HIL. The actual worker submit and
UI pump are thin glue around this FSM and are reviewed by inspection.
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
    parser.add_argument('--source', type=Path, default=root / 'main/word_app.cpp')
    args = parser.parse_args()
    compiler = shutil.which('g++')
    if compiler is None:
        raise SystemExit('FAIL: g++ unavailable; fixtures were not run')
    source = args.source.read_text(encoding='utf-8')

    # Global anonymous-namespace helpers: they take `const wqn::WordAppState&`
    # with explicit qualification, so they are injected at GLOBAL scope after
    # the fixture's wqn:: mocks are defined.
    counts = section(
        source,
        '// [scope-count] Effective study scope:',
        '\nwqn::WordHomeSelection HomeSelectionFromIndex(size_t index)')

    # InstallWordDeckCatalog + the full auto-repair state machine both live
    # inside `namespace wqn` in production, so they are injected inside the
    # fixture's `namespace wqn { ... }`.
    machine = section(
        source,
        'void InstallWordDeckCatalog(',
        '\nvoid SetDefaultWordDeck(')

    template = (root / 'scripts/testdata/word_scope_autorepair.cpp').read_text(
        encoding='utf-8')
    pieces = {
        '@@WQN@@': machine,
        '@@COUNT@@': counts,
    }
    for marker, body in pieces.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-scope-autorepair-') as directory:
        testdir = Path(directory)
        unit = testdir / 'scope_autorepair.cpp'
        binary = testdir / 'scope_autorepair'
        unit.write_text(template, encoding='utf-8')
        subprocess.run(
            [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
             str(unit), '-o', str(binary)],
            check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
