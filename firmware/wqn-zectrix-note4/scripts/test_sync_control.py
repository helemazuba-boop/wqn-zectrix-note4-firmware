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
    parser.add_argument('--mutation', choices=('mutable-sync-metadata', 'publish-before-commit'))
    args = parser.parse_args()
    sync = (root / 'main/services/sync_service.cpp').read_text()
    store = (root / 'main/storage.cpp').read_text()
    boundaries = [
        (sync, 'class SyncJournalLock {', '\nsize_t ContentDomainIndex('),
        (sync, 'size_t ContentDomainIndex(', '\nstd::time_t CurrentUnixSeconds()'),
        (sync, 'uint64_t DurableRetryDeadline(', '\nbool PeriodicScheduleDue('),
        (sync, 'void ClearFullSyncRetry()', '\nbool BootFullSyncDue('),
        (sync, 'wqn::services::SyncContentSnapshot* ContentSnapshotForKind(', '\nvoid WordOutboxTimerCallback('),
        (sync, 'std::string g_bootstrap_request_id;', '\nwqn::protocol::v3::ClaimKeyPair'),
        (sync, 'void CopyOutboxRetryCheckpoint(', '\nvoid SetOutboxProtocolSuspended('),
        (sync, 'esp_err_t EnsureControlStateLoaded()', '\nstd::string RandomControlId('),
        (sync, 'wqn::protocol::v3::RequestMetadata MakeControlMetadata()', '\nstd::string DeviceHardwareId('),
        (store, 'esp_err_t JsonToString(', '\nesp_err_t ReadStorageTextFile('),
        (store, 'void AddJournalContentState(', '\nbool ReadJournalU64('),
        (store, 'esp_err_t SaveSyncJournalRaw(', '\nesp_err_t SaveSyncJournalTransaction('),
        (sync, 'esp_err_t BootstrapControlV3(', '\n#endif\n\n#if CONFIG_WQN_DEVICE_CONTROL_V3_ENABLE\nWordOutboxUploadState UploadPendingWordObservations('),
    ]
    parts = [section(*item) for item in boundaries]
    public = section(sync, 'esp_err_t BeginContentInstall(', '\nvoid NoteWordInteraction(')
    parts.append('namespace wqn::services {\n' + public + '\n}')
    fixture = (root / 'scripts/testdata/sync_control.cpp').read_text()
    configs = {
        '@@RETRY_CONFIG@@': section(sync, 'constexpr uint32_t kFullSyncRetryLadderMs[] =', '// RTC retention'),
        '@@OUTBOX_RETRY_CONFIG@@': section(sync, 'constexpr uint32_t kWordOutboxRetryBaseMs =', '// Retry ladder reaches'),
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
                        '-x', 'c++', '-', str(root/'components/device_protocol/v3.cpp'),
                        str(cjson/'cJSON.c'), '-o', str(binary)],
                       input=fixture, text=True, check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
