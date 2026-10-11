#!/usr/bin/env python3
"""Exercise initial snapshot coalescing with actual page validation/API flow.

Host HTTP/token/storage/readiness seams; no ESP32, TLS, flash or timing proof.
The result/session structs and page extension are extracted from production.
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
    parser.add_argument('--cloud-source', type=Path, default=root / 'main/ui/word_cloud.cpp')
    parser.add_argument('--api-source', type=Path, default=root / 'main/wqn_api.cpp')
    args = parser.parse_args()
    cloud = args.cloud_source.read_text(encoding='utf-8')
    api = args.api_source.read_text(encoding='utf-8')
    store = (root / 'main/word_study_store.cpp').read_text(encoding='utf-8')
    header = (root / 'main/word_study_store.h').read_text(encoding='utf-8')
    ui = (root / 'main/ui/ui_internal.h').read_text(encoding='utf-8')
    protocol = (root / 'components/device_protocol/word_study.cpp').read_text(encoding='utf-8')
    branch = cloud[cloud.index('} else if (request.op == WordCloudOp::kStartSession)'):
                   cloud.index('} else if (request.op == WordCloudOp::kFetchSessionPage)')]
    if branch.count('PersistInitialWordSessionSnapshot(token, &result);') != 1 or \
            'SavePersistedWordSession(' in branch:
        raise ValueError('start branch must invoke coalescer once, not save an intermediate window')
    pieces = {
        '@@STRUCTS@@': section(header, 'struct StoredWordDeckId {', '\n// [ui-gates] Validates'),
        '@@RESULT@@': section(ui, 'enum class WordCloudOp {', '\nstruct WordCloudRequest {') +
            section(ui, 'struct WordCloudResult {', '\nstruct TodoCloudResultReady {'),
        '@@POLICY_NAME@@': section(protocol, 'const char* CandidatePolicyVersionName(',
                                  '\nconst char* ObservationActionName('),
        '@@EXTEND@@': section(store, 'bool WordSessionSnapshotMatches(',
                             '\nesp_err_t LoadPersistedWordSession('),
        '@@CODEC@@': section(store, 'template <typename T>\nvoid AppendScalar(', '\nclass PayloadReader {') +
            section(store, 'bool EncodeSession(', '\nbool DecodeSession('),
        '@@LIMITS@@': '\n'.join(line for line in store.splitlines() if line.startswith((
            'constexpr size_t kMaxSessionPayloadBytes =', 'constexpr size_t kMaxSessionCursorBytes ='))),
        '@@API@@': section(api, 'static esp_err_t FetchWordStudyCandidatePageV1Impl(',
                          '\nesp_err_t SubmitWordStudyObservationV1AtPath('),
        '@@API_LIMITS@@': '\n'.join(line for line in api.splitlines() if line.startswith((
            'constexpr int kHttpTimeoutMs =', 'constexpr int kWordCandidateCoalesceTimeoutMs ='))),
        '@@COALESCE@@': section(cloud, 'bool IsWordSessionInvalidError(',
                               '\n// Rebuilds the note screen'),
    }
    fixture = (root / 'scripts/testdata/word_initial_snapshot.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-initial-snapshot-') as directory:
        unit, binary = Path(directory) / 'initial.cpp', Path(directory) / 'initial'
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
