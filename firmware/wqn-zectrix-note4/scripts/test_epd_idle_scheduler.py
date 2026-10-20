#!/usr/bin/env python3
"""Production EPD deadline/owner-loop replay with virtual time and hardware ports.

Verifies scheduling, cancellation, lease lifetime and retry policy; optical,
BUSY, multicore races and sleep/power acceptance still require device HIL.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_epd_refresh_windows import function


def section(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-refresh', type=Path)
    args = parser.parse_args()
    display = (root/'components/display_service/display_service.cpp').read_text()
    refresh = (root/'main/ui/ui_refresh.cpp').read_text()
    fixture = (root/'scripts/testdata/epd_idle_scheduler.cpp').read_text()
    # Replay the helper used by the real owner task, not a second scheduler.
    owner = function(refresh, 'void EpdRefreshTask(')
    assert 'WaitForEpdFrameOrIdleMaintenance(&idle_schedule);' in owner
    assert 'ulTaskNotifyTake(pdTRUE, portMAX_DELAY)' not in owner
    legacy = ''
    legacy_test = ''
    if args.baseline_refresh:
        old_owner = function(args.baseline_refresh.read_text(), 'void EpdRefreshTask(')
        old_wait = section(old_owner, '        ESP_LOGD(kTag, "EPD refresh waiting on notify");',
                           '        // Critical section: copy only the signature')
        legacy = '''
std::atomic<bool> g_idle_maintenance_requested{false};
void RunIdleMaintenanceIfStillValid() { RunIdleMaintenanceIfStillValid(wqn::GetEpdActivityGeneration()); }
void LegacyWaitForEpdFrame() {
''' + old_wait + '\n}\n'
        legacy_test = '''
    Reset();g_heavy_partials_since_full=kIdleCleanupHeavyPartials;
    At(60100,Frame);LegacyWaitForEpdFrame();
    CHECK(cuts.empty()&&cleans.empty());
    CHECK(waits==std::vector<TickType_t>({portMAX_DELAY}));
'''
    pieces = {
        '@@BASELINE@@': legacy,
        '@@BASELINE_TEST@@': legacy_test,
        '@@DEBT_LIMITS@@': '\n'.join(re.search(r'constexpr uint32_t '+name+r' = [^;]+;', display).group() for name in ['kIdleCleanupFramePartials', 'kIdleCleanupHeavyPartials']),
        '@@DRIVER@@': '\n'.join(function(display, name) for name in ['static bool IdleCleanupPending(', 'static void RunIdleEpdCleanupIfNeeded(', 'int64_t EpdIdleMaintenanceDelayMs(']),
        '@@SLEEP_PREP@@': function(display, 'bool wqn::ServiceDisplaySleepPrepCommand('),
        '@@GLOBAL_DRIVER@@': '\n'.join(function(display, name) for name in ['void wqn::NoteEpdActivity(', 'uint32_t wqn::GetEpdActivityGeneration(', 'void wqn::PowerOffEpdAfterIdleIfNeeded(']),
        '@@TICKS@@': function(refresh, 'bool TickReached(') + '\n' + function(refresh, 'TickType_t TicksUntil('),
        '@@SCHEDULER@@': section(refresh, 'void RunIdleMaintenanceIfStillValid(', '\nvoid EpdRefreshTask('),
    }
    for marker, body in pieces.items():
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-epd-idle-') as temp:
        unit = Path(temp)/'idle.cpp'
        unit.write_text(fixture)
        for rate in [100, 1000]:
            binary = Path(temp)/f'idle-{rate}'
            subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            f'-DconfigTICK_RATE_HZ={rate}', str(unit), '-o', str(binary)], check=True, timeout=45)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    main()
