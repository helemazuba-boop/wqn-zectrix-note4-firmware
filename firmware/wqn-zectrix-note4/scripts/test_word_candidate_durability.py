#!/usr/bin/env python3
"""Run the production candidate reducer/extender with host UI/I/O seams.

Exercises publish-after-durable error paths, not flash, concurrency or HIL.
Session structs and page validation come from production; UI reads/navigation
and RAM-tail identity canaries are host seams, not a full UiRuntime simulation.
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
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    header = (root / 'main/word_study_store.h').read_text(encoding='utf-8')
    store = (root / 'main/word_study_store.cpp').read_text(encoding='utf-8')
    protocol = (root / 'components/device_protocol/word_study.cpp').read_text(encoding='utf-8')
    pieces = {
        '@@STRUCTS@@': section(header, 'struct StoredWordDeckId {', '\n// [ui-gates] Validates'),
        '@@POLICY_NAME@@': section(protocol, 'const char* CandidatePolicyVersionName(',
                                  '\nconst char* ObservationActionName('),
        '@@EXTEND@@': section(store, 'bool WordSessionSnapshotMatches(',
                             '\nesp_err_t LoadPersistedWordSession('),
        '@@REDUCER@@': section(source, 'void ApplyWordCandidatePageResult(',
                              '\nbool TakeWordObservationEffect('),
    }
    fixture = (root / 'scripts/testdata/word_candidate_durability.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-candidate-durable-') as directory:
        unit, binary = Path(directory) / 'candidate.cpp', Path(directory) / 'candidate'
        unit.write_text(fixture, encoding='utf-8')
        (Path(directory) / 'esp_err.h').write_text(
            '#pragma once\nusing esp_err_t = int;\n', encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+directory, '-I'+str(root / 'components/device_protocol/include'),
            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host-fixture"',
            '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host-fixture"',
            str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
