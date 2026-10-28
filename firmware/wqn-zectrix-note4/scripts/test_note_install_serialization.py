#!/usr/bin/env python3
"""Interleave the real full/single Note installers at every blocking boundary.

Storage/network are host ports; this does not test device scheduling or power loss.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_audio_runtime import function
from test_binary_outbox_recovery import ERR, HEAP


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=root)
    args = parser.parse_args()
    source = args.source_root.resolve()
    production = (source / 'main/note_cloud.cpp').read_bytes()
    merge = function((source / 'main/note_pack.cpp').read_text(), 'esp_err_t MergeNotePackManifestDelta(')
    fixture = (root / 'scripts/testdata/note_install_serialization.cpp').read_text()
    assert fixture.count('@@MERGE@@') == 1
    macros = [f'-DWQN_{name}_SCHEMA_SHA256="host-only"' for name in
              ('DEVICE_CONTROL', 'WORD_STUDY', 'NOTE_STUDY', 'PROBLEM_STUDY')]
    with tempfile.TemporaryDirectory(prefix='note4-install-') as directory:
        tmp = Path(directory)
        (tmp / 'esp_err.h').write_text(ERR)
        (tmp / 'esp_heap_caps.h').write_text(HEAP)
        (tmp / 'esp_log.h').write_text(
            '#pragma once\n#define ESP_LOGI(...) ((void)0)\n'
            '#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n'
            '#define esp_err_to_name(x) "host"\n')
        (tmp / 'services').mkdir()
        (tmp / 'services/sync_service.h').write_text(
            '#pragma once\n#include "device_protocol/v3.h"\n'
            'namespace wqn::services {protocol::v3::RequestMetadata MakeDeviceRequestMetadata();}\n')
        (tmp / 'storage.h').write_text(
            '#pragma once\n#include <cstddef>\n#include "esp_err.h"\n'
            'namespace wqn {esp_err_t EnsurePackDownloadCapacity(size_t);}\n')
        (tmp / 'note_cloud.cpp').write_bytes(production)
        (tmp / 'fixture.cpp').write_text(fixture.replace('@@MERGE@@', merge))
        binary = tmp / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-I'+str(tmp), '-I'+str(source / 'main'),
                        '-I'+str(source / 'components/device_protocol/include'), *macros,
                        str(tmp / 'note_cloud.cpp'), str(tmp / 'fixture.cpp'), '-o', str(binary)],
                       check=True, timeout=60)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)], env=env, timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
