#!/usr/bin/env python3
"""Run actual pre-sync manifest admission blocks with load/parse failure ports.

Exercises cache preservation policy, not SPIFFS power-loss behavior.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_audio_runtime import function
from test_binary_outbox_recovery import ERR, HEAP
from test_sync_retry import section


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=root)
    args = parser.parse_args()
    source = args.source_root.resolve()
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    note = (source / 'main/note_pack.cpp').read_text()
    start = note.index('class JsonDocument {')
    decoder = note[start:note.index('\n};', start) + 3]
    decoder += '\n' + '\n'.join(function(note, signature) for signature in
                                ('std::string GetOptionalString(', 'bool GetExactUint64(',
                                 'esp_err_t ParseNoteManifestPayload('))
    generated = []
    for domain, path in [('Note', 'main/note_cloud.cpp'), ('Problem', 'main/problem_cloud.cpp'),
                         ('Word', 'main/ui/word_cloud.cpp')]:
        text = (source / path).read_text()
        marker = f'Wqn{domain}PackManifest local_manifest;'
        if domain == 'Word':
            block = section(text, f'        wqn::{marker}', '        constexpr size_t kMaxManifestPagesPerSync =')
            generated.append('esp_err_t ProbeWord() {\nProbeResult result; result.result=prerequisite; '
                             'if (prerequisite != ESP_OK) result.message="install marker failed";\n'
                             + block + '\n(void)had_local_manifest; (void)manifest_content_changed; '
                             'if(result.result==ESP_OK) ++fetches; last_message=result.message; return result.result;\n}')
        else:
            text = function(text, f'esp_err_t Sync{domain}Packs(')
            block = section(text, f'    {marker}', '    // [snapshot-fix]')
            generated.append(f'esp_err_t Probe{domain}() {{\n{domain}PackSyncResult result; auto* out=&result;\n'
                             + block + '\n(void)had_local_manifest; (void)manifest_content_changed; '
                             'if(out->result==ESP_OK) ++fetches; last_message=out->message; return out->result;\n}')
    fixture = (root / 'scripts/testdata/manifest_load_preservation.cpp').read_text()
    fixture = fixture.replace('@@DECODER@@', decoder).replace('@@ADMISSION@@', '\n'.join(generated))
    macros = [f'-DWQN_{name}_SCHEMA_SHA256="host-only"' for name in
              ('DEVICE_CONTROL', 'WORD_STUDY', 'NOTE_STUDY', 'PROBLEM_STUDY')]
    with tempfile.TemporaryDirectory(prefix='note4-manifest-load-') as directory:
        tmp = Path(directory)
        (tmp / 'esp_err.h').write_text(ERR)
        (tmp / 'esp_heap_caps.h').write_text(HEAP)
        unit = tmp / 'test.cpp'
        unit.write_text(fixture)
        flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        obj = tmp / 'cjson.o'
        subprocess.run(['gcc', '-std=c99', *flags, '-I'+str(cjson), '-c', str(cjson / 'cJSON.c'),
                        '-o', str(obj)], check=True, timeout=30)
        binary = tmp / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I'+str(tmp), '-I'+str(source / 'main'), '-I'+str(cjson),
                        '-I'+str(source / 'components/device_protocol/include'), *macros,
                        str(unit), str(obj), '-o', str(binary)], check=True, timeout=60)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)], env=env, timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
