#!/usr/bin/env python3
"""Run the real voice pipe and terminal ledger against a virtual 100 Hz RTOS.

No device, network or model calls. SDK semaphore/time/transport command ports
are replaced; production ASR accumulation, finalize and ledger code are compiled.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_audio_runtime import function


PORTS = {
    'esp_err.h': '''#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=257, ESP_ERR_INVALID_ARG=258;
constexpr int ESP_ERR_INVALID_STATE=259, ESP_ERR_INVALID_SIZE=260;
constexpr int ESP_ERR_NOT_SUPPORTED=262, ESP_ERR_TIMEOUT=263, ESP_ERR_INVALID_RESPONSE=264;
inline const char* esp_err_to_name(int) { return "host"; }
''',
    'esp_log.h': '''#pragma once
void HostLog(const char*, const char*, ...);
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
''',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'ai_session.h': '#pragma once\nnamespace wqn { void ArmAiVoicePreroll(); }\n',
    'freertos/FreeRTOS.h': '''#pragma once
#include <cstdint>
#include <mutex>
using TickType_t=uint32_t;
constexpr TickType_t portMAX_DELAY=0xffffffff;
constexpr int pdTRUE=1, pdFALSE=0;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>((ms)/10))
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
void HostEnter(portMUX_TYPE*);
void HostExit(portMUX_TYPE*);
#define portENTER_CRITICAL(mux) HostEnter(mux)
#define portEXIT_CRITICAL(mux) HostExit(mux)
''',
    'freertos/semphr.h': '''#pragma once
#include "FreeRTOS.h"
struct StaticSemaphore_t { bool available=false; bool mutex=false; };
using SemaphoreHandle_t=StaticSemaphore_t*;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t*);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
''',
}


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', help='read production code from a git revision')
    parser.add_argument('--mutation', choices=('ignore-generation', 'ignore-request', 'drop-result-reread'))
    args = parser.parse_args()

    def source(name):
        if args.before:
            return subprocess.check_output(['git', 'show', f'{args.before}:firmware/wqn-zectrix-note4/main/{name}'],
                                           cwd=root, text=True)
        return (root / 'main' / name).read_text()

    ws = source('stdpro_ws_transport.cpp')
    pipe = source('agent_voice_pipe.cpp')
    # Test-only observer visibility seam; it reads the real pipe state while
    # holding the same lock. No production entry point is added.
    pipe += '''\nnamespace wqn {
bool HostVoiceResultPublished() {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool published = g_saw_complete || g_saw_error;
    xSemaphoreGive(g_lock);
    return published;
}
}\n'''
    if args.mutation:
        changes = {
            'ignore-generation': ('snapshot.turn_gen != expected_turn_gen ||', 'false ||'),
            'ignore-request': ('expected_request_id != snapshot.request_id', 'false'),
            'drop-result-reread': ('            complete = g_saw_complete;\n            error = g_saw_error;',
                                   '            /* mutation: omit post-snapshot result re-read */'),
        }
        needle, replacement = changes[args.mutation]
        target = pipe if args.mutation == 'drop-result-reread' else ws
        if target.count(needle) != 1:
            raise SystemExit('FAIL: mutation boundary drift')
        target = target.replace(needle, replacement)
        if args.mutation == 'drop-result-reread':
            pipe = target
        else:
            ws = target
    has_query = 'bool TryGetTurnTerminal(' in ws
    start = ws.index('struct TurnTerminalRecord {')
    record = ws[start:ws.index('\n};', start) + 3]
    fixture = (root / 'scripts/testdata/agent_voice_finalize.cpp').read_text()
    fixture = fixture.replace('@@RECORD@@', record)
    fixture = fixture.replace('@@PUBLISH@@', function(ws, 'void PublishTurnTerminalRecord('))
    fixture = fixture.replace('@@QUERY@@', function(ws, 'bool TryGetTurnTerminal(') if has_query else '')
    fixture = fixture.replace('@@WAIT@@', function(ws, 'TurnWaitResult WaitForTurnRelease('))
    macros = [f'-DWQN_{name}_SCHEMA_SHA256="host-only"' for name in
              ('DEVICE_CONTROL', 'WORD_STUDY', 'NOTE_STUDY', 'PROBLEM_STUDY')]
    with tempfile.TemporaryDirectory(prefix='wqn-voice-finalize-') as directory:
        tmp = Path(directory)
        for name, content in PORTS.items():
            p = tmp / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        for name in ('agent_voice_pipe.cpp', 'agent_voice_pipe.h', 'stdpro_ws_transport.h'):
            (tmp / name).write_text(pipe if name == 'agent_voice_pipe.cpp' else source(name))
        (tmp / 'test.cpp').write_text(fixture)
        binary = tmp / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                        '-DCONFIG_WQN_AI_ENABLE=1', f'-DHAS_TERMINAL_QUERY={int(has_query)}', *macros,
                        '-I'+str(tmp), '-I'+str(root / 'main'),
                        '-I'+str(root / 'components/device_protocol/include'),
                        str(tmp / 'agent_voice_pipe.cpp'), str(tmp / 'test.cpp'), '-o', str(binary)],
                       check=True, timeout=60)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)], env=env, timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
