#!/usr/bin/env python3
"""Run the production mount entry against SPIFFS API outcomes. NOT HIL."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', help='test storage.cpp from an immutable git revision')
    parser.add_argument('--mutation', choices=('forget-mount-proof',))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (subprocess.check_output([
        'git', 'show', f'{args.before}:firmware/wqn-zectrix-note4/main/storage.cpp'
    ], cwd=root, text=True) if args.before else (root/'main/storage.cpp').read_text())
    start = source.index('esp_err_t InitStoragePartition()\n{')
    end = source.index('\n}', start) + 2
    function = source[start:end]
    if args.mutation:
        proof = ' &&\n        esp_spiffs_mounted(kStoragePartitionLabel)'
        if function.count(proof) != 1:
            raise SystemExit('FAIL: mount guard mutation boundary drift')
        function = function.replace(proof, '')
    fixture = (root/'scripts/testdata/storage_mount_guard.cpp').read_text()
    if fixture.count('// PRODUCTION_FUNCTION') != 1:
        raise SystemExit('FAIL: mount guard fixture boundary drift')
    with tempfile.TemporaryDirectory(prefix='wqn-mount-guard-') as directory:
        path = Path(directory)
        cpp, binary = path/'mount.cpp', path/'mount'
        cpp.write_text(fixture.replace('// PRODUCTION_FUNCTION', function))
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wformat=2', str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
