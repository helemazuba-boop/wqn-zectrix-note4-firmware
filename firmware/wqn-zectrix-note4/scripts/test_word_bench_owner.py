#!/usr/bin/env python3
"""Run the actual legacy bench controller/lifecycle through host owner seams.

No experiment is enabled, no device/flash is used, and no timing is inferred.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/word_study_store.cpp')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    start, end = '// Legacy scratch lifecycle must use the same owner', '// [measure] Private scratch objects;'
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('production bench boundary drift')
    body = source[source.index(start):source.index(end)]
    for original, seam in (
            ('std::fopen(', 'TestOpen('), ('std::remove(', 'TestRemove('),
            ('std::fflush(', 'TestFlush('), ('std::fclose(', 'TestClose('),
            ('::fsync(', 'TestSync('), ('fileno(', 'TestFileNo(')):
        body = body.replace(original, seam)
    fixture = (root / 'scripts/testdata/word_bench_owner.cpp').read_text(encoding='utf-8')
    if fixture.count('@@PRODUCTION@@') != 1:
        raise ValueError('fixture marker drift')
    fixture = fixture.replace('@@PRODUCTION@@', body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-bench-owner-') as directory:
        unit, binary = Path(directory) / 'bench.cpp', Path(directory) / 'bench'
        unit.write_text(fixture, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
