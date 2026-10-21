#!/usr/bin/env python3
"""Run production word-chain/start/batch reducers and the event-loop pump.

Cloud queue, card/flash I/O and rendering are host seams, not HIL. The wiring
gate checks the real loop releases the old cloud owner before pumping starts.
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
    parser.add_argument('--cloud', type=Path, default=root / 'main/ui/word_cloud.cpp')
    parser.add_argument('--loop', type=Path, default=root / 'main/device_ui.cpp')
    parser.add_argument('--input', type=Path, default=root / 'main/ui/ui_input.cpp')
    parser.add_argument('--runtime', type=Path, default=root / 'main/ui/ui_runtime.cpp')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    header = (root / 'main/word_app.h').read_text(encoding='utf-8')
    store = (root / 'main/word_study_store.h').read_text(encoding='utf-8')
    cloud = args.cloud.read_text(encoding='utf-8')
    loop = args.loop.read_text(encoding='utf-8')
    inputs = args.input.read_text(encoding='utf-8')
    runtime = args.runtime.read_text(encoding='utf-8')
    pieces = {
        '@@POLICY@@': (root / 'main/word_batch_policy.h').read_text(encoding='utf-8').replace('#pragma once', ''),
        '@@ENUMS@@': section(header, 'enum class WordAppMode :', '\n// One word the user failed'),
        '@@STORE@@': section(store, 'struct StoredWordDeckId {', '\n// [ui-gates] Validates'),
        '@@OBSERVATION@@': section(store, 'struct DurableWordObservation {', '\nstruct WordOutboxSnapshot {'),
        '@@STATE@@': section(header, 'struct WordReplayEntry {', '\n// One mounted deck'),
        '@@CHAIN@@': section(source, 'void StartSequentialWalk(', '\n// Long-press on a card:'),
        '@@REQUEST@@': section(source, 'bool TakeWordSessionStartRequest(', '\nvoid CancelWordSessionStartResult('),
        '@@CANCEL@@': section(source, 'void CancelWordSessionStartResult(', '\nvoid ResetWordSessionForServerInvalid('),
        '@@BUFFER@@': section(source, 'bool HasBufferedWordObservations(', '\nbool BufferWordObservationEffect('),
        '@@BATCH_RESULT@@': section(source, 'bool ApplyWordObservationBatchResult(', '\nbool SameWordCardPrefetchIndex('),
        '@@FLUSH@@': section(runtime, 'void UiRuntime::RequestWordBatchFlush(', '\nbool UiRuntime::TakeWordCardPrefetchEntry('),
    }
    if 'bool UiRuntime::TakeWordSessionStartRequest(' in runtime:
        pieces['@@RUNTIME@@'] = section(runtime, 'bool UiRuntime::TakeWordSessionStartRequest(', '\nbool UiRuntime::TakeWordCandidatePageRequest(')
    else:
        pieces['@@RUNTIME@@'] = '''bool UiRuntime::TakeWordSessionStartRequest(wqn::protocol::word_study_v1::CreateSessionRequest* request) {
    return wqn::TakeWordSessionStartRequest(&state_.word_app, request);
}
void UiRuntime::RestoreWordSessionStartRequest() {wqn::RestoreWordSessionStartRequest(&state_.word_app);}
'''
    # An old tree has no pump/restore. Explicit no-op seams let the same
    # behavioural cases go red, rather than merely failing compilation.
    if 'void RestoreWordSessionStartRequest(' not in source:
        pieces['@@REQUEST@@'] += '\nvoid RestoreWordSessionStartRequest(WordAppState*) {}\n'
    if 'void PumpWordSessionStart(' in cloud:
        pieces['@@PUMP@@'] = section(cloud, 'void PumpWordSessionStart(', '\nvoid PumpWordCardPrefetch(')
    else:
        pieces['@@PUMP@@'] = 'void PumpWordSessionStart(UiRuntime*) {}\n'
    calls = loop.count('PumpWordSessionStart(&ui_runtime);')
    release = loop.find('if (word_cloud_completed) {')
    pump = loop.find('PumpWordSessionStart(&ui_runtime);')
    candidates = loop.find('PumpWordCandidatePrefetch(&ui_runtime);')
    wired = calls == 1 and 0 <= release < pump < candidates
    print(('PASS' if wired else 'FAIL') + ': loop pumps starts after old cloud completion and before candidate prefetch', flush=True)
    button_only_removed = 'QueueWordSessionStart(' not in inputs and 'TakeWordSessionStartRequest(' not in inputs
    print(('PASS' if button_only_removed else 'FAIL') + ': button reducer is not a second session-start dispatcher', flush=True)
    fixture = (root / 'scripts/testdata/word_session_start.cpp').read_text(encoding='utf-8')
    for marker, body in pieces.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-session-start-') as directory:
        unit, binary = Path(directory) / 'start.cpp', Path(directory) / 'start'
        unit.write_text(fixture, encoding='utf-8')
        (Path(directory) / 'esp_err.h').write_text('#pragma once\nusing esp_err_t = int;\n', encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+directory, '-I'+str(root / 'components/device_protocol/include'),
            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host-fixture"',
            '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host-fixture"',
            str(unit), '-o', str(binary)], check=True, timeout=30)
        result = subprocess.run([str(binary)], timeout=10).returncode
    return 0 if result == 0 and wired and button_only_removed else 1


if __name__ == '__main__':
    raise SystemExit(main())
