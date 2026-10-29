#!/usr/bin/env python3
"""Run the production turn commit, tap and PCM enqueue code with RTOS/WS ports.

Real cJSON and ASan/UBSan/LSan; no network, model or physical microphone calls.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_audio_runtime import function


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', help='read production code from a git revision')
    parser.add_argument('--mutation', choices=('no-replay', 'arm-before-send',
                                              'skip-prefix', 'continue-after-overflow',
                                              'ignore-missing-prefix', 'empty-final'))
    args = parser.parse_args()

    def source(name):
        if args.before:
            return subprocess.check_output(
                ['git', 'show', f'{args.before}:firmware/wqn-zectrix-note4/main/{name}'],
                cwd=root, text=True)
        return (root/'main'/name).read_text()

    ws, ai = source('stdpro_ws_transport.cpp'), source('ai_session.cpp')
    owner = function(ws, 'void ExecuteCtrlCmdSingleStep(')
    start = owner.index('        case CtrlCmdType::kStartTurn: {')
    end = owner.index('        case CtrlCmdType::kAbort:', start)
    commit = owner[start:end]
    push = function(ws, 'void PushPcm(const int16_t* samples, size_t count)')
    final = function(ws, 'FinalHandoffResult SendFinalAndWait(')
    if args.mutation:
        changes = {
            'no-replay': ('g_pcm_preroll_pending.store(true, std::memory_order_release);',
                          'g_pcm_preroll_pending.store(false, std::memory_order_release);'),
            'arm-before-send': ('            const size_t json_len = std::strlen(json_str);',
                                '            g_turn_state.store(TurnState::kRecording, std::memory_order_release);\n'
                                '            const size_t json_len = std::strlen(json_str);'),
            'skip-prefix': ('backlog_count = captured - count;', 'backlog_count = 0;'),
            'continue-after-overflow': ('                return;\n            }\n            if (g_transport_task',
                                        '                break;\n            }\n            if (g_transport_task'),
            'ignore-missing-prefix': ('g_prefinal_pcm_lost.store(true, std::memory_order_release);',
                                      'g_prefinal_pcm_lost.store(false, std::memory_order_release);'),
            'empty-final': ('if (g_pcm_preroll_pending.load(std::memory_order_acquire))', 'if (false)'),
        }
        old, new = changes[args.mutation]
        target = (commit if args.mutation in ('no-replay', 'arm-before-send')
                  else final if args.mutation == 'empty-final' else push)
        if target.count(old) != 1:
            raise SystemExit('FAIL: preroll mutation boundary drift')
        target = target.replace(old, new)
        if args.mutation in ('no-replay', 'arm-before-send'):
            commit = target
        elif args.mutation == 'empty-final':
            final = target
        else:
            push = target
    types = []
    for signature in ('enum class TurnState :', 'struct PcmBlock {', 'enum class CtrlCmdType :',
                      'enum class CmdDisposition :', 'struct CtrlCmd {', 'struct CtrlCompletion {'):
        start = ws.index(signature)
        types.append(ws[start:ws.index('\n};', start)+3])
    constants = []
    for name in ('kPcmFramesPerBlock', 'kPcmQueueLength'):
        constants.append(re.search(rf'constexpr size_t {name} = [^;]+;', ws).group())
    legacy_signature = 'void ArmAiVoicePreroll()\n{'
    legacy = function(ai, legacy_signature) if legacy_signature in ai else 'void ArmAiVoicePreroll() {}'
    fixture = (root/'scripts/testdata/pcm_preroll.cpp').read_text()
    replacements = {'@@TYPES@@': '\n'.join(types), '@@CONSTANTS@@': '\n'.join(constants),
                    '@@START_CASE@@': commit, '@@START@@': function(ws, 'esp_err_t StartTurn('),
                    '@@PUSH@@': push, '@@FINAL@@': final, '@@TAP@@': function(ai, 'void AudioCaptureTapHandler('),
                    '@@LEGACY_ARM@@': legacy}
    for marker, content in replacements.items():
        if fixture.count(marker) != 1:
            raise SystemExit(f'FAIL: preroll fixture boundary drift: {marker}')
        fixture = fixture.replace(marker, content)
    sdk = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))
    cjson = sdk/'components/json/cJSON'
    with tempfile.TemporaryDirectory(prefix='wqn-preroll-') as directory:
        tmp = Path(directory)
        (tmp/'test.cpp').write_text(fixture)
        binary = tmp/'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-I'+str(cjson),
                        str(tmp/'test.cpp'), str(cjson/'cJSON.c'), '-o', str(binary)],
                       check=True, timeout=60)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)], env=env, timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
