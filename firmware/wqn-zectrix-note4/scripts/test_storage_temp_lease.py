#!/usr/bin/env python3
"""Replay production capacity reclamation and all three pack download lifecycles.

Real libc files and controlled SPIFFS/HTTP/StorageService ports; not ESP32
filesystem, scheduling, electrical power-loss or clock-adjustment HIL.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def section(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('Production boundary drift: ' + start)
    return text[text.index(start):text.index(end, text.index(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--storage-source', type=Path, default=root / 'main/storage.cpp')
    parser.add_argument('--mutation', choices=('ignore-active', 'unguard-word', 'unguard-note',
                        'unguard-problem', 'release-early', 'lose-nesting', 'leak-lease'))
    args = parser.parse_args()
    source = args.storage_source.read_text()
    capacity = section(source, 'constexpr size_t kPackGcAttemptBytes =', '\nstruct NvsHandle {')
    if args.mutation == 'ignore-active':
        old = 'wqn::runtime::HasActiveStorageTempFileLease()'
        assert capacity.count(old) == 1
        capacity = capacity.replace(old, 'false')
    downloads = {}
    word_source = (root / 'main/word_pack.cpp').read_text()
    word_profile = section(word_source, 'struct WordPackAppendProfile {', 'struct WordPackStreamContext {')
    for domain in ('word', 'note', 'problem'):
        name = domain.title()
        source = (root / f'main/{domain}_pack.cpp').read_text()
        body = section(source, f'esp_err_t Append{name}PackStream(', f'\nbool {name}PackNeedsDownload(')
        guard = 'runtime::StorageTempFileLease temp_file_lease;'
        if args.mutation == 'unguard-' + domain:
            assert body.count(guard) == 1
            body = body.replace(guard, '')
        elif args.mutation == 'release-early':
            assert body.count(guard) == 1
            body = body.replace(guard, '{ ' + guard + ' }')
        downloads[f'@@{domain.upper()}@@'] = body
    lease = (root / 'main/runtime/storage_temp_file_lease.cpp').read_text()
    if args.mutation == 'lose-nesting':
        lease = lease.replace('fetch_add(1, std::memory_order_acq_rel)', 'store(1, std::memory_order_release)')
        lease = lease.replace('fetch_sub(1, std::memory_order_acq_rel)', 'store(0, std::memory_order_release)')
    if args.mutation == 'leak-lease':
        lease = lease.replace('g_temp_file_leases.fetch_sub(1, std::memory_order_acq_rel);', '')
    fixture = (root / 'scripts/testdata/storage_temp_lease.cpp').read_text()
    for marker, body in {'@@CAPACITY@@': capacity, '@@WORD_PROFILE@@': word_profile, **downloads}.items():
        assert fixture.count(marker) == 1
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='note4-temp-lease-') as directory:
        port = Path(directory)
        (port / 'fixture.cpp').write_text(fixture)
        (port / 'lease.cpp').write_text(lease)
        binary = port / 'test'
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-pthread',
                        '-I', str(root / 'main'), str(port / 'fixture.cpp'), str(port / 'lease.cpp'),
                        '-Wl,--wrap=time', '-o', str(binary)], check=True, timeout=45)
        return subprocess.run([str(binary), str(port / 'files')], timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
