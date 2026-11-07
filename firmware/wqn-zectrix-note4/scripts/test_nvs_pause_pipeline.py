#!/usr/bin/env python3
"""Actual SDK NVS -> production pause reader -> production session loader. NOT HIL."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

import nvs_context_native as native
from test_nvs_context import IDF, IDF_PYTHON
from test_outbox_forensics import function


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('swallow-layer-error','hide-reader-error','ignore-scope-readiness'))
    parser.add_argument('--evidence-dir',type=Path)
    args=parser.parse_args()
    source=(root/'main/storage.cpp').read_text();word=(root/'main/word_study_store.cpp').read_text()
    if args.mutation:
        old,new={
            'swallow-layer-error':('ESP_RETURN_ON_ERROR(cursor_error, kTag, "read authoritative word pause cursor");','(void)cursor_error;'),
            'hide-reader-error':('if (loaded != ESP_OK) return reject(loaded);','if (loaded != ESP_OK) return false;'),
            'ignore-scope-readiness':('if (!wqn::IsWordDeckScopeReady()) return ESP_ERR_INVALID_STATE;','(void)0;'),
        }[args.mutation]
        if args.mutation=='ignore-scope-readiness':
            target=function(word,'esp_err_t LoadSessionTransaction(')
            if target.count(old)!=1:raise RuntimeError('pause scope mutation boundary drift')
            word=word.replace(target,target.replace(old,new))
        else:
            if word.count(old)!=1:raise RuntimeError('pause pipeline mutation boundary drift')
            word=word.replace(old,new)
    # Reuse only the explicit SDK public-handle/readonly-media bridge. The
    # application readers/loader below are extracted from this checkout.
    bridge=(root/'scripts/testdata/nvs_typed_presence.cpp').read_text()
    if bridge.count('namespace business {')!=1:raise RuntimeError('SDK bridge boundary drift')
    bridge=bridge.split('namespace business {',1)[0].replace('@@TYPED_HEADER@@','#include "runtime/nvs_typed_value.h"')
    fixture=(root/'scripts/testdata/nvs_pause_pipeline.cpp').read_text()
    start=word.index('constexpr uint32_t kSessionCursorMagic =')
    end=word.index('// [word-session-cursor-nvs] NVS keys',start)
    pieces={'@@HANDLE@@':function(source,'struct NvsHandle {')+';',
        '@@NVS_LOAD@@':function(source,'esp_err_t LoadWordSessionCursorNvs('),
        '@@NVS_SAVE@@':function(source,'esp_err_t SaveWordSessionCursorNvs('),
        '@@KEY@@':function(word,'const char* GetSessionCursorNvsKey('),
        '@@CRC@@':function(word,'uint32_t Crc32('),'@@RECORD@@':word[start:end],
        '@@READER@@':function(word,'bool ReadSessionCursorPaused('),
        '@@CONTEXT@@':function(word,'struct LoadSessionContext {')+';',
        '@@LOADER@@':function(word,'esp_err_t LoadSessionTransaction(')}
    if args.evidence_dir:
        args.evidence_dir.mkdir(mode=0o700,exist_ok=True)
        pins={"storage_sha256":hashlib.sha256((root/'main/storage.cpp').read_bytes()).hexdigest(),
              "word_store_sha256":hashlib.sha256((root/'main/word_study_store.cpp').read_bytes()).hexdigest(),
              "production_functions":{key:hashlib.sha256(value.encode()).hexdigest() for key,value in pieces.items()}}
        (args.evidence_dir/'source.json').write_text(json.dumps(pins,indent=2)+'\n')
    for marker,body in pieces.items():
        if fixture.count(marker)!=1:raise RuntimeError('pause fixture boundary drift')
        fixture=fixture.replace(marker,body)
    cases=[]
    with tempfile.TemporaryDirectory(prefix='wqn-pause-pipeline-') as temporary:
        path=Path(temporary);sdk=path/'sdk';native.build(IDF,sdk,sanitize=True)
        if args.evidence_dir:(args.evidence_dir/'sdk-build.json').write_bytes((sdk/'build.json').read_bytes())
        unit=path/'fixture.cpp';unit.write_text(bridge+fixture);binary=path/'fixture'
        flags=['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fno-pie',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
            '-I'+str(sdk),'-I'+str(root/'main'),'-I'+str(IDF/'components/nvs_flash/src'),
            '-I'+str(IDF/'components/nvs_flash/include'),'-I'+str(IDF/'components/nvs_flash/private_include'),
            '-I'+str(IDF/'components/esp_common/include'),'-I'+str(IDF/'components/esp_rom/include')]
        subprocess.run(['g++',*flags,str(unit),str(sdk/'crc.o'),*[str(sdk/(name+'.o')) for name in native.CPP],
                        '-no-pie','-Wl,--gc-sections','-o',str(binary)],check=True,timeout=45)
        sid='11111111-2222-4333-8444-555555555555'
        def cursor(paused=1,session=sid):
            payload=struct.pack('<IHH37s3s',0x57435552,1,paused,session.encode(),b'\0'*3)
            # Same NVS-independent journal CRC convention as the production
            # wrapper; actual ROM CRC is compiled for the C++ validation.
            import zlib
            return payload+struct.pack('<I',zlib.crc32(payload,0xffffffff)^0xffffffff)
        legacy=path/'legacy.cur';legacy_data=cursor()
        for kind in ('wrong','short','crc','paused','unpaused','missing','legacy','stale','disappeared','lookup-fault','unready'):
            rows=[('key','type','encoding','value'),('wqn','namespace','','')]
            data=cursor(0 if kind=='unpaused' else 1,'22222222-2222-4333-8444-555555555555' if kind=='stale' else sid)
            if kind=='short':data=data[:-1]
            if kind=='crc':data=data[:-1]+bytes([data[-1]^1])
            if kind not in ('missing','legacy'):
                rows += [(key,'data','string' if kind=='wrong' else 'hex2bin','opaque' if kind=='wrong' else data.hex())
                    for key in ('cur_seq','cur_rnd','cur_dic','cur_rev','cur_int','cur_shf','cur_mis')]
            csvpath=path/(kind+'.csv');image=path/(kind+'.bin')
            with csvpath.open('w') as stream:csv.writer(stream).writerows(rows)
            subprocess.run([IDF_PYTHON,str(IDF/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'),
                            'generate',str(csvpath),str(image),'0x4000','--version','2'],check=True,timeout=30,stdout=subprocess.DEVNULL)
            if kind=='missing':
                if legacy.exists():legacy.unlink()
            else:legacy.write_bytes(legacy_data)
            before=hashlib.sha256(image.read_bytes()).hexdigest()
            done=subprocess.run([str(binary),kind,str(image),str(legacy)],capture_output=True,text=True,timeout=30,
                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            output=done.stdout+done.stderr;print(output,end='',flush=True)
            match=re.search(r'NVS pause pipeline: (\d+) checks, (\d+) failures',output)
            if done.returncode not in (0,1) or match is None or re.search(r'AddressSanitizer|LeakSanitizer|runtime error:|\bERROR:',output,re.I):
                raise RuntimeError('pause pipeline infrastructure failure')
            if (done.returncode==0)!=(int(match[2])==0) or int(match[1])!=14:raise RuntimeError('pause pipeline completion mismatch')
            if before!=hashlib.sha256(image.read_bytes()).hexdigest():raise RuntimeError('NVS source changed')
            if kind!='missing' and legacy.read_bytes()!=legacy_data:raise RuntimeError('legacy cursor source changed')
            cases.append(dict(case=kind,checks=int(match[1]),failures=int(match[2]),returncode=done.returncode))
            if args.evidence_dir:
                args.evidence_dir.mkdir(mode=0o700,exist_ok=True)
                (args.evidence_dir/(kind+'.log')).write_text(output)
        if args.evidence_dir:(args.evidence_dir/'results.json').write_text(json.dumps(cases,indent=2)+'\n')
    count=sum(row['checks'] for row in cases);failed=sum(row['failures'] for row in cases)
    print(f'NVS pause pipeline total: {count} checks, {failed} failures (actual SDK/readers/loader; healthy journal ports; NOT HIL)',flush=True)
    return 1 if failed else 0


if __name__=='__main__':raise SystemExit(main())
