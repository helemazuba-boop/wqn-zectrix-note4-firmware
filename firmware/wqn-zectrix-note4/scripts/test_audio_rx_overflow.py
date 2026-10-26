#!/usr/bin/env python3
"""Exercise the production duplex RX overflow callback and setup rollback on host.

The driver is a port; ISR placement/timing and physical samples still need HIL.
"""
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/services/audio_service.cpp').read_text()
    start = source.index('DRAM_ATTR std::atomic<uint32_t> g_duplex_rx_overflows')
    end = source.index('// Match the factory esp_codec_dev', start)
    fixture = (root / 'scripts/testdata/audio_rx_overflow.cpp').read_text()
    for marker, body in {
        '@@COUNTER@@': source[start:end],
        '@@CREATE@@': function(source, 'esp_err_t CreateAudioDuplexChannels('),
        '@@READ@@': function(source, 'uint32_t AudioDuplexRxOverflowCount()'),
    }.items():
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-rx-overflow-') as directory:
        unit, binary = Path(directory) / 'rx.cpp', Path(directory) / 'rx'
        unit.write_text(fixture)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-pthread', str(unit), '-o', str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
