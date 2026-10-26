#!/usr/bin/env python3
"""Execute production I2S adapters and caller timeout expressions at 100/1000 Hz.

Driver/RTOS ports are host substitutes. This checks API units, error/partial
completion propagation and operation lifetime, not PCM continuity or device HIL.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_audio_runtime import function


def timeout_expressions(source):
    """Read the final argument from every production audio channel call."""
    import re
    expressions = []
    for match in re.finditer(r'wqn::services::(Read|Write)AudioChannel\(', source):
        depth = 1
        start = argument = match.end()
        for end in range(start, len(source)):
            char = source[end]
            if char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
                if depth == 0:
                    expressions.append((match.group(1), source[argument:end].strip()))
                    break
            elif char == ',' and depth == 1:
                argument = end + 1
        else:
            raise ValueError('unclosed audio channel call')
    return expressions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('caller-ticks', 'driver-ticks'))
    parser.add_argument('--source-root', type=Path,
                        help='read production code from an exported baseline')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = args.source_root or root
    service = (sources / 'main/services/audio_service.cpp').read_text()
    read = function(service, 'esp_err_t ReadAudioChannel(')
    write = function(service, 'esp_err_t WriteAudioChannel(')
    if args.mutation == 'driver-ticks':
        read = read.replace('bytes_read, timeout_ms);',
                            'bytes_read, pdMS_TO_TICKS(timeout_ms));')
        write = write.replace('bytes_written, timeout_ms);',
                              'bytes_written, pdMS_TO_TICKS(timeout_ms));')
        if 'pdMS_TO_TICKS(timeout_ms)' not in read:
            raise ValueError('driver mutation did not apply')
    probes = []
    callers = (
        ('audio_capture.cpp', [('Read', 1000), ('Read', 1000)]),
        ('flash_session.cpp', [('Write', 250), ('Read', 200),
                               ('Read', 100), ('Read', 200)]),
        ('audio_selftest.cpp', [('Read', 1000)]),
    )
    for filename, expected in callers:
        found = timeout_expressions((sources / 'main' / filename).read_text())
        if len(found) != len(expected):
            raise ValueError(f'audio call inventory changed: {filename}')
        for (kind, expression), (expected_kind, milliseconds) in zip(found, expected):
            if kind != expected_kind:
                raise ValueError(f'audio call order changed: {filename}')
            if args.mutation == 'caller-ticks':
                expression = f'pdMS_TO_TICKS({expression})'
            probes.append(f'Probe{kind}({expression}, {milliseconds});')
    template = (root / 'scripts/testdata/audio_channel_timeout.cpp').read_text()
    for marker, body in {'@@READ@@': read, '@@WRITE@@': write,
                         '@@CALLERS@@': '\n    '.join(probes)}.items():
        if template.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        template = template.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-audio-timeout-') as temporary:
        directory = Path(temporary)
        (directory / 'freertos').mkdir()
        (directory / 'power').mkdir()
        (directory / 'esp_err.h').write_text(
            '#pragma once\nusing esp_err_t = int;\n')
        (directory / 'freertos/FreeRTOS.h').write_text(
            '#pragma once\n#include <cstdint>\nusing TickType_t = uint32_t;\n'
            '#define pdMS_TO_TICKS(ms) '
            '(static_cast<TickType_t>((uint64_t(ms) * FIXTURE_TICK_RATE_HZ) / 1000))\n')
        (directory / 'power/sleep_protocol.h').write_text(
            '#pragma once\nnamespace wqn::power { struct PrepareSleepCommand; }\n')
        unit = directory / 'timeout.cpp'
        unit.write_text(template)
        for tick_rate in (100, 1000):
            binary = directory / f'timeout-{tick_rate}'
            subprocess.run([
                compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                f'-DFIXTURE_TICK_RATE_HZ={tick_rate}', '-I', str(directory),
                '-I', str(sources / 'main'), str(unit), '-o', str(binary)],
                check=True, timeout=30)
            result = subprocess.run([str(binary)], timeout=10)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
