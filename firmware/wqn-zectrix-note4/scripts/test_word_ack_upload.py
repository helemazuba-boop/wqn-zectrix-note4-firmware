#!/usr/bin/env python3
"""Run the production upload loop against host transport/storage fault seams.

No network, flash, credentials or device are accessed. This does not prove HIL
durability; the journal implementation has separate replay/libc tests.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/services/sync_service.cpp')
    parser.add_argument('--mutation', choices=('skip-empty-maintenance',))
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    start = 'WordOutboxUploadState UploadPendingWordObservations('
    end = '// Uploads pending problem verdicts'
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit('FAIL: production upload boundary drift')
    body = source[source.index(start):source.index(end, source.index(start))]
    if args.mutation == 'skip-empty-maintenance':
        needle = 'wqn::MaintainWordObservationOutbox(\n            maintenance_gate, &maintenance_deferred)'
        if body.count(needle) != 1:
            raise SystemExit('FAIL: maintenance retry mutation boundary drift')
        body = body.replace(needle, 'ESP_OK')
    fixture = (root / 'scripts/testdata/word_ack_upload.cpp').read_text(encoding='utf-8')
    if fixture.count('@@UPLOAD@@') != 1:
        raise SystemExit('FAIL: fixture marker drift')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    if not (cjson / 'cJSON.c').is_file():
        raise SystemExit('FAIL: real SDK cJSON unavailable; activate ESP-IDF')
    with tempfile.TemporaryDirectory(prefix='wqn-ack-upload-') as directory:
        unit, binary = Path(directory) / 'upload.cpp', Path(directory) / 'upload'
        (Path(directory)/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=0x102,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_SIZE=0x104,ESP_ERR_INVALID_RESPONSE=0x108,ESP_ERR_NOT_FOUND=0x105;\n')
        (Path(directory)/'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\ninline void* heap_caps_malloc(size_t n,int){return malloc(n);}\ninline void heap_caps_free(void*p){free(p);}\n')
        (Path(directory)/'esp_check.h').write_text('#pragma once\n#include "esp_err.h"\n#define ESP_RETURN_ON_ERROR(x,...) do { int e=(x); if(e!=ESP_OK) return e; } while(0)\n')
        unit.write_text(fixture.replace('@@UPLOAD@@', body), encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I'+directory,'-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'),
                        '-I'+str(cjson),
                        '-DWQN_WORD_STUDY_SCHEMA_SHA256="fixture"','-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="fixture"',
                        str(unit),str(root/'components/device_protocol/word_study.cpp'),
                        str(cjson / 'cJSON.c'), '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
