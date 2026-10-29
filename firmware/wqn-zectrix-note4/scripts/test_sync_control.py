#!/usr/bin/env python3
"""Production control/journal host fault seams. NOT physical power-loss HIL."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_sync_retry import section


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('mutable-sync-metadata', 'publish-before-commit',
                                               'drop-local-retry', 'early-completion', 'cache-failed-write',
                                               'replay-successful-rpc', 'no-gap-escalation',
                                               'forget-gap-session', 'drop-restored-attempt',
                                               'retire-other-session'))
    args = parser.parse_args()
    sync = (root / 'main/services/sync_service.cpp').read_text()
    store = (root / 'main/storage.cpp').read_text()
    word = (root / 'main/word_study_store.h').read_text()
    dispatch = section(sync, 'void SyncServiceTask(', '\n#endif  // CONFIG_WQN_WIFI_STA_ENABLE')
    if dispatch.index('RetryCachedControlResponseIfDue();') >= dispatch.index('AcquireConnectivityDemand('):
        raise SystemExit('FAIL: local control commit is behind connectivity admission')
    boundaries = [
        (sync, 'class SyncJournalLock {', '\nsize_t ContentDomainIndex('),
        (sync, 'size_t ContentDomainIndex(', '\nstd::time_t CurrentUnixSeconds()'),
        (sync, 'uint64_t DurableRetryDeadline(', '\nbool PeriodicScheduleDue('),
        (sync, 'void ClearFullSyncRetry()', '\nbool BootFullSyncDue('),
        (sync, 'wqn::services::SyncContentSnapshot* ContentSnapshotForKind(', '\nvoid WordOutboxTimerCallback('),
        (sync, 'std::string g_bootstrap_request_id;', '\nwqn::protocol::v3::ClaimKeyPair'),
        (sync, 'void CopyOutboxRetryCheckpoint(', '\nvoid SetOutboxProtocolSuspended('),
        (sync, 'void SetOutboxProtocolSuspended(', '\nvoid ResetWordOutboxRetryBackoff('),
        (sync, 'esp_err_t EnsureControlStateLoaded()', '\nstd::string RandomControlId('),
        (sync, 'wqn::protocol::v3::RequestMetadata MakeControlMetadata()', '\nstd::string DeviceHardwareId('),
        (store, 'esp_err_t JsonToString(', '\nesp_err_t ReadStorageTextFile('),
        (store, 'bool SyncJournalHasUnrepresentableStrings(', '\nesp_err_t ParseSyncJournalPayload(const std::string& payload, wqn::SyncJournal* journal);'),
        (store, 'void AddJournalContentState(', '\nesp_err_t RemovePrototypeProblemFile('),
        (store, 'std::string g_sync_journal_durable_payload;', '\nstruct SyncJournalSaveContext'),
        (sync, 'TickType_t SchedulerWaitDelay()', '\nuint32_t FullSyncFailureRetryMs('),
        (sync, 'esp_err_t BootstrapControlV3(', '\n#endif\n\n#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE\nWordOutboxUploadState UploadPendingWordObservations('),
    ]
    parts = [section(*item) for item in boundaries]
    parts.append(section(sync, 'enum class WordOutboxUploadState :', '\nenum class OutboxRetryCause'))
    parts.append(section(sync, 'enum class OutboxFailureDisposition :', '\nconst char* OutboxRetryCauseName('))
    for domain in ('Word', 'Note', 'Problem'):
        parts.append(section(sync, f'void Reset{domain}OutboxRetryBackoff()',
                             f'\nTickType_t {domain}OutboxRetryWaitDelay()'))
    parts.append(section(sync, 'WordOutboxUploadState UploadPendingWordObservations(',
                         '// Uploads pending problem verdicts'))
    public = section(sync, 'SyncContentTicket TryClaimContentRefresh(', '\nvoid NoteWordInteraction(')
    parts.append('namespace wqn::services {\n' + public + '\n}')
    fixture = (root / 'scripts/testdata/sync_control.cpp').read_text()
    configs = {
        '@@RETRY_CONFIG@@': section(sync, 'constexpr uint32_t kFullSyncRetryLadderMs[] =', '// RTC retention'),
        '@@OUTBOX_RETRY_CONFIG@@': section(sync, 'constexpr uint32_t kWordOutboxRetryBaseMs =', '// Retry ladder reaches'),
        '@@GAP_CONFIG@@': section(sync, 'constexpr uint8_t kWordOutboxSequenceGapEscalation =', '\nstd::string g_bootstrap_request_id;'),
        '@@WORD_TYPES@@': 'namespace wqn {\n' + section(word, 'enum class WordPresentationPhase :', '\ntemplate <typename T>') +
            section(word, 'struct DurableWordObservation {', '\nstruct WordOutboxSnapshot {') +
            section(word, 'struct WordOutboxMaintenanceGate {', '// Moves one permanently rejected observation') + '\n}',
        '@@WORD_PORTS@@': (root / 'scripts/testdata/sync_word_gap_ports.cpp').read_text(),
        '@@WORD_CASES@@': (root / 'scripts/testdata/sync_word_gap_cases.cpp').read_text(),
        '@@PRODUCTION@@': '\n'.join(parts),
    }
    for marker, value in configs.items():
        if fixture.count(marker) != 1:
            raise SystemExit(f'FAIL: fixture drift: {marker}')
        fixture = fixture.replace(marker, value)
    if args.mutation == 'mutable-sync-metadata':
        needle = 'token, g_sync_request_metadata, g_sync_request_auto_interval_minutes,'
        if fixture.count(needle) != 1:
            raise SystemExit('FAIL: metadata mutation drift')
        fixture = fixture.replace(needle, 'token, MutableSyncMetadata(), g_sync_request_auto_interval_minutes,')
    if args.mutation == 'publish-before-commit':
        needle = '// One authority/one durable publication:'
        if fixture.count(needle) != 1:
            raise SystemExit('FAIL: publication mutation drift')
        fixture = fixture.replace(needle, 'g_config_revision=checkpoint.config_revision; g_sync_cursor=checkpoint.sync_cursor; PublishContentTargets(targets);\n    '+needle)
    for mutation, needle, replacement in (
        ('drop-local-retry', 'else JournalCommitFailedLocked();', 'else {}'),
        ('early-completion', 'g_content_completed_snapshot[index] = completed;',
         '*live_snapshot = completed; g_content_completed_snapshot[index] = completed;'),
        ('cache-failed-write', 'g_sync_journal_durable_payload_valid = false;\n    }',
         'g_sync_journal_durable_payload = payload; g_sync_journal_durable_payload_valid = true;\n    }'),
        ('replay-successful-rpc', 'if (!g_sync_response_ready) {', 'if (true) {'),
        ('no-gap-escalation', 'g_word_outbox_retry_attempts >=\n                           kWordOutboxSequenceGapEscalation', 'false'),
        ('forget-gap-session', 'g_word_outbox_gap_terminal_session_id = pending.session_id;',
         'g_word_outbox_gap_terminal_session_id.clear();'),
        ('drop-restored-attempt', '*attempts = source.attempt;', '*attempts = 0;'),
        ('retire-other-session', 'pending.session_id ==\n                        g_word_outbox_gap_terminal_session_id', 'true'),
    ):
        if args.mutation == mutation:
            if fixture.count(needle) != 1:
                raise SystemExit(f'FAIL: mutation drift: {mutation}')
            fixture = fixture.replace(needle, replacement)
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    if not (cjson / 'cJSON.c').is_file():
        raise SystemExit('FAIL: activate the project ESP-IDF environment for cJSON')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-sync-control-') as tmp:
        binary = Path(tmp) / 'test'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I'+str(root/'scripts/testdata/sync_host'), '-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'), '-I'+str(cjson),
                        '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host-audit-only"',
                        '-DWQN_WORD_STUDY_SCHEMA_SHA256="host-audit-only"',
                        '-x', 'c++', '-', str(root/'components/device_protocol/v3.cpp'),
                        str(cjson/'cJSON.c'), '-o', str(binary)],
                       input=fixture, text=True, check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
