#!/usr/bin/env python3
"""Run the production upload loop against host transport/storage fault seams.

No network, flash, credentials or device are accessed. This does not prove HIL
durability; the journal implementation has separate replay/libc tests.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/services/sync_service.cpp')
    parser.add_argument('--mutation', choices=('skip-empty-maintenance',))
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    start = 'WordOutboxUploadState UploadPendingWordObservations('
    end = '// Uploads pending problem verdicts'
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit('FAIL: production upload boundary drift')
    body = source[source.index(start):source.index(end, source.index(start))]
    if args.mutation == 'skip-empty-maintenance':
        needle = 'wqn::MaintainWordObservationOutbox(\n            maintenance_gate, &maintenance_deferred)'
        if body.count(needle) != 1:
            raise SystemExit('FAIL: maintenance retry mutation boundary drift')
        body = body.replace(needle, 'ESP_OK')
    fixture = (root / 'scripts/testdata/word_ack_upload.cpp').read_text(encoding='utf-8')
    header = (root / 'main/word_study_store.h').read_text(encoding='utf-8')
    gate_start = 'struct WordOutboxMaintenanceGate {'
    gate_end = '// Moves one permanently rejected observation'
    if header.count(gate_start) != 1 or header.count(gate_end) != 1 or fixture.count('@@MAINTENANCE_GATE@@') != 1:
        raise SystemExit('FAIL: production maintenance API boundary drift')
    fixture = fixture.replace('@@MAINTENANCE_GATE@@', header[header.index(gate_start):header.index(gate_end)])
    if fixture.count('@@UPLOAD@@') != 1:
        raise SystemExit('FAIL: fixture marker drift')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-ack-upload-') as directory:
        unit, binary = Path(directory) / 'upload.cpp', Path(directory) / 'upload'
        unit.write_text(fixture.replace('@@UPLOAD@@', body), encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
