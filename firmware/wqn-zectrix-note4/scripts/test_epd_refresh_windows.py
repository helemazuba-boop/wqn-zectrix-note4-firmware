#!/usr/bin/env python3
"""Exercise production EPD write commands against a persistent-window SPI port.

This checks command order, target RAM pixels and failure propagation, not real
controller timing, BUSY behavior or the optical result on an e-paper panel.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    assert source.count(signature) == 1, signature
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
    raise ValueError(signature)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=root/'components/display_service/display_service.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    unit = (root/'scripts/testdata/epd_refresh_windows.cpp').read_text()
    pieces = {
        '@@RECT@@': function(source, 'struct DirtyRect {') + ';',
        '@@PACK@@': function(source, 'void Pack1bppToSsd2683('),
        '@@INTERLEAVE@@': function(source, 'void InterleavePreviousAndCurrent('),
        '@@SET_WINDOW@@': function(source, 'esp_err_t SetPartialWindow('),
        '@@HOT_PRELUDE@@': function(source, 'esp_err_t PreparePanelForHotFramebufferWrite()'),
        '@@WRITE_FRAME@@': function(source, 'esp_err_t SendFramebufferToPanel('),
        '@@WRITE_RECT@@': function(source, 'esp_err_t SendDirtyRectToPanel('),
    }
    for key, value in pieces.items():
        assert unit.count(key) == 1, key
        unit = unit.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='note4-epd-window-') as temporary:
        binary = Path(temporary)/'windows'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-x', 'c++', '-', '-o', str(binary)],
                       input=unit, text=True, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    main()
