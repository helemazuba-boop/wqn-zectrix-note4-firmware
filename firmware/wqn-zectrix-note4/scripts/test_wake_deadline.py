#!/usr/bin/env python3
"""Exercise full wake/RTC/lock sources and production short waits at two tick rates.

Driver, GPIO, RTOS and monotonic clock ports model completion and tick phase.
No physical BUSY/ADC/interrupt timing or task scheduling is proven here.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_audio_runtime import function
from test_rtc_timekeep import HEADERS, replace_once


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wake-source', type=Path)
    parser.add_argument('--display-source', type=Path)
    parser.add_argument('--mutation', choices=('renew-rollback', 'ignore-rtc-deadline',
        'accept-late-i2c', 'ignore-ext1-deadline', 'zero-settle', 'omit-tick-phase'))
    args = parser.parse_args()
    wake = (args.wake_source or root / 'main/power/wake_controller.cpp').read_text()
    pcf = (root / 'main/pcf8563.cpp').read_text()
    delay = (root / 'components/power_runtime/include/runtime/task_delay.h').read_text()
    if args.mutation == 'renew-rollback':
        wake = wake.replace('DisarmWakeSources(deadline_us);', 'DisarmWakeSources();')
    elif args.mutation == 'ignore-rtc-deadline':
        pcf = replace_once(pcf, 'bool Pcf8563ConfigureTimerWake(uint8_t seconds, int64_t deadline_us)',
                           'bool Pcf8563ConfigureTimerWake(uint8_t seconds, int64_t)')
        pcf = pcf.replace('// Stop the countdown before changing its value.',
                          'const int64_t deadline_us = 0;\n    // Stop the countdown before changing its value.')
    elif args.mutation == 'accept-late-i2c':
        old = 'return DeadlineExpired(deadline_us) ? ESP_ERR_TIMEOUT : result;'
        if pcf.count(old) != 2:
            raise ValueError('I2C result boundary drift')
        pcf = pcf.replace(old, 'return result;')
    elif args.mutation == 'ignore-ext1-deadline':
        wake = replace_once(wake, '''    if (DeadlineExpired(deadline_us)) {
        DisarmWakeSources(deadline_us);
        result.error = ESP_ERR_TIMEOUT;
        return result;
    }
    ResetActiveWakeLineDenialTracking();''', '    ResetActiveWakeLineDenialTracking();')
    elif args.mutation == 'zero-settle':
        wake = replace_once(wake, 'wqn::runtime::DelayAtLeastMs<2>();',
                            'vTaskDelay(pdMS_TO_TICKS(2));')
    elif args.mutation == 'omit-tick-phase':
        delay = replace_once(delay, 'configTICK_RATE_HZ + 999) / 1000 + 1;',
                             'configTICK_RATE_HZ + 999) / 1000;')

    display = (args.display_source or root / 'components/display_service/display_service.cpp').read_text()
    fixture = (root / 'scripts/testdata/wake_deadline.cpp').read_text()
    for marker, value in {
        '@@EPD_BUSY@@': function(display, 'esp_err_t WaitBusyTimeout('),
        '@@EPD_STATUS@@': function(display, 'esp_err_t WaitPanelStatusReady('),
    }.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture boundary drift: {marker}')
        fixture = fixture.replace(marker, value)
    headers = dict(HEADERS)
    headers['esp_log.h'] += '#define ESP_LOGD(...) FixtureLog(__VA_ARGS__)\n'
    headers['freertos/FreeRTOS.h'] += '#define pdTICKS_TO_MS(t) ((t)*1000/FIXTURE_TICK_RATE_HZ)\n'
    headers['freertos/task.h'] += 'TickType_t xTaskGetTickCount();\n'
    headers['driver/gpio.h'] = '''#pragma once
#include "esp_err.h"
enum gpio_num_t { GPIO_NUM_0=0, GPIO_NUM_1=1, GPIO_NUM_2=2, GPIO_NUM_5=5,
                  GPIO_NUM_8=8, GPIO_NUM_18=18 };
constexpr int GPIO_INTR_LOW_LEVEL=0;
int gpio_get_level(gpio_num_t);
esp_err_t gpio_wakeup_enable(gpio_num_t, int);
'''
    headers['esp_sleep.h'] = '''#pragma once
#include <cstdint>
#include "esp_err.h"
enum esp_sleep_wakeup_cause_t { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_EXT1,
                              ESP_SLEEP_WAKEUP_TIMER };
constexpr int ESP_EXT1_WAKEUP_ANY_LOW=0;
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t);
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t, int);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t);
esp_err_t esp_sleep_enable_gpio_wakeup();
'''
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-wake-deadline-') as temporary:
        directory = Path(temporary)
        headers['runtime/task_delay.h'] = delay
        for name, body in headers.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(body)
        for name, body in [('wake.cpp', wake), ('pcf.cpp', pcf), ('fixture.cpp', fixture)]:
            (directory / name).write_text(body)
        for hz in (100, 1000):
            binary = directory / f'wake-{hz}'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                f'-DFIXTURE_TICK_RATE_HZ={hz}', '-I', str(directory),
                '-I', str(root / 'main'), '-I', str(root / 'components/power_runtime/include'),
                str(directory / 'wake.cpp'), str(directory / 'pcf.cpp'),
                str(root / 'main/i2c_bus_lock.cpp'), str(directory / 'fixture.cpp'),
                '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], timeout=30)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
