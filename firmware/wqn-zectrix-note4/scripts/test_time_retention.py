#!/usr/bin/env python3
"""Run full timer and retained-state sources with controlled wall/monotonic time.

RTC memory is ordinary host memory; UI/hardware header dependencies are ports.
This does not prove RTC retention across physical resets or real SNTP timing.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--retention-source', type=Path)
    parser.add_argument('--mutation', choices=('wall-elapsed', 'spend-paused',
                                               'keep-action', 'skip-checksum'))
    args = parser.parse_args()
    timer = (root / 'main/time_app.cpp').read_text()
    retention = (args.retention_source or root / 'main/ui/time_app_retention.cpp').read_text()
    if args.mutation == 'wall-elapsed':
        timer = timer.replace('std::max(0, state->remaining_seconds - elapsed_seconds)',
            'static_cast<int>(std::max<int64_t>(0, state->phase_ends_unix_seconds - now_unix_seconds))')
    elif args.mutation == 'spend-paused':
        retention = retention.replace('restored.status == wqn::TimerStatus::kRunning &&',
                                     'restored.status != wqn::TimerStatus::kIdle &&')
    elif args.mutation == 'keep-action':
        retention = retention.replace('    wqn::DisarmTimeAppAction(&restored);',
                                     '    restored.action_armed = true;')
    elif args.mutation == 'skip-checksum':
        retention = retention.replace('retained.checksum == RetainedTimeAppChecksum(retained)', 'true')
    constants = re.findall(r'^constexpr std::time_t kMinReasonableUnixTime = [^;]+;',
                           (root / 'main/ui/ui_internal.h').read_text(), re.M)
    if len(constants) != 1:
        raise ValueError('wall-clock bound drift')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-time-retention-') as temporary:
        directory = Path(temporary)
        (directory / 'esp_attr.h').write_text('#pragma once\n#define RTC_DATA_ATTR\n')
        (directory / 'esp_timer.h').write_text('#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n')
        (directory / 'ui_internal.h').write_text('''#pragma once
#include <cstdio>
#include <ctime>
#include "time_app.h"
namespace device_ui_internal {
''' + constants[0] + '''
void RetainTimeAppState(const wqn::TimeAppState&);
bool RestoreRetainedTimeApp(wqn::TimeAppState*);
bool HasRetainedTimeApp();
}
''')
        (directory / 'timer.cpp').write_text(timer)
        (directory / 'retention.cpp').write_text(retention)
        (directory / 'fixture.cpp').write_text(
            (root / 'scripts/testdata/time_retention.cpp').read_text())
        binary = directory / 'time-retention'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(directory), '-I', str(root / 'main'),
            str(directory / 'timer.cpp'), str(directory / 'fixture.cpp'),
            '-Wl,--wrap=time', '-o', str(binary)], check=True, timeout=60)
        return subprocess.run([str(binary)], timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
