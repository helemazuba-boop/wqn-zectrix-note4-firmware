#!/usr/bin/env python3
"""Run production audio token, clock and idle-cleanup code with host RTOS seams.

The storage owner and caller are exercised with controlled completion ordering.
This does not validate ESP32 cache-disabled execution or device scheduling.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError(f'production boundary drift: {signature}')
    start = source.index(signature)
    body = source.index('{', start)
    depth = 0
    for end in range(body, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError(f'unclosed function: {signature}')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('direct-nvs', 'clock-every-tick',
                                              'ignore-active-use', 'abandon-running'))
    args = parser.parse_args()
    storage = (root / 'main/storage.cpp').read_text()
    service = (root / 'main/services/storage_service.cpp').read_text()
    transport = (root / 'main/stdpro_ws_transport.cpp').read_text()
    clock = (root / 'main/ai_session.cpp').read_text()
    bodies = {
        '@@STORAGE_TYPES@@': service[service.index('constexpr char kTag[] ='):
                                     service.index('StaticQueue_t g_background_queue_storage;')],
        '@@STORAGE_OWNER@@': function(service, 'void StorageServiceTask(void*)'),
        '@@STORAGE_CALLER@@': function(service, 'esp_err_t ExecuteStorageTransactionInternal('),
        '@@STORAGE_FOREGROUND@@': function(service, 'esp_err_t ExecuteForegroundStorageTransaction('),
        '@@TOKEN@@': function(storage, 'esp_err_t LoadAccessTokenFromStorageService('),
        '@@CLOCK@@': function(clock, 'void SetAiRecordingLabel(int32_t elapsed_ms)'),
        '@@IDLE@@': function(transport, 'void TryIdleDisconnect()') + '\n' +
                     function(transport, 'void RequestIdleDisconnect()'),
    }
    if args.mutation == 'direct-nvs':
        bodies['@@TOKEN@@'] = ('esp_err_t LoadAccessTokenFromStorageService(std::string* token) '
                                '{ return LoadAccessToken(token); }')
    elif args.mutation == 'clock-every-tick':
        bodies['@@CLOCK@@'] = bodies['@@CLOCK@@'].replace(
            'if (g_state.toast_label != buf)', 'if (true)')
    elif args.mutation == 'abandon-running':
        bodies['@@STORAGE_CALLER@@'] = bodies['@@STORAGE_CALLER@@'].replace(
            'xSemaphoreTake(completion->semaphore, portMAX_DELAY);',
            'return ESP_ERR_TIMEOUT;')
    gate = (root / 'main/voice_idle_disconnect.h').read_text()
    if args.mutation == 'ignore-active-use':
        gate = gate.replace('users_.load(std::memory_order_acquire) != 0 || ', '')
    fixture = (root / 'scripts/testdata/audio_runtime.cpp').read_text()
    for marker, body in bodies.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-audio-runtime-') as directory:
        unit, binary = Path(directory) / 'runtime.cpp', Path(directory) / 'runtime'
        (Path(directory) / 'voice_idle_disconnect.h').write_text(gate)
        unit.write_text(fixture)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(root / 'main'),
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
