#!/usr/bin/env python3
"""Run real ASR pipe, WS owner loop/consumer and terminal ledger with host ports.

Virtual RTOS schedules at 100/1000 Hz; real SSE, cJSON and WFLV encoding.
No network/model/device calls. The connect, notification and SDK I/O ports
cannot prove callback concurrency, SDK stop latency or physical HIL.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_audio_runtime import function
from test_agent_voice_finalize import PORTS


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', help='read runtime code from a git revision')
    parser.add_argument('--tick-hz', type=int, choices=(100, 1000), default=100)
    parser.add_argument('--mutation', choices=('no-expiry', 'ignore-generation', 'ignore-request',
                        'renew-duplicate', 'full-tier-asr', 'short-from-final', 'no-absolute-cap'))
    args = parser.parse_args()

    def source(name):
        if args.before:
            return subprocess.check_output(['git', 'show',
                f'{args.before}:firmware/wqn-zectrix-note4/main/{name}'], cwd=root, text=True)
        return (root/'main'/name).read_text()

    ws = source('stdpro_ws_transport.cpp')
    header = (root/'main/turn_release_deadline.h').read_text()
    mutations = {
        'no-expiry': ('ws', 'g_turn_release_deadline.Expired(g_active_turn_gen, g_active_turn_req_id, now_ms)', 'false'),
        'ignore-generation': ('header', 'generation == generation_', '(static_cast<void>(generation), true)'),
        'ignore-request': ('header', 'std::strcmp(request_id_, request_id) == 0;', 'true;'),
        'renew-duplicate': ('header', 'if (due_ms_ == 0 || due < due_ms_) due_ms_ = due;', 'due_ms_ = due;'),
        'full-tier-asr': ('ws', 'const bool asr_terminal = g_active_turn_asr_only &&', 'const bool asr_terminal = true &&'),
        'short-from-final': ('ws', 'wqn::stdpro_ws::TurnReleaseDeadline::kMaxAsrHandoffMs);',
                             'wqn::stdpro_ws::TurnReleaseDeadline::kResultGraceMs);'),
        'no-absolute-cap': ('ws', 'if (g_active_turn_asr_only) {', 'if (false) {'),
    }
    if args.mutation:
        target, old, new = mutations[args.mutation]
        code = ws if target == 'ws' else header
        if code.count(old) != 1:
            raise SystemExit('FAIL: mutation boundary drift')
        code = code.replace(old, new)
        if target == 'ws':
            ws = code
        else:
            header = code
    types = []
    for signature in ('enum class ConnectionState :', 'enum class TurnState :', 'struct PcmBlock {',
                      'enum class CtrlCmdType :', 'enum class CmdDisposition :', 'struct CtrlCmd {',
                      'struct CtrlCompletion {', 'struct TurnTerminalRecord {', 'enum class TeardownReason :'):
        start = ws.index(signature)
        types.append(ws[start:ws.index('\n};', start)+3])
    constants = '\n'.join(re.search(rf'constexpr size_t {name} = [^;]+;', ws).group()
                          for name in ('kPcmFramesPerBlock', 'kPcmQueueLength'))
    owner = function(ws, 'void ExecuteCtrlCmdSingleStep(')
    start = owner.index('        case CtrlCmdType::kStartTurn: {')
    end = owner.index('        case CtrlCmdType::kAbort:', start)
    fixture = (root/'scripts/testdata/ws_release_deadline.cpp').read_text()
    replacements = {
        '@@TYPES@@': '\n'.join(types), '@@CONSTANTS@@': constants,
        '@@START_CASE@@': owner[start:end],
        '@@OWNER@@': '\n'.join(function(ws, signature) for signature in (
            'void PublishTurnTerminalRecord(', 'void DoDestroyWs()',
            'void TeardownFailedTurn(', 'void ProcessTextStream()')),
        '@@ROOT@@': function(ws, 'void VoiceWsTransportTask('),
        '@@PUBLIC@@': '\n'.join(function(ws, signature) for signature in (
            'esp_err_t StartTurn(', 'void PushPcm(const int16_t* samples, size_t count)',
            'FinalHandoffResult SendFinalAndWait(', 'void SetSseCallback(',
            'bool TryGetTurnTerminal(', 'TurnWaitResult WaitForTurnRelease(')),
    }
    for marker, code in replacements.items():
        if fixture.count(marker) != 1:
            raise SystemExit('FAIL: fixture boundary drift: '+marker)
        fixture = fixture.replace(marker, code)
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))/'components/json/cJSON'
    with tempfile.TemporaryDirectory(prefix='wqn-release-deadline-') as directory:
        tmp = Path(directory)
        ports = dict(PORTS)
        ports['freertos/FreeRTOS.h'] = ports['freertos/FreeRTOS.h'].replace(
            '(static_cast<TickType_t>((ms)/10))',
            '(static_cast<TickType_t>((ms)*configTICK_RATE_HZ/1000))')
        ports['esp_log.h'] += '\n#define ESP_LOGE(...) HostLog(__VA_ARGS__)\n'
        for name, code in ports.items():
            p = tmp/name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(code)
        for name in ('agent_voice_pipe.cpp', 'agent_voice_pipe.h', 'stdpro_ws_transport.h',
                     'sse_chunk.h', 'sse_chunk.cpp'):
            (tmp/name).write_text(source(name))
        (tmp/'turn_release_deadline.h').write_text(header)
        (tmp/'test.cpp').write_text(fixture)
        binary = tmp/'test'
        macros = [f'-DWQN_{name}_SCHEMA_SHA256="host-only"' for name in
                  ('DEVICE_CONTROL', 'WORD_STUDY', 'NOTE_STUDY', 'PROBLEM_STUDY')]
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
            '-fno-pie', '-no-pie', '-DCONFIG_WQN_AI_ENABLE=1',
            f'-DconfigTICK_RATE_HZ={args.tick_hz}', *macros,
            '-I'+str(tmp), '-I'+str(root/'main'), '-I'+str(cjson),
            '-I'+str(root/'components/device_protocol/include'), str(tmp/'test.cpp'),
            str(tmp/'agent_voice_pipe.cpp'), str(tmp/'sse_chunk.cpp'), str(cjson/'cJSON.c'),
            '-o', str(binary)], check=True, timeout=60)
        return subprocess.run([str(binary)], timeout=30, env=dict(os.environ,
            ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__ == '__main__':
    raise SystemExit(main())
