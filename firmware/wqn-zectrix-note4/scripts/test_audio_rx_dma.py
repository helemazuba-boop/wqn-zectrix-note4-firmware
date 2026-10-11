#!/usr/bin/env python3
"""Exercise the production RX allocation path with a fragmented host heap.

The I2S adapter models allocation, cleanup and driver errors. It does not
validate ESP32 interrupt timing, recording quality or physical DMA behavior.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f'production audio boundary drift: {start}')
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/services/audio_service.cpp')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    constants = section(source, 'constexpr uint32_t kI2sDmaDescNum =',
                        'static_assert(sizeof(lldesc_t)')
    config = section(source, 'i2s_chan_config_t MakeChannelConfig(',
                     'i2s_std_config_t MakeStandardConfig(')
    create = section(source, 'esp_err_t CreateAudioRxChannel(',
                     'esp_err_t CreateAudioTxChannel(')
    fixture = (root / 'scripts/testdata/audio_rx_dma.cpp').read_text(encoding='utf-8')
    for marker, body in {'@@CONSTANTS@@': constants,
                         '@@CONFIG@@': config, '@@CREATE@@': create}.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-audio-rx-') as directory:
        unit, binary = Path(directory) / 'audio.cpp', Path(directory) / 'audio'
        unit.write_text(fixture, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(unit), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
