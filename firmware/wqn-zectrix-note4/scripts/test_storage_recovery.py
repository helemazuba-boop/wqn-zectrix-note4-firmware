#!/usr/bin/env python3
"""R-18 actual recovery model, coordinator and UI with explicit host ports. NOT HIL."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ERRORS = '''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=257, ESP_ERR_INVALID_ARG=258,
 ESP_ERR_INVALID_STATE=259, ESP_ERR_NOT_SUPPORTED=262, ESP_ERR_TIMEOUT=263;
inline const char* esp_err_to_name(int) { return "host-error"; }
'''
RTOS = '''#pragma once
#include <cstdint>
#include <cstddef>
#include <mutex>
using BaseType_t=int; using UBaseType_t=unsigned; using TickType_t=uint32_t;
using StackType_t=uint32_t; using TaskHandle_t=void*;
struct StaticTask_t {};
struct portMUX_TYPE { std::mutex mutex; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define taskENTER_CRITICAL(p) ((p)->mutex.lock())
#define taskEXIT_CRITICAL(p) ((p)->mutex.unlock())
constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>((ms)*configTICK_RATE_HZ/1000)
TaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,uint32_t,void*,unsigned,StackType_t*,StaticTask_t*);
TaskHandle_t xTaskGetCurrentTaskHandle();
uint32_t ulTaskNotifyTake(int,TickType_t);
int xTaskNotifyGive(TaskHandle_t);
'''
UI = '''#pragma once
#include <string>
#include <vector>
#include "esp_err.h"
#include "freertos/task.h"
#include "display/display_types.h"
#include "ui/display_recovery.h"
#define ESP_RETURN_ON_ERROR(expr,tag,...) do {(void)(tag);const auto e=(expr); if(e!=ESP_OK)return e;} while(false)
int64_t esp_timer_get_time();
namespace wqn {
enum class UiScreen { kProvisioning };
enum class UiTextStyle { kTitle,kBody,kWarning,kWrappedBody,kSelected };
struct UiLine { UiTextStyle style; std::string text; };
struct UiFrame { UiScreen screen{}; bool prefer_full_refresh=false; std::vector<UiLine> lines; };
}
namespace device_ui_internal {
enum class RefreshSchedule { kCommit };
extern void* g_display_result_queue;
wqn::display::DisplaySubmission RequestEpdUiRefresh(const wqn::UiFrame&,const std::string&,
 wqn::display::DisplayRevision,RefreshSchedule,wqn::display::WaveformRequirement);
void AcknowledgeDisplayResult(wqn::display::DisplayRevision);
std::string FrameSignature(const wqn::UiFrame&);
DisplayRecoveryStatus CopyDisplayRecoveryStatus();
bool RequestDisplayRecovery(int64_t);
}
int xQueueReceive(void*,wqn::display::DisplayResult*,TickType_t);
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--part', choices=('model', 'service', 'ui', 'all'), default='all')
    parser.add_argument('--tick-hz', type=int, choices=(100, 1000), default=1000)
    parser.add_argument('--mutation', choices=('short-hold', 'unpresented-reset', 'forget-uncertain'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='wqn-recovery-') as temporary:
        tmp = Path(temporary)
        headers = {
            'esp_err.h': ERRORS, 'freertos/FreeRTOS.h': RTOS,
            'freertos/task.h': '#pragma once\n#include "freertos/FreeRTOS.h"\n',
            'esp_heap_caps.h': '#pragma once\nvoid heap_caps_free(void*);\n',
            'esp_log.h': '#pragma once\nvoid HostLog(const char*,const char*,...);\n#define ESP_LOGI(...) HostLog(__VA_ARGS__)\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'esp_system.h': '#pragma once\nvoid esp_restart();\n',
            'ui/ui_internal.h': UI,
            'ui/storage_recovery_model.h': (root/'main/ui/storage_recovery_model.h').read_text(),
        }
        if args.mutation:
            old, new = {
                'short-hold': ('event.occurred_at_ms - held >= 2000 &&\n                    event.duration_ms >= 2000',
                               'event.occurred_at_ms - held >= 0 &&\n                    event.duration_ms >= 0'),
                'unpresented-reset': ('if (!presented_ || event.occurred_at_ms <= presented_at_ms_) {\n                held_at_ms_ = 0;',
                                     'if (false) {\n                held_at_ms_ = 0;'),
                'forget-uncertain': ('else if (result.outcome == Outcome::kUncertain) SetPage(Page::kUncertain);',
                                     'else if (result.outcome == Outcome::kUncertain) SetPage(Page::kError);'),
            }[args.mutation]
            body = headers['ui/storage_recovery_model.h']
            if body.count(old) != 1:
                raise ValueError('recovery mutation boundary drift')
            headers['ui/storage_recovery_model.h'] = body.replace(old, new)
        for name, body in headers.items():
            path = tmp/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)
        parts = ('model', 'service', 'ui') if args.part == 'all' else (args.part,)
        code = 0
        for part in parts:
            binary = tmp/f'fixture-{part}'
            sources = [root/f'scripts/testdata/storage_recovery_{part}.cpp']
            if part == 'service': sources.append(root/'main/services/storage_recovery_service.cpp')
            if part == 'ui': sources.append(root/'main/ui/storage_recovery_ui.cpp')
            subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                '-fno-pie', '-no-pie', f'-DconfigTICK_RATE_HZ={args.tick_hz}',
                '-I'+str(tmp), '-I'+str(root/'main'), '-I'+str(root/'components/display_service/include'),
                *map(str, sources), '-o', str(binary)], check=True, timeout=60)
            done = subprocess.run([str(binary)], timeout=30, env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
            if done.returncode: code = done.returncode
        return code


if __name__ == '__main__':
    raise SystemExit(main())
