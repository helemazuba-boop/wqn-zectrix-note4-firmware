#!/usr/bin/env python3
"""Replay production GPIO/task/ring, stale filter, PTT and sleep commit on host ports."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_audio_runtime import function

HEADERS = {
    'esp_err.h': '''#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=259, ESP_ERR_NO_MEM=257;
inline const char* esp_err_to_name(int) { return "port"; }
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))
''',
    'esp_log.h': '''#pragma once
void FixtureLog(const char*, const char*, ...);
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
''',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'esp_sleep.h': '#pragma once\nint esp_sleep_enable_gpio_wakeup();\n',
    'driver/i2c_master.h': '#pragma once\nusing i2c_port_t=int; using i2c_master_bus_handle_t=void*;\n',
    'driver/gpio.h': '''#pragma once
#include <cstdint>
#define IRAM_ATTR
using gpio_num_t=int;
constexpr int GPIO_NUM_39=39, GPIO_NUM_18=18, GPIO_NUM_0=0;
constexpr int GPIO_MODE_INPUT=1, GPIO_PULLUP_ENABLE=1, GPIO_PULLDOWN_DISABLE=0, GPIO_INTR_LOW_LEVEL=1;
struct gpio_config_t { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; };
int gpio_config(const gpio_config_t*);
int gpio_get_level(gpio_num_t);
int gpio_intr_disable(gpio_num_t);
int gpio_intr_enable(gpio_num_t);
int gpio_install_isr_service(int);
int gpio_isr_handler_add(gpio_num_t, void(*)(void*), void*);
int gpio_wakeup_enable(gpio_num_t, int);
''',
    'freertos/FreeRTOS.h': '''#pragma once
#include <cstdint>
using BaseType_t=int; using UBaseType_t=unsigned; using TickType_t=uint32_t;
using TaskHandle_t=void*;
struct portMUX_TYPE { unsigned depth=0; };
#define portMUX_INITIALIZER_UNLOCKED {}
constexpr int pdFALSE=0, pdTRUE=1, pdPASS=1;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr int configTICK_RATE_HZ=FIXTURE_TICK_RATE_HZ;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>((ms)*configTICK_RATE_HZ/1000))
#define pdTICKS_TO_MS(ticks) ((ticks)*1000/configTICK_RATE_HZ)
void FixtureEnter(portMUX_TYPE*);
void FixtureExit(portMUX_TYPE*);
#define taskENTER_CRITICAL(m) FixtureEnter(m)
#define taskEXIT_CRITICAL(m) FixtureExit(m)
#define portYIELD_FROM_ISR(x) ((void)(x))
''',
    'freertos/task.h': '''#pragma once
#include "freertos/FreeRTOS.h"
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t*);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskDelay(TickType_t);
BaseType_t xTaskCreate(void(*)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
unsigned uxTaskGetStackHighWaterMark(void*);
''',
}


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--button-source', type=Path, default=root/'main/button_input.cpp')
    parser.add_argument('--ui-source', type=Path, default=root/'main/device_ui.cpp')
    parser.add_argument('--case', choices=('all', 'gestures', 'capacity', 'stale', 'ring', 'sleep', 'irq'), default='all')
    parser.add_argument('--tick-rate', type=int, choices=(100,1000))
    parser.add_argument('--mutation', choices=('drop-release', 'small-ring', 'publish-after-ring',
                        'repeat-activity', 'drop-new-critical', 'keep-repeats', 'wrong-repeat',
                        'consume-time', 'ignore-final-gate', 'clear-after-enable', 'idle-poll', 'lose-release-tail'))
    args = parser.parse_args()
    buttons = args.button_source.read_text()
    ui = args.ui_source.read_text()
    power = (root/'main/power_manager.cpp').read_text()
    inputs = (root/'main/ui/ui_input.cpp').read_text()
    model = (root/'main/ui_model.h').read_text()
    stale = function(ui, 'bool ShouldDropStaleButtonEvent(')
    flash_start = inputs.index('    if (state->screen == wqn::UiScreen::kAi &&', inputs.index('// [mistouch/PTT]'))
    flash = inputs[flash_start:inputs.index('    // A 200..999 ms Flash PTT', flash_start)]
    agent_start = inputs.index('    if (is_agent_ptt &&', inputs.index('// Agent PTT is intentionally'))
    agent = inputs[agent_start:inputs.index('    // button_input emits a derived', agent_start)]
    publish = function(power, 'void PublishUserActivity(')
    note = function(power, 'void NoteUserActivityAtMs(')
    commit = function(power, 'static DeepSleepCommitAbortReason CommitDeepSleep(')
    if args.mutation == 'drop-release':
        stale = stale.replace('event.type == wqn::ButtonEventType::kHoldPress)',
                              'event.type == wqn::ButtonEventType::kHoldPress || event.type == wqn::ButtonEventType::kRelease)')
    if args.mutation == 'small-ring':
        buttons = buttons.replace('kEventRingCapacity = 84;', 'kEventRingCapacity = 64;')
    if args.mutation == 'publish-after-ring':
        block = '    if (!IsRepeatEvent(event)) {\n        wqn::NoteUserActivityAtMs(event.occurred_at_ms);\n    }\n'
        assert buttons.count(block) == 1
        buttons = buttons.replace(block, '')
        buttons = buttons.replace('    g_event_ring[g_ring_head] = event;',
                                   '    g_event_ring[g_ring_head] = event;\n'+block)
    if args.mutation == 'repeat-activity':
        buttons = buttons.replace('if (!IsRepeatEvent(event)) {', 'if (true) {')
    if args.mutation == 'drop-new-critical':
        buttons = buttons.replace('if (IsRepeatEvent(event)) {\n            // Full ring', 'if (true) {\n            // Full ring')
    if args.mutation == 'keep-repeats':
        buttons = buttons.replace('if (IsRepeatEvent(newest) && newest.button == event.button)', 'if (false)')
    if args.mutation == 'wrong-repeat':
        buttons = buttons.replace('event.repeat = is_repeat;', 'event.repeat = !is_repeat;')
    if args.mutation == 'consume-time':
        publish = publish.replace('UserActivityMsRef().store(occurred_at_ms,', '(void)occurred_at_ms; UserActivityMsRef().store(esp_timer_get_time()/1000,')
    if args.mutation == 'ignore-final-gate':
        commit = commit.replace('if (gate_on_activity_baseline != nullptr)', 'if (false && gate_on_activity_baseline != nullptr)')
    if args.mutation == 'clear-after-enable':
        buttons = buttons.replace('button.isr_disabled.store(0, std::memory_order_release);\n            gpio_intr_enable(button.pin);',
                                  'gpio_intr_enable(button.pin);\n            button.isr_disabled.store(0, std::memory_order_release);')
    if args.mutation == 'idle-poll':
        buttons = buttons.replace('if (!AnyButtonWindowActive()) {', 'if (false && !AnyButtonWindowActive()) {')
    if args.mutation == 'lose-release-tail':
        buttons = buttons.replace('button.pending_edge = wqn::ButtonEventType::kRelease;',
                                  'button.pending_edge = wqn::ButtonEventType::kNone;')
    fixture = (root/'scripts/testdata/button_input.cpp').read_text()
    pieces = {'@@STALE@@': stale, '@@FLASH@@': flash, '@@AGENT@@': agent,
              '@@UI_TYPES@@': function(model, 'enum class AiTier :')+';\n'+function(model, 'struct UiGestureState {')+';',
              '@@ACTIVITY@@': publish+'\n'+note,
              '@@COMMIT@@': function(power, 'enum class DeepSleepCommitAbortReason :')+';\nnamespace wqn {\n'+commit+'\n}'}
    for marker, value in pieces.items():
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, value)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='note4-button-input-') as directory:
        directory = Path(directory)
        for name, body in HEADERS.items():
            path=directory/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(body)
        (directory/'button_input.cpp').write_text(buttons)
        (directory/'fixture.cpp').write_text(fixture)
        for hz in ((args.tick_rate,) if args.tick_rate else (100,1000)):
            binary=directory/f'buttons-{hz}'
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                f'-DFIXTURE_TICK_RATE_HZ={hz}', '-DCONFIG_WQN_AI_ENABLE=1',
                '-DCONFIG_ESP_CONSOLE_UART_NUM=0', '-I'+str(directory), '-I'+str(root/'main'),
                str(directory/'fixture.cpp'), str(root/'main/ui/navigation_model.cpp'),
                '-o', str(binary)], check=True, timeout=45)
            result=subprocess.run([str(binary),args.case], timeout=20)
            if result.returncode: return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
