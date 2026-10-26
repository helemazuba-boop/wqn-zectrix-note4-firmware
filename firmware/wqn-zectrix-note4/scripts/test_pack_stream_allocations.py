#!/usr/bin/env python3
"""Execute production pack stream callbacks with C++ allocation denied after Begin.

Real libc files, production path/hash rendering and download orchestration;
controlled RTOS, SHA and HTTP ports. Not ESP32 heap, crypto or SPIFFS HIL.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_audio_runtime import function
from test_storage_temp_lease import write_freertos_port


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, default=root / 'main')
    parser.add_argument('--measure-only', action='store_true')
    parser.add_argument('--mutation', choices=('rebuild-word', 'rebuild-note', 'rebuild-problem',
                        'heap-digest', 'ignore-digest', 'omit-paths', 'wrong-final-path',
                        'unlease-word', 'unlease-note', 'unlease-problem', 'release-path-early',
                        'unguard-reset', 'late-cache-guard'))
    args = parser.parse_args()
    sources = []
    for domain, member, stem, temp, final in (
        ('word', 'item', 'SafePackStem', 'TempPackPathForItem', 'PackPathForItem'),
        ('note', 'notebook', 'SafeNotePackStem', 'TempPackPathForNotebook', 'PackPathForNotebook'),
        ('problem', 'set', 'SafeProblemPackStem', 'TempPackPathForSet', 'PackPathForSet')):
        name = domain.title()
        text = (args.source_dir / f'{domain}_pack.cpp').read_text()
        safe_stem = function(text, f'std::string {stem}(')
        for key in ('kPackIdStemChars', 'kPackHashStemChars'):
            value = re.search(r'constexpr size_t ' + key + r' = (\d+);', text)
            assert value, key
            safe_stem = safe_stem.replace(key, value[1])
        factory = function(text, 'std::string PackPathForStem(')
        factory = factory.replace('PackPathForStem', name + 'FixturePackPathForStem')
        builders = []
        for signature in (temp, final):
            body = function(text, f'std::string {signature}(')
            body = body.replace('PackPathForStem', name + 'FixturePackPathForStem')
            body = body.replace('{', '{\n    ++path_calls;', 1)
            builders.append(body)
        start = text.index(f'enum class {name}PackStreamOperation :')
        end = text.index(f'\nbool {name}PackNeedsDownload(', start)
        body = text[start:end]
        guard = '''    runtime::PackStreamPathLease path_lease(context.temp_path.c_str());
    if (!path_lease) {
        return ESP_ERR_INVALID_STATE;
    }'''
        if args.mutation == 'unlease-' + domain:
            assert body.count(guard) == 1
            body = body.replace(guard, '')
        elif args.mutation == 'release-path-early':
            assert body.count(guard) == 1
            body = body.replace(guard, '    {\n' + guard + '\n    }')
        if args.mutation == 'rebuild-' + domain:
            old = 'const std::string& temp_path = context->temp_path;\n    const std::string& final_path = context->final_path;'
            assert body.count(old) == 1
            body = body.replace(old, f'const std::string temp_path = {temp}(*context->{member});\n    const std::string final_path = {final}(*context->{member});')
        if args.mutation == 'heap-digest':
            body = body.replace('std::array<char, 64> actual_sha = {};', "std::string actual_sha(64, '0');")
        if args.mutation == 'ignore-digest':
            body = body.replace(f'context->{member}->sha256.size() != actual_sha.size() ||\n                std::memcmp(actual_sha.data(), context->{member}->sha256.data(), actual_sha.size()) != 0', 'false')
        if args.mutation == 'omit-paths':
            body = body.replace(f'context.final_path = {final}({member});', '')
        if args.mutation == 'wrong-final-path':
            body = body.replace('const std::string& final_path = context->final_path;', 'const std::string& final_path = context->temp_path;')
        reset = function(text, f'esp_err_t Reset{name}PackStorageCacheRaw(')
        reset_guard = '''    if (wqn::runtime::HasActivePackStreamPath()) {
        return ESP_ERR_INVALID_STATE;
    }'''
        if args.mutation in ('unguard-reset', 'late-cache-guard'):
            assert reset.count(reset_guard) == 1
            reset = reset.replace(reset_guard, '')
            if args.mutation == 'late-cache-guard':
                reset = reset.replace('    DIR* directory = opendir(kStorageRoot);', reset_guard + '\n    DIR* directory = opendir(kStorageRoot);')
        sources.append('\n'.join([safe_stem, factory, *builders, reset, body]))
    fixture = (root / 'scripts/testdata/pack_stream_allocations.cpp').read_text()
    fixture = fixture.replace('@@PRODUCTION@@', '\n'.join(sources))
    fixture = fixture.replace('@@FILE_EXISTS@@', function((args.source_dir / 'word_pack.cpp').read_text(), 'bool FileExists('))
    with tempfile.TemporaryDirectory(prefix='note4-stream-allocation-') as directory:
        port = Path(directory)
        (port / 'fixture.cpp').write_text(fixture)
        write_freertos_port(port)
        binary = port / 'test'
        command = ['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-pthread',
                   '-I', str(port), '-I', str(root / 'main'), str(port / 'fixture.cpp'),
                   str(root / 'main/runtime/storage_temp_file_lease.cpp'),
                   str(root / 'main/runtime/pack_stream_path_lease.cpp'),
                   '-Wl,--wrap=fopen', '-Wl,--wrap=fclose', '-Wl,--wrap=fflush',
                   '-Wl,--wrap=fsync', '-Wl,--wrap=rename', '-o', str(binary)]
        subprocess.run(command, check=True, timeout=45)
        return subprocess.run([str(binary), str(port / 'files'), str(int(args.measure_only))], timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
