#!/usr/bin/env python3
"""Replay production STD input/decision gates with injected audio and HTTP ports.

No RTOS scheduling, TLS memory, microphone or panel HIL is claimed.
"""
from pathlib import Path
import subprocess
import re
import hashlib
import tempfile
from test_audio_runtime import function


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/ai_session.cpp').read_text()
    source = source[:source.rindex('\n#else')]
    model = (root / 'main/ui_model.h').read_text()
    model = model[model.index('enum class AiSessionStatus'):model.index('enum class TodoSyncStatus')]
    names = ['SubmitDispatchedLocked', 'FinishSubmitTaskLocked', 'SetStateLocked',
             'HasEffectiveSpeech', 'OnStdTranscribeEvent', 'TranscribeInput',
             'ReceiveInteractionLocked', 'PumpInteractionReply', 'StopAiRecordingAndSubmit', 'ConfirmAiPrompt',
             'CancelAiPrompt', 'ReplyAiInteraction']
    functions = '\n'.join(function(source, re.search(r'^(?:bool|void|esp_err_t) '+name+r'\(', source, re.M).group()) for name in names)
    fixture = (root / 'scripts/testdata/std_ai.cpp').read_text()
    fixture = fixture.replace('@@MODEL@@', model).replace('@@FUNCTIONS@@', functions)
    flags = []
    for domain, schema in [('DEVICE_CONTROL','device-control-v3'), ('NOTE_STUDY','note-study-v1'), ('PROBLEM_STUDY','problem-study-v1'), ('WORD_STUDY','word-study-v1')]:
        path = next((root/'contracts').rglob(schema+'.schema.json'))
        flags.append(f'-DWQN_{domain}_SCHEMA_SHA256="{hashlib.sha256(path.read_bytes()).hexdigest()}"')
    with tempfile.TemporaryDirectory(prefix='wqn-std-ai-') as temp:
        temp = Path(temp)
        unit = temp / 'std.cpp'
        unit.write_text(fixture)
        binary = temp / 'std'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        *flags, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I'+str(root/'scripts/testdata/sync_host'), '-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'),
                        str(unit), '-o', str(binary)], check=True, timeout=45)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == '__main__':
    main()
