#!/usr/bin/env python3
"""Run production ADC batches and the synchronous UI battery guard/adapter.

ADC, calibration and RTOS ports are controlled; elapsed values model injected
sample costs and tick phase, not measured ESP32 conversion time or UI latency.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_audio_runtime import function
from test_rtc_timekeep import HEADERS


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    power = (args.source or root / 'main/power_manager.cpp').read_text()
    battery = (root / 'main/ui/battery.cpp').read_text()
    internal = (root / 'main/ui/ui_internal.h').read_text()
    fixture = (root / 'scripts/testdata/power_sampling.cpp').read_text()
    samples = re.findall(r'^constexpr int kBatAdcSamples = [^;]+;', power, re.M)
    if len(samples) != 1:
        raise ValueError('sample count boundary drift')
    curve_start = power.index('struct BatteryCurvePoint {')
    curve_end = power.index('\nstatic_assert(BatteryPercentFromMillivolts', curve_start)
    production = '\n'.join(function(power, signature) for signature in (
        'bool ReadPowerStatus(', 'uint16_t GetBatteryVoltageMv()',
        'bool IsCharging()', 'bool IsUsbPowered()', 'bool IsFullyCharged()',
        'bool IsBatteryVeryLow()', 'void CheckBatteryAfterUserActivity()'))
    replacements = {
        '@@SAMPLES@@': samples[0],
        '@@POLICY@@': function(power, 'constexpr bool ShouldBlockSleepForExternalPower('),
        '@@CURVE@@': power[curve_start:curve_end],
        '@@POWER@@': production,
        '@@BATTERY_TYPE@@': function(internal, 'struct BatteryReading {') + ';',
        '@@LOG_GATE@@': function(battery, 'bool ShouldLogBattery()'),
        '@@BATTERY_ADAPTER@@': function(battery, 'bool ReadBatteryStatus('),
    }
    for marker, body in replacements.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture boundary drift: {marker}')
        fixture = fixture.replace(marker, body)
    # Keep the traced UI call sites explicit. The whole UI reducer is not a
    # timing model; guard and adapter below are executed as production code.
    ui = (root / 'main/device_ui.cpp').read_text()
    assert ui.index('wqn::CheckBatteryAfterUserActivity();') < ui.index('ui_runtime.DispatchButton(')
    for path, signature in [('main/ui/ui_state.cpp', 'void BuildHomeSummary('),
                            ('main/ui/diagnostics.cpp', 'void UpdateSettingsDiagnostics(')]:
        assert 'ReadBatteryStatus(&battery)' in function((root / path).read_text(), signature)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-adc-ui-latency-') as temporary:
        directory = Path(temporary)
        headers = dict(HEADERS)
        headers['driver/gpio.h'] = '#pragma once\nusing gpio_num_t=int;\n'
        headers['driver/i2c_master.h'] += 'using i2c_port_t=int;\n'
        for name, body in headers.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(body)
        (directory / 'fixture.cpp').write_text(fixture)
        for hz in (100, 1000):
            binary = directory / f'power-{hz}'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                f'-DFIXTURE_TICK_RATE_HZ={hz}', '-I', str(directory),
                '-I', str(root / 'main'), '-I', str(root / 'components/power_runtime/include'),
                str(directory / 'fixture.cpp'), '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], timeout=30)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
