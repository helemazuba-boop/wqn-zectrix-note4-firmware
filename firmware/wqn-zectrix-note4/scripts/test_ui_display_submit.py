#!/usr/bin/env python3
"""Host seams around the production submit/merge functions, not EPD hardware."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def extract(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('production display boundary drift')
    return source[source.index(start):source.index(end)]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/ui/ui_refresh.cpp')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    body = extract(source, 'void MergeDisplayPolicy(', 'RefreshSchedule EffectiveSchedule(')
    body += extract(source, 'wqn::display::DisplaySubmission RequestEpdUiRefresh(',
                    'void AcknowledgeDisplayResult(')
    fixture = (root / 'scripts/testdata/ui_display_submit.cpp').read_text(encoding='utf-8')
    if fixture.count('@@PRODUCTION@@') != 1:
        raise ValueError('fixture marker drift')
    fixture = fixture.replace('@@PRODUCTION@@', body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-ui-submit-') as directory:
        unit, binary = Path(directory) / 'submit.cpp', Path(directory) / 'submit'
        unit.write_text(fixture, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
