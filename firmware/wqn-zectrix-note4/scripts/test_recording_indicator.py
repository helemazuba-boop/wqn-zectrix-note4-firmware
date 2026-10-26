#!/usr/bin/env python3
"""Replay production recording LED gates and AudioService capability validation.

GPIO and scheduling ports are host substitutes; physical LED/HIL is separate.
"""
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def section(source, start, end):
    assert source.count(start) == 1, start
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    root = Path(__file__).resolve().parents[1]
    service = (root/'main/services/audio_service.cpp').read_text()
    capture = (root/'main/audio_capture.cpp').read_text()
    flash = (root/'main/flash_session.cpp').read_text()
    board = (root/'components/platform_note4/board_zectrix_note4.cpp').read_text()
    replacements = {
        '@@AUDIO_TYPES@@': section((root/'main/services/audio_service.h').read_text(), 'enum class AudioActivity', 'struct AudioSnapshot'),
        '@@DRIVER_GUARD@@': function(service, 'bool SessionMatches(') + '\n' + section(service, 'class DriverOperation {', 'esp_err_t RegisterResource('),
        '@@SET@@': function(service, 'esp_err_t SetAudioRecordingIndicator('),
        '@@END@@': function(service, 'esp_err_t EndActivity('),
        '@@HAL@@': function(board, 'esp_err_t SetNote4RecordingLed('),
        '@@CAPTURE@@': function(capture, 'void SetCaptureIndicatorLocked('),
        '@@FLASH@@': function(flash, 'void SetFlashCaptureIndicatorLocked(') + '\n' + function(flash, 'void SetErrorLocked(const std::string& message)\n'),
    }
    # Check the wiring around the tested production policies: no admission light;
    # ready comes after discarded warmup, and every terminal path clears it.
    worker = function(capture, 'void CaptureSession()')
    assert worker.index('warmup_bytes +=') < worker.index('SetCaptureIndicatorLocked(true)') < worker.index('g_audio.initialized = true')
    assert 'SetCaptureIndicatorLocked(false)' in function(capture, 'esp_err_t CleanupCaptureHardware(')
    assert 'SetCaptureIndicatorLocked(false)' in function(capture, 'esp_err_t StopAudioCapture(AudioCaptureChunk* chunk)')
    stream = function(flash, 'void CaptureFlashPcmTurn(uint32_t generation)')
    assert stream.index('for (int warmup') < stream.index('if (read_err != ESP_OK') < stream.index('SetFlashCaptureIndicatorLocked(true)')
    for signature in ['void FinishAudioStreamingTask(', 'void StopAudioStreaming()', 'void OnFlashButtonReleased(bool submit)', 'esp_err_t StopFlashSessionNow()\n{']:
        assert 'SetFlashCaptureIndicatorLocked(false)' in function(flash, signature)
    fixture = (root/'scripts/testdata/recording_indicator.cpp').read_text()
    for key, value in replacements.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='wqn-recording-led-') as temp:
        unit, binary = Path(temp)/'led.cpp', Path(temp)/'led'
        unit.write_text(fixture)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        str(unit),'-o',str(binary)],check=True,timeout=30)
        subprocess.run([str(binary)],check=True,timeout=15)


if __name__ == '__main__':
    main()
