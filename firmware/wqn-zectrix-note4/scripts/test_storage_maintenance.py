#!/usr/bin/env python3
"""Compile real read-only NVS evidence and typed cursor reader with fault ports."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_binary_outbox_recovery import ERR
from test_outbox_forensics import function


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('ignore-nvs-size','truncate-size','write-open'))
    args=parser.parse_args()
    source=(root/'main/storage.cpp').read_text()
    if args.mutation=='ignore-nvs-size':
        body=function(source,'esp_err_t LoadWordSessionCursorNvs(')
        if body.count('return ESP_ERR_INVALID_SIZE;')!=2:raise ValueError('typed NVS mutation boundary drift')
        source=source.replace(body,body.replace('return ESP_ERR_INVALID_SIZE;','return ESP_OK;'))
    elif args.mutation=='truncate-size':
        old='cursor.stored_bytes = measured;'
        if source.count(old)!=1:raise ValueError('raw-size mutation boundary drift')
        source=source.replace(old,'cursor.stored_bytes = std::min(measured, cursor.bytes.size());')
    elif args.mutation=='write-open':
        body=function(source,'esp_err_t InspectWordRecoveryNvsOnStorageOwner(')
        if body.count('NVS_READONLY')!=1:raise ValueError('read-only mutation boundary drift')
        source=source.replace(body,body.replace('NVS_READONLY','NVS_READWRITE'))
    fixture=(root/'scripts/testdata/storage_maintenance.cpp').read_text()
    for key,value in {
        '@@LOOKUP@@':function((root/'main/word_study_store.cpp').read_text(),'const char* GetSessionCursorNvsKey('),
        '@@CONSTANTS@@':'\n'.join(re.search(r'constexpr const char\* '+name+r' = [^;]+;',source).group(0)
            for name in ['kDeckScopeMarkerKey','kDeckScopeGenKey']),
        '@@HANDLE@@':function(source,'struct NvsHandle {')+';',
        '@@LOAD@@':function(source,'esp_err_t LoadWordSessionCursorNvs('),
        '@@INSPECT@@':function(source,'esp_err_t InspectWordRecoveryNvsOnStorageOwner('),
    }.items():
        if fixture.count(key)!=1:raise ValueError('maintenance fixture boundary drift')
        fixture=fixture.replace(key,value)
    with tempfile.TemporaryDirectory(prefix='wqn-maintenance-nvs-') as temp:
        path=Path(temp);(path/'esp_err.h').write_text(ERR)
        (path/'nvs.h').write_text('#pragma once\n')  # API ports are declared in the fixture.
        (path/'fixture.cpp').write_text(fixture)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
            '-fno-pie','-no-pie','-I'+str(path),'-I'+str(root/'main'),
            '-I'+str(root/'components/device_protocol/include'),
            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"','-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"',str(path/'fixture.cpp'),
            '-o',str(path/'test')],check=True,timeout=45)
        return subprocess.run([str(path/'test')],timeout=15).returncode


if __name__=='__main__':raise SystemExit(main())
