#!/usr/bin/env python3
"""Exercise production Word/Note/Problem publication and rollback pruning.

Uses real libc files, manifest parsers and ESP-IDF cJSON with injected read/
allocation faults. Does not prove SPIFFS scheduling, power loss or device RAM.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_audio_runtime import function
from test_binary_outbox_recovery import ERR


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, default=root / 'main')
    parser.add_argument('--domain', choices=('all', 'word', 'note', 'problem'), default='all')
    parser.add_argument('--idf-path', type=Path,
                        default=Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')))
    parser.add_argument('--mutation', choices=('ignore-read-word', 'ignore-read-note', 'ignore-read-problem',
                        'ignore-parse', 'no-backup-stems', 'prune-current',
                        'skip-all', 'stat-gate'))
    args = parser.parse_args()
    cjson = args.idf_path / 'components/json/cJSON'
    if not (cjson / 'cJSON.c').is_file():
        raise SystemExit('ESP-IDF cJSON source missing; pass --idf-path')
    api = (args.source_dir / 'wqn_api.cpp').read_text()
    api_start = api.index('class JsonDocument {')
    api_document = api[api_start:api.index('\n};', api_start) + 3]
    codec = '\n'.join(function(api, signature) for signature in (
        'bool GetRequiredSafeUint64(', 'bool GetRequiredSafeUint32(', 'bool IsLowercaseSha256(',
        'bool GetSuccess(', 'std::string GetOptionalString(', 'esp_err_t ParseWordPackManifestItem(',
        'esp_err_t ParseWordPackManifestResponseImpl('))
    pieces = ['namespace word_codec {\n' + api_document + '\n' + codec + '\n}\n'
              'namespace wqn {\nusing word_codec::ParseWordPackManifestResponseImpl;\n'
              + function(api, 'esp_err_t ParseWordPackManifestResponse(') + '\n}\n']
    for domain, stem, add, suffix in (
            ('note', 'SafeNotePackStem', 'AddManifestNotebookStems', 'Note'),
            ('problem', 'SafeProblemPackStem', 'AddManifestSetStems', 'Problem'),
            ('word', 'SafePackStem', 'AddManifestPackStems', 'Word')):
        text = (args.source_dir / f'{domain}_pack.cpp').read_text()
        safe_stem = function(text, f'std::string {stem}(')
        for key in ('kPackIdStemChars', 'kPackHashStemChars'):
            value = re.search(r'constexpr size_t ' + key + r' = (\d+);', text)
            assert value, key
            safe_stem = safe_stem.replace(key, value[1])
        constants = '\n'.join(re.findall(
            r'^constexpr (?:uint64_t|size_t) (?:kPackSchemaVersion|kMaxPackEntries|kMaxPackBytes|kMaxStoredNotebooks|kMaxStoredSets|kMaxSetNameBytes)\s*=.*?;',
            text, flags=re.M | re.S))
        doc_start = text.index('class JsonDocument {')
        document = text[doc_start:text.index('\n};', doc_start) + 3]
        prune = function(text, 'void PruneUnreferencedPackFiles(' if domain == 'word'
                         else f'void PruneUnreferenced{suffix}PackFiles(')
        read_check = 'if (backup_result != ESP_OK ||'
        parse_check = ('wqn::ParseWordPackManifestResponse(backup_payload, &backup) != ESP_OK' if domain == 'word'
                       else f'Parse{suffix}ManifestPayload(backup_payload, &backup) != ESP_OK')
        if args.mutation == 'ignore-read-' + domain:
            assert prune.count(read_check) == 1
            prune = prune.replace(read_check, 'if (false ||')
            # The injected read failure retains valid bytes in its partial buffer.
        if args.mutation == 'ignore-parse':
            assert prune.count(parse_check) == 1
            prune = prune.replace(parse_check, 'false')
        if args.mutation == 'no-backup-stems':
            prune = prune.replace(f'{add}(backup, &retained_stems);', '')
        if args.mutation == 'prune-current':
            prune = prune.replace(f'{add}(current, &retained_stems);', '(void)current;')
        if args.mutation == 'skip-all':
            prune = prune.replace('{', '{\n    return;', 1)
        if args.mutation == 'stat-gate':
            prune = prune.replace('const esp_err_t backup_result = ReadWholeFile(kManifestBackupPath, &backup_payload);',
                'const esp_err_t backup_result = FileExists(kManifestBackupPath) ? ReadWholeFile(kManifestBackupPath, &backup_payload) : ESP_ERR_NOT_FOUND;')
        signatures = (
            'std::string GetOptionalString(', 'bool GetExactUint64(', 'bool FileExists(',
            'esp_err_t ReadWholeFile(', f'void {add}(',
            f'esp_err_t Parse{suffix}ManifestPayload(', f'esp_err_t ParseStored{suffix}Manifest(')
        if domain == 'word':
            constants += '\n' + re.search(r'^constexpr size_t kPackHashStemChars = .*?;', text, re.M)[0]
            constants += '\n' + re.search(r'^constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes\[\] = \{.*?\};', text, re.M | re.S)[0]
            signatures = ('bool FileExists(', 'esp_err_t ReadWholeFile(', f'void {add}(', 'esp_err_t ParsePackManifestFile(')
            document = ''
        functions = '\n'.join(function(text, signature) for signature in signatures)
        save = function(text, f'esp_err_t Save{suffix}PackManifestRaw(')
        pieces.append('namespace wqn {\n' + safe_stem + '\n}\nnamespace ' + domain + '_fixture {\n'
            + 'using namespace wqn;\n' + constants + '\n' + document + '\n' + functions
            + '\n' + prune + '\n' + save + '\n}\n')
    fixture = (root / 'scripts/testdata/pack_prune.cpp').read_text()
    assert fixture.count('@@PRODUCTION@@') == 1
    fixture = fixture.replace('@@PRODUCTION@@', '\n'.join(pieces))
    with tempfile.TemporaryDirectory(prefix='note4-pack-prune-') as temp:
        port = Path(temp)
        (port / 'esp_err.h').write_text(ERR)
        (port / 'fixture.cpp').write_text(fixture)
        flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer', '-fno-pie']
        subprocess.run(['gcc', '-std=c99', *flags, '-I', str(cjson), '-c',
                        str(cjson / 'cJSON.c'), '-o', str(port / 'cjson.o')], check=True, timeout=30)
        command = ['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                   *flags, '-no-pie', '-I', str(port), '-I', str(root / 'main'),
                   '-I', str(root / 'components/device_protocol/include'), '-I', str(cjson),
                   '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"',
                   '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"',
                   '-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"',
                   '-DWQN_PROBLEM_STUDY_SCHEMA_SHA256="host"',
                   str(port / 'fixture.cpp'), str(port / 'cjson.o'), '-o', str(port / 'test'),
                   '-Wl,--wrap=fopen,--wrap=fclose,--wrap=fread,--wrap=ferror,--wrap=stat,--wrap=remove']
        subprocess.run(command, check=True, timeout=45)
        return subprocess.run([str(port / 'test'), str(port / 'files'), args.domain], timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
