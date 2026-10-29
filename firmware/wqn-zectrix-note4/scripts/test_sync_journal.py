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
    parser.add_argument('--before', help='read storage.cpp from a git revision')
    parser.add_argument('--mutation', choices=('destroy-backup', 'cache-failed-write','accept-opaque',
                        'erase-opaque-primary','erase-opaque-backup','fallback-opaque','oom-is-corrupt'))
    args = parser.parse_args()
    source = (subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/main/storage.cpp'],cwd=root,text=True)
              if args.before else (root / 'main/storage.cpp').read_text())
    io = section(source, 'esp_err_t JsonToString(', '\nvoid AddJournalContentState(')
    io = io.replace('std::rename(', 'JournalRename(').replace('std::remove(', 'JournalRemove(')
    io = io.replace('std::fwrite(', 'JournalWrite(').replace('std::fflush(', 'JournalFlush(')
    io = io.replace('std::fclose(', 'JournalClose(').replace('::fsync(', 'JournalSync(')
    pieces = [io, section(source, 'void AddJournalContentState(', '\nesp_err_t RemovePrototypeProblemFile('),
              'namespace wqn {\n' + section(source, 'esp_err_t LoadSyncJournal(', '\nnamespace {') + '\n}',
              section(source, 'std::string g_sync_journal_durable_payload;', '\n}  // namespace\n\nesp_err_t SaveSyncJournalThroughStorageService('),
              'namespace wqn {\n' + section(source, 'esp_err_t SaveSyncJournalThroughStorageService(', '\nesp_err_t LoadAutoSyncIntervalMinutes(') + '\n}']
    unit = (root / 'scripts/testdata/sync_journal.cpp').read_text().replace('@@PRODUCTION@@', '\n'.join(pieces))
    identity = (root / 'scripts/testdata/sync_journal_identity.cpp').read_text()
    unit = unit.replace('int main(int argc,char** argv) {',identity+'\nint main(int argc,char** argv) {')
    unit = unit.replace('    const std::string new_payload=ReadHost(primary);',
                        '    const std::string new_payload=ReadHost(primary);\n    JournalIdentityCases(old_payload,new_payload,old_state,new_state);')
    mutations = {
        'destroy-backup': ('if (primary_valid) {', 'if (primary_valid || read_result == ESP_OK) {'),
        'cache-failed-write': ('g_sync_journal_durable_payload_valid = false;\n    }',
                               'g_sync_journal_durable_payload = payload; g_sync_journal_durable_payload_valid = true;\n    }'),
        'accept-opaque': ('!wqn::protocol::JsonStringsRepresentable(payload.data(), payload.size());','false;'),
        'erase-opaque-primary': ('if (parse_result == ESP_ERR_NOT_SUPPORTED || parse_result == ESP_ERR_NO_MEM) return parse_result;',
                                 'if (parse_result == ESP_ERR_NO_MEM) return parse_result;'),
        'erase-opaque-backup': ('backup_read == ESP_OK && SyncJournalHasUnrepresentableStrings(backup_payload)','false'),
        'fallback-opaque': ('primary_result == ESP_ERR_NOT_SUPPORTED || primary_result == ESP_ERR_NO_MEM',
                            'primary_result == ESP_ERR_NO_MEM'),
        'oom-is-corrupt': ('? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;','? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_STATE;'),
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
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer','-fno-pie','-no-pie',
                        '-I'+str(root/'scripts/testdata/sync_host'), '-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'), '-I'+str(cjson),
                        '-x', 'c++', '-', str(cjson/'cJSON.c'), '-o', str(binary)],
                       input=unit, text=True, check=True, timeout=30)
        return subprocess.run([str(binary), tmp], timeout=30,env=dict(os.environ,
                              ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__ == '__main__':
    raise SystemExit(main())
