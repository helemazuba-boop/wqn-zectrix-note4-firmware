#!/usr/bin/env python3
"""Exercise production session replay/checkpoint functions with host I/O mocks.

These fixtures prove replay ordering and error propagation, not SPIFFS/NVS
durability, StorageService concurrency or physical power-loss recovery.
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
    parser.add_argument('--source', type=Path,
                        default=root / 'main/word_study_store.cpp')
    parser.add_argument('--mutation', choices=('disable-interaction-gate', 'gate-forced-repair'))
    args = parser.parse_args()
    compiler = shutil.which('g++')
    if compiler is None:
        raise SystemExit('FAIL: g++ unavailable; fixtures were not run')
    source = args.source.read_text(encoding='utf-8')
    if args.mutation == 'disable-interaction-gate':
        needle = 'context->maintenance_gate.should_defer != nullptr &&'
        if source.count(needle) != 1:
            raise SystemExit('FAIL: interaction gate mutation boundary drift')
        source = source.replace(needle, 'false && ' + needle)
    elif args.mutation == 'gate-forced-repair':
        needle = 'if (!context->force_compact && !context->for_sleep &&'
        if source.count(needle) != 1:
            raise SystemExit('FAIL: mandatory maintenance mutation boundary drift')
        source = source.replace(needle, 'if (!context->for_sleep &&')
    header = (root / 'main/word_study_store.h').read_text(encoding='utf-8')
    pieces = {
        '@@MAINTENANCE_GATE@@': section(header, 'struct WordOutboxMaintenanceGate {',
            '// Moves one permanently rejected observation'),
        '@@MAINTENANCE_THRESHOLD@@': next(line for line in source.splitlines()
            if line.startswith('constexpr size_t kRuntimeCompactAckThreshold =')),
        '@@BATCH_ENABLED@@': (
            '#define FIXTURE_BATCH ' + str(int('struct CommitBatchContext {' in source)) +
            '\nconstexpr size_t kOutboxAppendBatchCapacity = 10;\n'),
        '@@SESSION_GENERATION@@': next(
            (line for line in source.splitlines()
             if line.startswith('uint64_t g_session_mutation_generation =')),
            'uint64_t g_session_mutation_generation = 0;'),
        '@@STEPPED_MAINTENANCE@@': (
            '#define FIXTURE_STEPPED_MAINTENANCE ' +
            str(int('struct OutboxMaintenanceContext {' in source))),
        '@@OUTBOX_QUOTA@@': (
            '#define FIXTURE_OUTBOX_QUOTA 1\n' + section(
                source, 'constexpr size_t kOutboxMaxRecords =',
                '\n// [measure] §五之十 §6. `open_ms`')
            if 'constexpr size_t kOutboxMaxRecords =' in source else
            '#define FIXTURE_OUTBOX_QUOTA 0\n'),
        '@@MODES@@': section(source,
                            'constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes[] = {',
                            '\n#pragma pack(push, 1)'),
        '@@RAW_SAVE@@': section(source, 'esp_err_t SaveSessionRaw(',
                               '// Small mutable companion to the per-mode session snapshot.'),
        '@@RECONCILE@@': section(source, 'bool SetSessionCursorOrdinal(',
                                 '\nesp_err_t CheckpointSessionFromOutbox('
                                 if 'esp_err_t CheckpointSessionFromOutbox(' in source
                                 else '\nesp_err_t CheckpointSessionsFromOutbox('),
        '@@CHECKPOINT@@': section(source,
                                 'esp_err_t CheckpointSessionFromOutbox('
                                 if 'esp_err_t CheckpointSessionFromOutbox(' in source
                                 else 'esp_err_t CheckpointSessionsFromOutbox(',
                                 '\nesp_err_t MaybeCompactCachedOutbox('),
        '@@RUNTIME@@': section(source, 'esp_err_t MaybeCompactCachedOutbox(',
                              '\nstruct LoadSessionContext {'),
        '@@LOAD@@': section(source, 'struct LoadSessionContext {',
                            '\nesp_err_t SaveSessionTransaction('),
        '@@SAVE@@': section(source, 'esp_err_t SaveSessionTransaction(',
                            '\nesp_err_t SaveCursorTransaction('),
        '@@COMMIT@@': section(source, 'struct CommitContext {',
                              '\nesp_err_t PeekObservationTransaction('),
        '@@ACK@@': section(source, 'struct AckContext {', '\nstruct QuarantineContext {'),
        '@@TERMINAL@@': section(source, 'struct QuarantineContext {',
                                '\nesp_err_t SnapshotTransaction('),
        '@@MAINTENANCE_DRIVER@@': (
            section(source, 'template <typename Transaction>\nesp_err_t ExecuteWithStorageLease(',
                    '\n\n}  // namespace\n\nnamespace wqn {')
            if 'esp_err_t RunOutboxMaintenance(' in source else ''),
        '@@PUBLIC_MAINTENANCE@@': (
            'namespace wqn {\n' + section(
                source, 'esp_err_t AcknowledgeWordObservation(',
                '\nesp_err_t ReadWordOutboxSnapshot(') + section(
                source, 'esp_err_t PrepareWordObservationOutboxForSleep(',
                '\n}  // namespace wqn\n\n// [measure] Temporary bench starter.') + '\n}\n'
            if 'esp_err_t RunOutboxMaintenance(' in source else ''),
    }
    template = (root / 'scripts/testdata/word_session_replay.cpp').read_text(
        encoding='utf-8')
    for marker, body in pieces.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-session-replay-') as directory:
        unit = Path(directory) / 'replay.cpp'
        binary = Path(directory) / 'replay'
        unit.write_text(template, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
