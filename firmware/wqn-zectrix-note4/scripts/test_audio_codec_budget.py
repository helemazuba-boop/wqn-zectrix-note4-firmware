#!/usr/bin/env python3
"""Run complete production codec configuration and register/retry/cleanup paths.

Lifecycle, resource registry, GPIO, SDK and RTOS ports are controlled. This
checks whole-call costs and ownership decisions, not audio/electrical HIL.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_audio_runtime import function
from test_rtc_timekeep import HEADERS, replace_once


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--happy-only', action='store_true')
    parser.add_argument('--mutation', choices=('infinite-locks', 'renew-register-budget',
        'skip-cleanup', 'accept-late-cache', 'skip-session-guard', 'hold-backoff',
        'retry-after-cleanup-error', 'deny-admitted-cleanup'))
    args = parser.parse_args()
    source = (args.source or root / 'main/services/audio_service.cpp').read_text()
    if args.mutation == 'infinite-locks':
        source = re.sub(r', budget\.lock_ticks\(\)\)', ')', source)
    elif args.mutation == 'renew-register-budget':
        source = replace_once(source,
            'result = WriteAudioCodecRegisterBefore(\n            session, codec, reg, value, budget);',
            'result = wqn::services::WriteAudioCodecRegister(\n            session, codec, reg, value);')
    elif args.mutation == 'skip-cleanup':
        source = replace_once(source, '        if (codec != nullptr) {',
                               '        if (codec != nullptr && !budget.expired()) {')
    elif args.mutation == 'accept-late-cache':
        source = replace_once(source, '''        // It must finish before this private handle leaves scope.
        if (budget.expired()) return ESP_ERR_TIMEOUT;''',
        '        // It must finish before this private handle leaves scope.')
    elif args.mutation == 'skip-session-guard':
        source = replace_once(source, '''    DriverOperation operation(session);
    if (!operation) return ESP_ERR_INVALID_STATE;
    const CodecDeadline budget''', '    const CodecDeadline budget')
    elif args.mutation == 'hold-backoff':
        source = source.replace('        if (!budget.delay(kCodecI2cRetryDelay))',
            '        ScopedI2cBusLock extra_backoff_lock("bad_backoff", budget.lock_ticks());\n'
            '        if (!budget.delay(kCodecI2cRetryDelay))')
    elif args.mutation == 'retry-after-cleanup-error':
        source = replace_once(source, '''            if (remove_result != ESP_OK) {
                ESP_LOGE(kTag,''', '''            if (result == ESP_OK) {
                result = remove_result;
            } else if (remove_result != ESP_OK) {
                ESP_LOGE(kTag,''')
    elif args.mutation == 'deny-admitted-cleanup':
        source = replace_once(source, 'RemoveAudioCodecInternal(session, &codec, true);',
                               'RemoveAudioCodecInternal(session, &codec, false);')

    constants = []
    wanted = {'kCallTimeoutUs', 'kEs8311Address', 'kCodecI2cClockHz', 'kCodecI2cMaxAttempts',
        'kCodecI2cRetryDelay', 'kCodecClockSwitchSettle', 'kCodecResetSettle',
        'kCodecProgramRetryDelay', 'kCodecProgramMaxAttempts', 'kMaxCodecHandles',
        'kMicGainIndex', 'kMicGainDb', 'kDacMuteBits', 'kDacVolumeZeroDb'}
    for line in source.splitlines():
        m = re.match(r'constexpr \w+ (\w+) = .+;', line)
        if m and (m[1] in wanted or m[1].startswith('kReg')):
            constants.append(line)
    names = ['CodecProfileName', 'CodecProfileHasOutput', 'PercentToCodecVolumeRegister',
             'RecoverCodecI2cBusLocked']
    return_types = ['const char*', 'bool', 'uint8_t', 'esp_err_t']
    internals = [function(source, f'{ret} {name}(') for ret, name in zip(return_types, names)]
    for name in ('AddAudioCodecBefore', 'WriteAudioCodecRegisterBefore', 'ReadAudioCodecRegisterBefore'):
        if f'esp_err_t {name}(' in source:
            internals.append(function(source, f'esp_err_t {name}('))
    if 'esp_err_t RemoveAudioCodecInternal(' in source:
        internals.append(function(source, 'esp_err_t RemoveAudioCodecInternal('))
    internals.append(function(source, 'esp_err_t RunEs8311RegisterProgram('))
    public = '\n'.join(function(source, f'esp_err_t {name}(') for name in
        ('AddAudioCodec', 'RemoveAudioCodec', 'WriteAudioCodecRegister',
         'ReadAudioCodecRegister', 'ConfigureAudioCodec'))
    fixture = (root / 'scripts/testdata/audio_codec_budget.cpp').read_text()
    close_helpers = '\n'.join(function(source, f'{ret} {name}(') for ret, name in
        [('esp_err_t', 'BeginCloseResource'), ('esp_err_t', 'WaitForResourceDrain'),
         ('void', 'FinishCloseResource')])
    for marker, body in {'@@CONSTANTS@@': '\n'.join(constants),
        '@@RESOURCE_SLOT@@': function(source, 'struct ResourceSlot {') + ';',
        '@@CLOSE_HELPERS@@': close_helpers,
        '@@RETRYABLE@@': function(source, 'bool IsRetryableCodecI2cError('),
        '@@INTERNALS@@': '\n'.join(internals), '@@PUBLIC@@': public}.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture boundary drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-codec-budget-') as temporary:
        directory = Path(temporary)
        headers = dict(HEADERS)
        headers['driver/gpio.h'] = '''#pragma once
using gpio_num_t=int;
constexpr int GPIO_NUM_47=47, GPIO_NUM_48=48;
int gpio_get_level(int);
'''
        headers['driver/i2c_master.h'] += '''
esp_err_t i2c_master_probe(void*, uint16_t, int);
esp_err_t i2c_master_bus_rm_device(void*);
esp_err_t i2c_master_bus_reset(void*);
'''
        headers['driver/i2c_master.h'] = headers['driver/i2c_master.h'].replace(
            'uint32_t scl_speed_hz=0;', 'uint32_t scl_speed_hz=0;\n    uint32_t scl_wait_us=0;')
        for name, body in headers.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(body)
        unit = directory / 'fixture.cpp'
        unit.write_text(fixture)
        for hz in (100, 1000):
            binary = directory / f'codec-{hz}'
            flags = ['-DFIXTURE_HAPPY_ONLY'] if args.happy_only else []
            if 'bool previously_admitted' in source:
                flags.append('-DFIXTURE_ADMITTED_CLOSE')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                f'-DFIXTURE_TICK_RATE_HZ={hz}', *flags, '-I', str(directory),
                '-I', str(root / 'main'), '-I', str(root / 'components/power_runtime/include'),
                str(unit), str(root / 'main/i2c_bus_lock.cpp'), '-o', str(binary)],
                check=True, timeout=60)
            result = subprocess.run([str(binary)], timeout=30)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
