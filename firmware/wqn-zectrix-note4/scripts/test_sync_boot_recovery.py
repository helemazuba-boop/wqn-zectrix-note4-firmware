#!/usr/bin/env python3
"""Compile production startup with injected journal/allocation failures. NOT HIL."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_sync_retry import section


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('late-journal-gate', 'ignore-sync-start-failure',
                                              'ignore-schema-recovery', 'ignore-schema-restart'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    boot = (root / 'main/main.cpp').read_text()
    if args.mutation == 'late-journal-gate':
        gate = section(boot, '    const esp_err_t sync_boot_result =',
                       '    ESP_ERROR_CHECK(wqn::services::StartAudioService());')
        boot = boot.replace(gate, '', 1)
        needle = '    ESP_ERROR_CHECK(wqn::StartDeviceUiIfEnabled());'
        boot = boot.replace(needle, needle + '\n' + gate, 1)
    elif args.mutation == 'ignore-sync-start-failure':
        boot = boot.replace('ESP_ERROR_CHECK(wqn::services::StartSyncService());',
                            'ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::services::StartSyncService());', 1)
    elif args.mutation in ('ignore-schema-recovery', 'ignore-schema-restart'):
        action = ('kRecoveryRequired' if args.mutation == 'ignore-schema-recovery'
                  else 'kRestartRequired')
        old = ('(storage_schema.action ==\n'
               '        wqn::runtime::StorageSchemaBootAction::' + action + ')')
        if boot.count(old) != 1:
            raise ValueError('startup schema gate mutation boundary drift')
        boot = boot.replace(old, '(false)', 1)
    sync = (root / 'main/services/sync_service.cpp').read_text()
    fixture = (root / 'scripts/testdata/sync_boot_recovery.cpp').read_text()
    parts = {
        '@@RECOVERY@@': section(boot, '[[noreturn]] void EnterStorageRecoveryMode(',
                                '\n}  // namespace'),
        '@@APP_MAIN@@': boot[boot.index('extern "C" void app_main(void)'):],
        '@@PREPARE@@': section(sync, 'esp_err_t PrepareSyncServiceAtBoot()',
                              '\n#if CONFIG_WQN_WIFI_STA_ENABLE\n\nbool EvaluateSyncWorkAtBoot()'),
        '@@START_SYNC@@': section(sync, 'esp_err_t StartSyncService()',
                                 '\n#endif  // CONFIG_WQN_WIFI_STA_ENABLE'),
    }
    for marker, value in parts.items():
        if fixture.count(marker) != 1:
            raise SystemExit(f'FAIL: fixture drift: {marker}')
        fixture = fixture.replace(marker, value)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-sync-boot-') as temporary:
        for wifi, agent in ((1, 1), (1, 0), (0, 0)):
            binary = Path(temporary) / f'test-wifi{wifi}-agent{agent}'
            subprocess.run([
                compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                f'-DCONFIG_WQN_WIFI_STA_ENABLE={wifi}', f'-DCONFIG_WQN_AGENT_ENABLE={agent}',
                '-x', 'c++', '-', '-o', str(binary)],
                input=fixture, text=True, check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
