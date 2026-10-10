#!/usr/bin/env python3
"""Run production journal codec/rotation on a temporary host FS. NOT NOR HIL."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_sync_retry import section


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('destroy-backup', 'cache-failed-write'))
    args = parser.parse_args()
    source = (root / 'main/storage.cpp').read_text()
    io = section(source, 'esp_err_t JsonToString(', '\nvoid AddJournalContentState(')
    io = io.replace('std::rename(', 'JournalRename(').replace('std::remove(', 'JournalRemove(')
    io = io.replace('std::fwrite(', 'JournalWrite(').replace('std::fflush(', 'JournalFlush(')
    io = io.replace('std::fclose(', 'JournalClose(').replace('::fsync(', 'JournalSync(')
    pieces = [io, section(source, 'void AddJournalContentState(', '\nesp_err_t RemovePrototypeProblemFile('),
              'namespace wqn {\n' + section(source, 'esp_err_t LoadSyncJournal(', '\nnamespace {') + '\n}',
              section(source, 'std::string g_sync_journal_durable_payload;', '\n}  // namespace\n\nesp_err_t SaveSyncJournalThroughStorageService('),
              'namespace wqn {\n' + section(source, 'esp_err_t SaveSyncJournalThroughStorageService(', '\nesp_err_t LoadAutoSyncIntervalMinutes(') + '\n}']
    unit = (root / 'scripts/testdata/sync_journal.cpp').read_text().replace('@@PRODUCTION@@', '\n'.join(pieces))
    mutations = {
        'destroy-backup': ('if (primary_valid) {', 'if (primary_valid || read_result == ESP_OK) {'),
        'cache-failed-write': ('g_sync_journal_durable_payload_valid = false;\n    }',
                               'g_sync_journal_durable_payload = payload; g_sync_journal_durable_payload_valid = true;\n    }'),
    }
    if args.mutation:
        needle, replacement = mutations[args.mutation]
        if unit.count(needle) != 1:
            raise SystemExit('FAIL: mutation boundary drift')
        unit = unit.replace(needle, replacement)
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    if not (cjson / 'cJSON.c').is_file():
        raise SystemExit('FAIL: activate project ESP-IDF for cJSON')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-sync-journal-') as tmp:
        binary = Path(tmp) / 'test'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I'+str(root/'scripts/testdata/sync_host'), '-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'), '-I'+str(cjson),
                        '-x', 'c++', '-', str(cjson/'cJSON.c'), '-o', str(binary)],
                       input=unit, text=True, check=True, timeout=30)
        return subprocess.run([str(binary), tmp], timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
