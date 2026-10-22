#!/usr/bin/env python3
"""Run full RTC/timekeep/lock sources and codec register functions at 100/1000 Hz.

Real public headers are used. Driver, semaphore, clock and wake-context ports
are host substitutes; this is not electrical, scheduler or deep-sleep HIL.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_audio_runtime import function


HEADERS = {
    'esp_err.h': '''#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_TIMEOUT=263;
constexpr int ESP_ERR_INVALID_ARG=258, ESP_ERR_INVALID_STATE=259;
constexpr int ESP_ERR_INVALID_RESPONSE=264;
inline const char* esp_err_to_name(int e) { return e ? "FAULT" : "ESP_OK"; }
''',
    'esp_attr.h': '#pragma once\n#define RTC_DATA_ATTR\n',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'esp_check.h': '#pragma once\n#include "esp_err.h"\n',
    'esp_log.h': '''#pragma once
void FixtureLog(const char*, const char*, ...);
#define ESP_LOGE(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)
''',
    'esp_system.h': '''#pragma once
enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_DEEPSLEEP,
                         ESP_RST_SW, ESP_RST_BROWNOUT };
''',
    'esp_sleep.h': '''#pragma once
enum esp_sleep_wakeup_cause_t { ESP_SLEEP_WAKEUP_UNDEFINED };
''',
    'freertos/FreeRTOS.h': '''#pragma once
#include <cstdint>
#include <cstdlib>
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr BaseType_t pdTRUE=1, pdFALSE=0;
#define configTICK_RATE_HZ FIXTURE_TICK_RATE_HZ
#define configASSERT(x) do { if (!(x)) std::abort(); } while (0)
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>((uint64_t(ms)*FIXTURE_TICK_RATE_HZ)/1000))
''',
    'freertos/task.h': '''#pragma once
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t);
''',
    'freertos/semphr.h': '''#pragma once
#include "freertos/FreeRTOS.h"
struct FixtureMutex;
using SemaphoreHandle_t = FixtureMutex*;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t);
''',
    'driver/gpio.h': '#pragma once\n',
    'driver/i2c_master.h': '''#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
constexpr int I2C_ADDR_BIT_LEN_7=0;
struct i2c_device_config_t {
    int dev_addr_length=0;
    uint16_t device_address=0;
    uint32_t scl_speed_hz=0;
};
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t*, i2c_master_dev_handle_t*);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t*, size_t, int);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t*, size_t, uint8_t*, size_t, int);
''',
}


def replace_once(body, old, new):
    if body.count(old) != 1:
        raise ValueError(f'mutation boundary drift: {old}')
    return body.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pcf-source', type=Path)
    parser.add_argument('--timekeep-source', type=Path)
    parser.add_argument('--codec-source', type=Path)
    parser.add_argument('--mutation', choices=('driver-ticks', 'infinite-lock',
        'keep-old-trust', 'ignore-generation', 'ignore-snapshot',
        'ignore-settime-error', 'codec-ticks', 'ignore-write-budget',
        'ignore-persist-budget', 'late-persist-trust', 'omit-stop-cleanup',
        'stop-error-no-cleanup', 'ignore-late-write'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    pcf = (args.pcf_source or root / 'main/pcf8563.cpp').read_text()
    timekeep = (args.timekeep_source or root / 'main/power/rtc_timekeep.cpp').read_text()
    codec = (args.codec_source or root / 'main/services/audio_service.cpp').read_text()
    power = (root / 'main/power_manager.cpp').read_text()
    if power.count('PersistSystemTimeToRtc(generation, command.deadline_us)') != 1:
        raise ValueError('sleep coordinator must forward the prepare deadline')
    if args.mutation == 'driver-ticks':
        pcf = replace_once(pcf, 'kI2cTransferTimeoutMs = 100;',
                           'kI2cTransferTimeoutMs = pdMS_TO_TICKS(100);')
    elif args.mutation == 'infinite-lock':
        pcf = replace_once(pcf, 'kI2cLockWaitTicks = pdMS_TO_TICKS(100);',
                           'kI2cLockWaitTicks = portMAX_DELAY;')
    elif args.mutation == 'keep-old-trust':
        timekeep = replace_once(timekeep, '    g_trust_record = {};\n', '')
    elif args.mutation == 'ignore-generation':
        timekeep = replace_once(timekeep,
            'g_trust_record.generation != wake.sleep_generation', 'false')
    elif args.mutation == 'ignore-snapshot':
        timekeep = replace_once(timekeep, '!wake.sleep_snapshot_valid', 'false')
    elif args.mutation == 'ignore-settime-error':
        timekeep = timekeep.replace('if (settimeofday(&tv, nullptr) != 0)',
                                    'if ((settimeofday(&tv, nullptr), false))')
        if timekeep.count('if ((settimeofday(&tv, nullptr), false))') != 2:
            raise ValueError('settimeofday boundary drift')
    elif args.mutation == 'codec-ticks':
        old = 'timeout_ms);  // ESP-IDF I2C timeout is milliseconds.'
        codec = codec.replace(old, 'pdMS_TO_TICKS(timeout_ms));')
        if codec.count('pdMS_TO_TICKS(timeout_ms));') != 2:
            raise ValueError('codec boundary drift')
    elif args.mutation == 'ignore-write-budget':
        pcf = replace_once(pcf, '''int sec, int weekday, int64_t deadline_us)
{
''', '''int sec, int weekday, int64_t deadline_us)
{
    deadline_us = 0;
''')
    elif args.mutation == 'ignore-persist-budget':
        timekeep = replace_once(timekeep, 'calendar.weekday, deadline_us)',
                               'calendar.weekday, 0)')
    elif args.mutation == 'late-persist-trust':
        guard = '''    if (deadline_us > 0 && esp_timer_get_time() >= deadline_us) {
        return false;
    }
'''
        if timekeep.count(guard) != 2:
            raise ValueError('persist deadline boundary drift')
        timekeep = ''.join(timekeep.rsplit(guard, 1))
    elif args.mutation == 'omit-stop-cleanup':
        pcf = replace_once(pcf, '''const esp_err_t release_err = i2c_master_transmit(
            g_dev, stop_off, sizeof(stop_off), kI2cTransferTimeoutMs);''',
            'const esp_err_t release_err = ESP_OK;')
    elif args.mutation == 'stop-error-no-cleanup':
        pcf = replace_once(pcf, '''ESP_LOGW(kTag, "RTC STOP set failed: %s", esp_err_to_name(err));
        return release_after_failure();''', '''ESP_LOGW(kTag, "RTC STOP set failed: %s", esp_err_to_name(err));
        return false;''')
    elif args.mutation == 'ignore-late-write':
        guard = '    if (DeadlineExpired(deadline_us)) return false;\n'
        if pcf.count(guard) != 2:
            raise ValueError('write deadline boundary drift')
        pcf = ''.join(pcf.rsplit(guard, 1))

    # Compile historical translation units against the current header too.
    # These compatibility shims deliberately preserve their missing deadline
    # behavior rather than failing to link before a runtime assertion can run.
    if 'int sec, int weekday, int64_t deadline_us)' not in pcf:
        pcf += '''
namespace wqn {
bool Pcf8563WriteTime(int y, int m, int d, int h, int n, int s, int w, int64_t) {
    return Pcf8563WriteTime(y, m, d, h, n, s, w);
}
}
'''
    if 'uint32_t sleep_generation, int64_t deadline_us)' not in timekeep:
        timekeep += '''
namespace wqn::power::timekeep {
bool PersistSystemTimeToRtc(uint32_t generation, int64_t) {
    return PersistSystemTimeToRtc(generation);
}
}
'''

    declarations = []
    names = ['kEs8311Address', 'kCodecI2cMaxAttempts', 'kCodecI2cRetryDelay']
    helpers = ''
    if 'esp_err_t WriteAudioCodecRegisterBefore(' in codec:
        names.append('kCallTimeoutUs')
        helpers = '\n'.join(function(codec, f'esp_err_t {name}(') for name in
            ('WriteAudioCodecRegisterBefore', 'ReadAudioCodecRegisterBefore'))
    for name in names:
        found = re.findall(r'^constexpr [^\n]+ ' + name + r' = [^;]+;', codec, re.M)
        if len(found) != 1:
            raise ValueError(f'codec constant boundary drift: {name}')
        declarations.extend(found)
    functions = '\n'.join(function(codec, f'esp_err_t {name}(')
                          for name in ('WriteAudioCodecRegister', 'ReadAudioCodecRegister'))
    fixture = (root / 'scripts/testdata/rtc_timekeep.cpp').read_text()
    fixture = fixture.replace('@@CODEC_CONSTANTS@@', '\n'.join(declarations))
    fixture = fixture.replace('@@CODEC_FUNCTIONS@@', functions)
    fixture = fixture.replace('@@CODEC_HELPERS@@', helpers)
    fixture = fixture.replace('@@RETRYABLE@@', function(codec, 'bool IsRetryableCodecI2cError('))
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-rtc-') as temporary:
        directory = Path(temporary)
        for name, body in HEADERS.items():
            header = directory / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(body)
        (directory / 'pcf.cpp').write_text(pcf)
        (directory / 'timekeep.cpp').write_text(timekeep)
        (directory / 'fixture.cpp').write_text(fixture)
        for hz in (100, 1000):
            binary = directory / f'rtc-{hz}'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-builtin',
                f'-DFIXTURE_TICK_RATE_HZ={hz}', '-I', str(directory),
                '-I', str(root / 'main'),
                '-I', str(root / 'components/power_runtime/include'),
                str(directory / 'pcf.cpp'), str(directory / 'timekeep.cpp'),
                str(root / 'main/i2c_bus_lock.cpp'), str(directory / 'fixture.cpp'),
                '-Wl,--wrap=time,--wrap=settimeofday', '-o', str(binary)],
                check=True, timeout=60)
            result = subprocess.run([str(binary)], timeout=30)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
