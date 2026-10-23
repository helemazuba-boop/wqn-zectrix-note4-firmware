#!/usr/bin/env python3
"""Run production RTC fault journal, error recorder and clock source. Not retention HIL."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

HEADERS = {
    'esp_attr.h': '#pragma once\n#define RTC_NOINIT_ATTR\n',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'esp_rom_crc.h': '#pragma once\n#include <cstdint>\n#include <cstddef>\nuint32_t esp_rom_crc32_le(uint32_t,const uint8_t*,size_t);\n',
    'esp_log.h': '#pragma once\nvoid FaultHostLog(const char*,const char*,...);\n#define ESP_LOGI(...) FaultHostLog(__VA_ARGS__)\n',
    'esp_system.h': '''#pragma once
enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
 ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
 ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE,
 ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP };
esp_reset_reason_t esp_reset_reason();
''',
    'freertos/FreeRTOS.h': '''#pragma once
#include <mutex>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
void FaultHostEnter(portMUX_TYPE*);
void FaultHostExit(portMUX_TYPE*);
#define taskENTER_CRITICAL(m) FaultHostEnter(m)
#define taskEXIT_CRITICAL(m) FaultHostExit(m)
''',
}


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--error-source', type=Path, default=root/'main/error_recorder.cpp')
    parser.add_argument('--mutation', choices=('ignore-crc', 'old-copy', 'retain-cold',
                        'single-copy', 'clear-history', 'relabel-origin', 'lose-context',
                        'no-overwrite-count', 'no-legacy'))
    args = parser.parse_args()
    source = (root/'main/runtime/fault_diagnostics.cpp').read_text()
    error = args.error_source.read_text()
    mutations = {
        'ignore-crc': ('journal.crc != Crc(journal)', 'false'),
        'old-copy': ('After(candidate.revision, g_current.revision)', 'After(g_current.revision, candidate.revision)'),
        'retain-cold': ('case ESP_RST_SW: case ESP_RST_PANIC:', 'case ESP_RST_POWERON: case ESP_RST_SW: case ESP_RST_PANIC:'),
        'single-copy': ('const unsigned target = 1 - g_active_copy;', 'const unsigned target = g_active_copy;'),
        'clear-history': ('view.previous_boot_id = found ? view.boot_id : 0;', 'view.count = 0; view.previous_boot_id = found ? view.boot_id : 0;'),
        'relabel-origin': ('taskEXIT_CRITICAL(&g_lock);\n    return true;',
                           'for (size_t i=0;i<out->count;++i) { out->records[i].boot_id=out->boot_id; }\n    taskEXIT_CRITICAL(&g_lock);\n    return true;'),
        'lose-context': ('record.operation = operation;', '(void)operation; record.operation = 0;'),
        'no-overwrite-count': ('else view.overwritten = Increment(view.overwritten);', 'else view.overwritten = 0;'),
    }
    if args.mutation in mutations:
        old,new=mutations[args.mutation]
        if source.count(old)!=1: raise SystemExit('FAIL: mutation boundary drift')
        source=source.replace(old,new)
    if args.mutation=='no-legacy':
        assert error.count('runtime::RecordLegacyFault(tag);')==1
        error=error.replace('runtime::RecordLegacyFault(tag);','/* no retained summary */')
    # The only write seam injects reset in the real inactive-copy publication.
    source=source.replace('std::memcpy(', 'FaultHostCopy(')
    compiler=shutil.which('g++')
    if not compiler: raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-fault-journal-') as tmp:
        tmp=Path(tmp)
        for name,body in HEADERS.items():
            path=tmp/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(body)
        (tmp/'fault_diagnostics.cpp').write_text(source)
        (tmp/'error_recorder.cpp').write_text(error)
        (tmp/'fixture.cpp').write_text((root/'scripts/testdata/fault_diagnostics.cpp').read_text())
        subprocess.run([compiler,'-std=c++20','-O1','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread',
            '-DWQN_GIT_COMMIT="host1234"','-I'+str(tmp),'-I'+str(root/'main'),
            str(tmp/'fixture.cpp'),'-Wl,--wrap=time','-o',str(tmp/'test')],check=True,timeout=45)
        return subprocess.run([str(tmp/'test')],timeout=30).returncode


if __name__=='__main__':
    raise SystemExit(main())
