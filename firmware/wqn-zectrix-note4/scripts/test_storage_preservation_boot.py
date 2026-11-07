#!/usr/bin/env python3
"""Production physical boot selector with GPIO/timer/reset ports. No devices."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_storage_recovery import ERRORS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tick-hz', type=int, choices=(100, 1000), default=1000)
    parser.add_argument('--mutation', choices=('short-hold', 'wake-select'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='wqn-preservation-boot-') as temp:
        directory = Path(temp)
        headers = {
            'esp_err.h': ERRORS+'\nconstexpr int ESP_ERR_INVALID_RESPONSE=264;\n',
            'esp_system.h': '#pragma once\nenum esp_reset_reason_t {ESP_RST_SW=3, ESP_RST_DEEPSLEEP=5};\nesp_reset_reason_t esp_reset_reason();\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'board_zectrix_note4.h': '#pragma once\nnamespace wqn { int ReadNote4BootPageUpLevel(); }\n',
            'freertos/FreeRTOS.h': '#pragma once\n#include <cstdint>\nusing TickType_t=uint32_t;\n#define pdMS_TO_TICKS(ms) ((ms)*configTICK_RATE_HZ/1000)\n',
            'freertos/task.h': '#pragma once\n#include "freertos/FreeRTOS.h"\nvoid vTaskDelay(TickType_t);\n',
        }
        for name, body in headers.items():
            path=directory/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(body)
        source=(root/'main/runtime/storage_preservation_boot.cpp').read_text()
        if args.mutation:
            old,new={'short-hold':('now-started >= 3000000','now-started >= 0'),
                     'wake-select':('esp_reset_reason() == ESP_RST_DEEPSLEEP','false')}[args.mutation]
            if source.count(old)!=1:raise RuntimeError('preservation mutation boundary drift')
            source=source.replace(old,new)
        (directory/'selector.cpp').write_text(source)
        binary=directory/'native'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie',
            f'-DconfigTICK_RATE_HZ={args.tick_hz}','-I'+str(directory),'-I'+str(root/'main'),
            str(directory/'selector.cpp'),str(root/'scripts/testdata/storage_preservation_boot.cpp'),'-o',str(binary)],check=True,timeout=30)
        return subprocess.run([str(binary)],timeout=15).returncode

if __name__=='__main__':raise SystemExit(main())
