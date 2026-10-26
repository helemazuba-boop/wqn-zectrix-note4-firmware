#!/usr/bin/env python3
"""Exercise production bounded path admission with task-lock ports; not ESP32 SMP HIL."""
from pathlib import Path
import argparse
import subprocess
import tempfile

from test_storage_temp_lease import write_freertos_port


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('pointer-identity', 'serialize-all', 'leak-slot', 'wrong-slot'))
    args = parser.parse_args()
    body = (root / 'main/runtime/pack_stream_path_lease.cpp').read_text()
    replacements = {
        'pointer-identity': ('std::strcmp(g_paths[i], path) == 0', 'g_paths[i] == path'),
        'serialize-all': ('std::strcmp(g_paths[i], path) == 0', 'true'),
        'leak-slot': ('g_paths[slot_] = nullptr;', '(void)slot_;'),
        'wrong-slot': ('g_paths[slot_] = nullptr;', 'g_paths[0] = nullptr;'),
    }
    if args.mutation:
        old, new = replacements[args.mutation]
        assert body.count(old) == 1
        body = body.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='note4-path-lease-') as directory:
        port = Path(directory)
        write_freertos_port(port)
        (port / 'lease.cpp').write_text(body)
        binary = port / 'test'
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-pthread',
                        '-I', str(port), '-I', str(root / 'main'), str(port / 'lease.cpp'),
                        str(root / 'scripts/testdata/pack_stream_path_lease.cpp'),
                        '-o', str(binary)], check=True, timeout=40)
        return subprocess.run([str(binary)], timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
