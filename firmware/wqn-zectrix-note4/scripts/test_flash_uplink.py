#!/usr/bin/env python3
"""Run the production Flash queue/capture/sender/FINAL policies on host ports."""
import os
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def section(source, start, end):
    assert source.count(start) == 1, start
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', help='Compare only the capture loop at an immutable git revision')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/flash_session.cpp').read_text()
    baseline = None
    if args.baseline:
        baseline = subprocess.check_output([
            'git', 'show', args.baseline + ':firmware/wqn-zectrix-note4/main/flash_session.cpp'
        ], cwd=root, text=True)
    replacements = {
        '@@HEADER@@': section(source, 'struct AudioFrameHeader {', 'void WriteLE16('),
        '@@REGISTRATION@@': section(source, 'class FlashWsSendRegistration {', 'enum class FlashTerminalReason'),
        '@@ENCODE@@': '\n'.join(function(source, signature) for signature in (
            'std::string EncodeBase64(', 'void WriteLE16(', 'void WriteLE32(',
            'void EncodeV2AudioFrame(', 'void BuildV2AudioFrame(')),
        '@@CAPTURE@@': '\n'.join(function(source, signature) for signature in (
            'void CleanupStreamHardware()', 'void FinishAudioStreamingTask(',
            'void CaptureFlashPcmTurn(', 'void AudioStreamingTask(void* param)')),
        '@@SENDER@@': function(source, 'bool SendFlashUplinkPcm(') + '\n' + function(source, 'void FlashUplinkTask('),
        '@@WAIT@@': function(source, 'bool WaitForFlashUplink('),
        '@@RELEASE@@': function(source, 'void OnFlashButtonReleased(bool submit)'),
        '@@STOP@@': function(source, 'esp_err_t StopFlashSessionNow()\n{'),
    }
    fixture = (root / 'scripts/testdata/flash_uplink.cpp').read_text()
    if baseline:
        # The old capture body contains raw JSON strings whose brace counts
        # are unbalanced across string fragments. Use its immutable neighbor
        # boundary rather than the simple function parser for that body.
        replacements['@@CAPTURE@@'] = '\n'.join(function(baseline, signature) for signature in (
            'void CleanupStreamHardware()', 'void FinishAudioStreamingTask(')) + '\n' + section(
                baseline, 'void AudioStreamingTask(void* param)', 'void StartAudioStreaming()')
    for key, value in replacements.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='wqn-flash-uplink-') as directory:
        unit = Path(directory) / 'flash.cpp'
        unit.write_text(fixture)
        for mode in ((1,) if baseline else (1, 0)):
            binary = Path(directory) / f'flash-{mode}'
            subprocess.run([
                'g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                f'-DCONFIG_WQN_FLASH_PROTOCOL_V2={mode}',
                f'-DBASELINE_CAPTURE={int(baseline is not None)}',
                '-I', str(root / 'main'), str(unit), '-o', str(binary),
            ], check=True, timeout=60)
            environment = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}
            subprocess.run([str(binary)], check=True, timeout=30, env=environment)
    print('Flash baseline capture simulation passed' if baseline else
          'Flash production queue/capture/sender/FINAL host ports passed; not device HIL')


if __name__ == '__main__':
    main()
