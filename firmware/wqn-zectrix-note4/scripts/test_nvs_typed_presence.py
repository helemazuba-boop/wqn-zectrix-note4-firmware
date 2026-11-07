#!/usr/bin/env python3
"""Production storage/boot functions against actual SDK NVS and CRC. NOT HIL."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import hashlib
import re
import zlib
import struct

import nvs_context_native as native
from test_nvs_context import IDF, IDF_PYTHON
from test_outbox_forensics import function


def build(root,path,storage,schema,mutation=None):
    sdk=path/'sdk';native.build(IDF,sdk,sanitize=True)
    source=storage.read_text();boot=schema.read_text()
    typed=(root/'main/runtime/nvs_typed_value.h').read_text() if (root/'main/runtime/nvs_typed_value.h').exists() else None
    if mutation:
        if mutation=='hide-wrong-type':
            old='if (observed != expected) return ESP_ERR_NVS_TYPE_MISMATCH;'
            if typed is None or typed.count(old)!=1:raise RuntimeError('typed presence mutation boundary drift')
            typed=typed.replace(old,'if (observed != expected) return ESP_OK;')
        else:
            signature,old,new={
                'ignore-disappearance':('esp_err_t LoadU64FromNvs(',
                    'ESP_RETURN_ON_ERROR(result, kTag, "read NVS u64");',
                    'if (result == ESP_ERR_NVS_NOT_FOUND) { return ESP_OK; }\n    ESP_RETURN_ON_ERROR(result, kTag, "read NVS u64");'),
                'overwrite-cursor':('esp_err_t SaveWordSessionCursorNvs(',
                    'runtime::ReadNvsTypedPresence(nvs.handle, key, NVS_TYPE_BLOB, &present)',
                    '(present = false, ESP_OK)'),
                'hide-boot-intent':('esp_err_t ReadResetIntent(',
                    'wqn::runtime::ReadNvsTypedPresence(nvs.handle, kResetIntentKey, NVS_TYPE_BLOB, &present)',
                    '(present = false, ESP_OK)'),
                'drop-legacy-volume':('esp_err_t LoadVolumePercent(',
                    'type == NVS_TYPE_U8','false'),
                'accept-embedded-nul':('esp_err_t LoadStringFromNvs(',
                    "required_size != measured_size || buffer.back() != '\\0' ||\n        std::memchr(buffer.data(), '\\0', measured_size - 1) != nullptr",
                    'required_size != measured_size'),
            }[mutation]
            target=boot if mutation=='hide-boot-intent' else source
            body=function(target,signature)
            if body.count(old)!=1:raise RuntimeError('typed NVS mutation boundary drift: '+mutation)
            target=target.replace(body,body.replace(old,new))
            if mutation=='hide-boot-intent':boot=target
            else:source=target
    business=['struct NvsHandle {','esp_err_t LoadStringFromNvs(','esp_err_t LoadU64FromNvs(',
        'esp_err_t LoadBlobFromNvs(','struct DeckScopeText {','esp_err_t LoadDeckScopeTextLocked(']
    public=['esp_err_t SaveWordSessionCursorNvs(','esp_err_t ClearWordSessionCursorNvs(',
        'esp_err_t LoadWordSessionCursorNvs(','esp_err_t InspectWordRecoveryNvsOnStorageOwner(','esp_err_t LoadVolumePercent(']
    def bodies(signatures):return '\n'.join(function(source,signature)+(';' if signature.startswith('struct ') else '') for signature in signatures)
    header=root/'main/runtime/nvs_typed_value.h'
    if typed is not None:
        private=path/'headers/runtime';private.mkdir(parents=True)
        (private/'nvs_typed_value.h').write_text(typed)
    fixture=(root/'scripts/testdata/nvs_typed_presence.cpp').read_text()
    for marker,value in {'@@TYPED_HEADER@@':'#include "runtime/nvs_typed_value.h"' if header.exists() else '',
        '@@BUSINESS@@':bodies(business),'@@PUBLIC@@':bodies(public),
        '@@BOOT@@':'\n'.join(function(boot,signature) for signature in ['esp_err_t ReadSchemaMarker(','esp_err_t ReadResetIntent('])}.items():
        if fixture.count(marker)!=1:raise RuntimeError('typed NVS fixture boundary drift')
        fixture=fixture.replace(marker,value)
    unit=path/'fixture.cpp';unit.write_text(fixture);binary=path/'test'
    flags=['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fno-pie',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
        '-I'+str(sdk),'-I'+str(path/'headers'),'-I'+str(root/'main'),'-I'+str(IDF/'components/nvs_flash/src'),
        '-I'+str(IDF/'components/nvs_flash/include'),'-I'+str(IDF/'components/nvs_flash/private_include'),
        '-I'+str(IDF/'components/esp_common/include'),'-I'+str(IDF/'components/esp_rom/include')]
    subprocess.run(['g++',*flags,str(unit),str(sdk/'crc.o'),*[str(sdk/(name+'.o')) for name in native.CPP],
        '-no-pie','-Wl,--gc-sections','-o',str(binary)],check=True,timeout=45)
    return binary


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--storage',type=Path,default=root/'main/storage.cpp')
    parser.add_argument('--schema',type=Path,default=root/'main/runtime/storage_schema.cpp')
    parser.add_argument('--evidence-dir',type=Path)
    parser.add_argument('--mutation',choices=('hide-wrong-type','ignore-disappearance','overwrite-cursor',
        'hide-boot-intent','drop-legacy-volume','accept-embedded-nul'))
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wqn-nvs-typed-') as temp:
        path=Path(temp);binary=build(root,path,args.storage,args.schema,args.mutation)
        rows=[('key','type','encoding','value'),('wqn','namespace','','')]
        rows +=[(key,'data','hex2bin',(b'R'*52).hex()) for key in ['cur_seq','cur_rnd','cur_dic','cur_rev','cur_int','cur_shf','cur_mis']]
        rows += [('word_deck_gen','data','u64','17'),('word_seq_idx','data','u64','5'),
            ('word_deck','data','string','11111111-2222-4333-8444-555555555555'),
            ('word_deck_chg','data','string','18|11111111-2222-4333-8444-555555555555'),
            ('wifi_ssid','data','string','synthetic-net'),('wifi_creds','data','hex2bin','010203'),
            ('wqn_boot','namespace','',''),('schema_gen','data','u32','3'),('reset_id','data','hex2bin','11'*16),
            ('reset_pending','data','hex2bin',(b'WQNRST\x01\0\x03\0\0\0'+b'\x11'*16).hex()),
            ('audio','namespace','',''),('output_volume','data','u8','37')]
        failed=checks=0;results=[]
        for label in ['wrong','valid','legacy','absent','boot-wrong','boot-id-wrong','boot-valid','volume-i32',
            'volume-low','volume-high','cursor-size','blob-empty','string-embedded-nul','string-empty-sdk']:
            case=[]
            for row in rows:
                key,kind,encoding,value=row
                if label=='absent' and kind!='namespace' and key!='key':continue
                if label=='wrong' and kind=='data' and key not in ['schema_gen','reset_id','reset_pending']:
                    encoding,value=('string','unknown') if encoding=='hex2bin' else ('u32','3')
                if label=='boot-wrong' and key in ['schema_gen','reset_pending']:encoding,value='string','unknown'
                if label=='boot-id-wrong' and key=='reset_id':encoding,value='string','unknown'
                if label=='volume-i32' and key=='output_volume':encoding,value='i32','73'
                if label=='volume-low' and key=='output_volume':encoding,value='i32','-5'
                if label=='volume-high' and key=='output_volume':encoding,value='u8','255'
                if label=='cursor-size' and key=='cur_seq':encoding,value='hex2bin',(b'R'*53).hex()
                if label=='blob-empty' and key=='wifi_creds':encoding,value='hex2bin',''
                if label=='string-embedded-nul' and key=='wifi_ssid':encoding,value='string','left\0right'
                case.append((key,kind,encoding,value))
            csvpath=path/(label+'.csv');image=path/(label+'.bin')
            with csvpath.open('w') as stream:csv.writer(stream).writerows(case)
            subprocess.run([IDF_PYTHON,str(IDF/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'),
                'generate',str(csvpath),str(image),'0x4000','--version','1' if label=='legacy' else '2'],check=True,timeout=30,stdout=subprocess.DEVNULL)
            if label=='string-empty-sdk':
                data=bytearray(image.read_bytes())
                at=next(at for page in range(4) for index in range(126)
                    for at in [page*4096+64+index*32]
                    if data[at+1]==0x21 and data[at+8:at+24].split(b'\0',1)[0]==b'wifi_ssid')
                page,index=at//4096,(at%4096-64)//32
                for child in range(index+1,index+data[at+2]):
                    bit=page*4096+32+child//4;data[bit]&=~(3<<(2*(child%4)))
                data[at+2]=1;struct.pack_into('<H',data,at+24,0);struct.pack_into('<I',data,at+28,0xffffffff)
                struct.pack_into('<I',data,at+4,zlib.crc32(data[at:at+4]+data[at+8:at+32],0xffffffff))
                image.write_bytes(data)
            before=hashlib.sha256(image.read_bytes()).hexdigest()
            done=subprocess.run([str(binary),label,str(image)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30,
                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            print(done.stdout,end='',flush=True)
            if done.returncode not in (0,1) or 'NVS typed presence:' not in done.stdout or re.search(r'AddressSanitizer|LeakSanitizer|runtime error:|\bERROR:',done.stdout,re.I):
                raise RuntimeError('native SDK typed presence infrastructure failure')
            match=re.search(r'NVS typed presence: (\d+) checks, (\d+) failures',done.stdout)
            if match is None or (done.returncode==0)!=(int(match[2])==0):raise RuntimeError('native typed presence result/exit mismatch')
            if hashlib.sha256(image.read_bytes()).hexdigest()!=before:raise RuntimeError('SDK fixture source changed')
            checks+=int(match[1]);failed+=int(match[2]);results.append(dict(case=label,returncode=done.returncode,checks=int(match[1]),failures=int(match[2])))
            if args.evidence_dir:
                args.evidence_dir.mkdir(mode=0o700,exist_ok=True)
                (args.evidence_dir/(label+'.log')).write_text(done.stdout)
        if args.evidence_dir:(args.evidence_dir/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(f'NVS typed presence total: {checks} checks, {failed} failures (actual SDK and production functions; host bridge; NOT HIL)',flush=True)
        return 1 if failed else 0


if __name__=='__main__':raise SystemExit(main())
