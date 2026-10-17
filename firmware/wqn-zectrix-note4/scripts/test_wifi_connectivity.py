#!/usr/bin/env python3
"""Exercise production WiFi role, retry and handoff paths with host ports; not HIL."""
from pathlib import Path
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1:
        raise ValueError(f'Production boundary changed: {start}')
    first = source.index(start)
    return source[first:source.index(end, first + len(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    service = (root/'main/services/connectivity_service.cpp').read_text()
    header = (root/'main/services/connectivity_service.h').read_text()
    storage = (root/'main/storage.cpp').read_text()
    store_header = (root/'main/storage.h').read_text()
    provision = (root/'main/provision_manager.cpp').read_text()
    definitions = ('namespace wqn::services {\n' + section(header, 'enum class ConnectivityState',
                    'struct ConnectivityDemandTicket') + '}\nnamespace wqn {\n' +
                   section(store_header, 'struct WifiCredentialSlot', '// Outcome of MigrateLegacy') +
                   section(storage, 'void CopyWifiCredentialField(', '\nwqn::WifiCredentialStore EmptyWifiCredentialStore()') + '}\n')
    constants = section(service, 'constexpr char kTag[]', '\nStaticQueue_t g_command_queue_storage;')
    storage_paths = '\n'.join(section(storage, start, end) for start, end in (
        ('esp_err_t UpsertWifiCredential(', '\nesp_err_t SetWifiCredentialForRole('),
        ('esp_err_t SetWifiCredentialForRole(', '\nesp_err_t MarkWifiSlotPreferred('),
        ('esp_err_t MarkWifiSlotPreferred(', '\nesp_err_t LoadWifiCredentials('),
    ))
    functions = '\n'.join(section(service, start, end) for start, end in (
        ('bool HasPendingUserConnection()', '// [wifi-redundancy] Publishes'),
        ('void PublishSnapshotIdentity()', '// [wifi-redundancy] Reloads'),
        ('esp_err_t KickConnectFromStore()', '// [wifi-redundancy] Switches'),
        ('bool TryPivotToBackupSlot()', '\nbool PostAsyncCommand('),
        ('void ClearScheduledAction()\n{', '\nvoid ScheduleAction('),
        ('void ScheduleAction(ScheduledAction action, int64_t delay_us)\n{', '\nint64_t RetryDelayUs('),
        ('int64_t RetryDelayUs(', '\nesp_err_t BeginConnectionAttempt()\n{'),
        ('esp_err_t BeginConnectionAttempt()\n{', '\nvoid ScheduleBackoff(wqn::services::ConnectivityWaitResult failure)\n{'),
        ('void ScheduleBackoff(wqn::services::ConnectivityWaitResult failure)\n{', '\nvoid ResetBackoffEscalation()'),
        ('void ResetBackoffEscalation()', '\nvoid RecordAttemptFailure('),
        ('void RecordAttemptFailure(', '\nesp_err_t BeginProvisioning()'),
        ('esp_err_t StartWithCredentials(', '\nesp_err_t PrepareForSleep('),
        ('esp_err_t HandleCommand(', '\nvoid HandleScheduledAction()'),
    ))
    report = section(provision, 'void ReportProvisioningConnectionResult(', '\n}  // namespace wqn')
    fixture = (root/'scripts/testdata/wifi_connectivity.cpp').read_text()
    for key, value in {'@@DEFINITIONS@@':definitions, '@@CONSTANTS@@':constants,
                       '@@STORAGE@@':storage_paths, '@@REPORT@@':report, '@@FUNCTIONS@@':functions}.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='wqn-wifi-host-') as temporary:
        station = (root/'main/wifi_manager.cpp').read_text()
        driver_fixture = (root/'scripts/testdata/wifi_station.cpp').read_text().replace(
            '@@STATION@@', section(station, 'esp_err_t StartWifiWithCredentials(',
                                    '\nesp_err_t StartWifiStationIfEnabled()'))
        for name, unit in [('connectivity', fixture), ('station', driver_fixture)]:
            binary = Path(temporary)/name
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', '-x', 'c++', '-', '-o', str(binary)],
                           input=unit, text=True, check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
