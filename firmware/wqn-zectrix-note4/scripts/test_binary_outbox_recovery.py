#!/usr/bin/env python3
"""Run production Word/Note journal code and real headers against libc fault ports.

This tests file/API boundaries and restart selection, not physical NOR power loss.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_word_outbox_quota import section

ERR = '''#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=257, ESP_ERR_INVALID_ARG=258;
constexpr int ESP_ERR_INVALID_STATE=259, ESP_ERR_INVALID_SIZE=260, ESP_ERR_NOT_FOUND=261;
constexpr int ESP_ERR_INVALID_RESPONSE=264, ESP_ERR_INVALID_CRC=265;
'''
HEAP = '''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1;
inline void* heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
'''


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--domain', choices=('word', 'note'))
    parser.add_argument('--source', type=Path)
    parser.add_argument('--mutation', choices=('delete-latest', 'ignore-recovery',
                                              'keep-failed-cache', 'skip-marker-repair'))
    args = parser.parse_args()
    if args.source and not args.domain:
        parser.error('--source requires --domain')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ unavailable')
    for domain in (args.domain,) if args.domain else ('word', 'note'):
        source = args.source or root / 'main' / ('word_study_store.cpp' if domain == 'word' else 'note_store.cpp')
        body = source.read_text()
        word = domain == 'word'
        if args.mutation:
            if args.mutation == 'delete-latest':
                old, new = ('std::rename(kOutboxPath, kOutboxRecoveryPath)',
                            'std::remove(kOutboxPath)')
            elif args.mutation == 'ignore-recovery':
                old, new = 'if (recovery_result == ESP_OK)', 'if (false)'
            elif args.mutation == 'keep-failed-cache':
                old, new = ('if (result != ESP_OK) g_outbox_cache_loaded = false;',
                            'if (false) g_outbox_cache_loaded = false;')
            else:
                if word:
                    parser.error('skip-marker-repair requires --domain note')
                old = '''    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionFromOutbox(*scan), kTag, "checkpoint before note suspend repair");
        ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "repair before note suspend append");
    }'''
                new = ''
            if old not in body:
                raise ValueError('mutation boundary drift')
            body = body.replace(old, new)
        records = section(body, '#pragma pack(push, 1)\nstruct SessionHeader {',
                          '\n// Every public operation below' if word else '\n// Parsed journal cached')
        validation = section(body, 'bool ValidMode(', '\nbool ValidPurpose(' if word else '\nbool ValidOrdering(')
        if not word:
            validation += '\n' + section(body, 'bool ValidAction(', '\nbool EncodeSession(')
        io = section(body, 'uint32_t RecordCrc(', '\nconstexpr size_t kOutboxMaxRecords =' if word else '\n// Bounded rolling forensic journal')
        if word:
            io += '\n' + section(body, 'constexpr size_t kOutboxMaxRecords =', '\n// The rejected journal is forensic data')
            io += '\n' + section(body, 'esp_err_t AppendOutboxRecords(\n', '\nbool SetSessionCursorOrdinal(')
        else:
            io += '\n' + section(body, 'esp_err_t CompactOutbox(', '\n// Advances a persisted note session')
        markers = ''
        if not word:
            markers = '''unsigned checkpoints=0;
esp_err_t CheckpointSessionFromOutbox(const OutboxScan&) { ++checkpoints; return ESP_OK; }
esp_err_t MaybeCompactCachedOutbox(OutboxScan*) { return ESP_OK; }
'''
            markers += section(body, 'struct RequestIdContext {', '\nesp_err_t QuarantineObservationTransaction(')
            markers += section(body, 'struct SuspendContext {', '\nesp_err_t SnapshotTransaction(')
        replacements = {
            '@@DOMAIN@@': f'#define WORD_DOMAIN {int(word)}\n#include "{ "word_study_store.h" if word else "note_store.h" }"\n',
            '@@RECORDS@@': records,
            '@@UTILITIES@@': section(body, 'uint32_t Crc32(', '\ntemplate <typename T>\nvoid AppendScalar('),
            '@@VALIDATE@@': validation,
            '@@IO@@': io,
            '@@MARKERS@@': markers,
        }
        fixture = (root / 'scripts/testdata/binary_outbox_recovery.cpp').read_text()
        for marker, value in replacements.items():
            assert fixture.count(marker) == 1, marker
            fixture = fixture.replace(marker, value)
        with tempfile.TemporaryDirectory(prefix=f'note4-{domain}-outbox-') as temporary:
            directory = Path(temporary)
            (directory / 'esp_err.h').write_text(ERR)
            (directory / 'esp_heap_caps.h').write_text(HEAP)
            unit, binary = directory / 'test.cpp', directory / 'test'
            unit.write_text(fixture)
            wraps = ('fopen', 'fclose', 'fwrite', 'fread', 'ferror', 'fflush', 'fsync', 'rename', 'remove')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"', '-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"',
                            '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"', '-I', str(directory),
                            '-I', str(root / 'main'), '-I', str(root / 'components/device_protocol/include'),
                            str(unit), '-o', str(binary), '-Wl,' + ','.join('--wrap=' + w for w in wraps)],
                           check=True, timeout=60)
            result = subprocess.run([str(binary), temporary], timeout=30)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
