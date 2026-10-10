#!/usr/bin/env python3
"""Execute production retry admission/helpers with host seams; NOT HIL."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1:
        raise SystemExit(f'FAIL: non-unique source boundary: {start}')
    pos = source.index(start)
    return source[pos:source.index(end, pos + len(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('reset-on-auto', 'wrap-attempt'))
    args = parser.parse_args()
    source = (root / 'main/services/sync_service.cpp').read_text()
    pieces = [
        section(source, 'constexpr uint32_t kFullSyncRetryLadderMs[] =', '// RTC retention'),
        section(source, 'enum FullSyncReason :', '\nstd::atomic<uint32_t>'),
        section(source, 'constexpr uint32_t kWordOutboxRetryBaseMs =', '// Retry ladder reaches'),
        section(source, 'void ClearFullSyncRetry()', '\nbool BootFullSyncDue('),
        section(source, 'uint32_t FullSyncFailureRetryMs(', '\nvoid SyncServiceTask('),
    ]
    for domain in ('Word', 'Note', 'Problem'):
        pieces.append(section(source, f'void Reset{domain}OutboxRetryBackoff()',
                              f'\nTickType_t {domain}OutboxRetryWaitDelay()'))
    admission = section(source, '        bool full_requested =', '\n        bool word_outbox_requested =')
    if args.mutation == 'reset-on-auto':
        needle = 'if ((admitted_full_reasons &\n             (kFullSyncManual | kFullSyncCredentials)) != 0) {'
        if admission.count(needle) != 1:
            raise SystemExit('FAIL: admission mutation boundary drift')
        admission = admission.replace(needle, 'if (full_requested) {')
    pieces.append('void AdmitFull(uint32_t admitted_full_reasons, bool retry_due, bool periodic_due) {\n'
                  + admission + '\n    (void)full_requested;\n}')
    unit = (root / 'scripts/testdata/sync_retry.cpp').read_text().replace('@@PRODUCTION@@', '\n'.join(pieces))
    if args.mutation == 'wrap-attempt':
        needle = 'if (g_full_sync_retry_attempts < UINT8_MAX) {\n        ++g_full_sync_retry_attempts;\n    }'
        if unit.count(needle) != 1:
            raise SystemExit('FAIL: saturation mutation boundary drift')
        unit = unit.replace(needle, '++g_full_sync_retry_attempts;')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-sync-retry-') as tmp:
        binary = Path(tmp) / 'test'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-x', 'c++', '-', '-o', str(binary)], input=unit, text=True,
                       check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
