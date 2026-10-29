#!/usr/bin/env python3
"""Real protocol parsers, SSE decoder and HTTP result-delivery functions.

Synthetic JSON boundary cases, SDK cJSON and ASan/UBSan/LSan; no network/model.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_audio_runtime import function


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--cases-json',type=Path)
    parser.add_argument('--mutation',choices=('raw-nul','escaped-nul','escape-parity','word-boundary','sse-boundary','portal-boundary'))
    args=parser.parse_args()

    def source(name):
        if args.before:
            return subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{name}'],cwd=root,text=True)
        return (root/name).read_text()

    def golden(contract,name):
        return json.loads((root/f'contracts/{contract}/fixtures/valid/{name}.json').read_text())

    uuid='11111111-1111-4111-8111-111111111111'
    def envelope(data):
        return dict(ok=True,request_id='req_note_boundary_001',server_time_ms=1784426400000,data=data)
    note_session=envelope(dict(session_id=uuid,domain='note',mode='sequential',purpose='browse',
        ordering='sequential_note_v1',candidate_policy_version='sequential_note_v1',seed='seed',
        optional_count=1,scope=dict(notebook_ids=[uuid],include_archived=False),snapshot=[],
        items=[],next_sequence=0,progress_revision=1,has_more=False,cursor='0'))
    note_page=copy.deepcopy(note_session); note_page['data']['next_cursor']='0'
    note_observation=envelope(dict(observation_id=uuid,session_id=uuid,item_id=uuid,sequence=0,
        action='opened',progress=None,projection_applied=False,replayed=False))
    note_manifest=envelope(dict(cursor='0',has_more=False,notebooks=[dict(notebook_id=uuid,
        title='note',change_sequence=1,content_revision=1,deleted=False,pack=None)]))
    entries=[
        ('v3.claim-start',golden('device-control-v3','claim-start-response')),
        ('v3.claim-poll',golden('device-control-v3','claim-poll-response')),
        ('v3.bootstrap',golden('device-control-v3','bootstrap-response')),
        ('v3.sync',golden('device-control-v3','sync-response')),
        *[(f'word.{kind}',golden('word-study-v1',file)) for kind,file in (
            ('session','session-response'),('page','candidate-page-response'),
            ('observation','observation-response'),('manifest','manifest-response'))],
        ('note.session',note_session),('note.page',note_page),('note.observation',note_observation),
        ('note.manifest',note_manifest),
        ('problem.manifest',golden('problem-study-v1','manifest-response')),
        ('problem.observation',golden('problem-study-v1','observation-response')),
    ]
    cases=[]
    def add(kind,base,label,valid,value=None,wire=None):
        payload=copy.deepcopy(base) if value is None else value
        cases.append(dict(parser=kind,name=kind+'/'+label,request_id=base['request_id'],valid=valid,
            wire=json.dumps(payload,ensure_ascii=True,separators=(',',':')) if wire is None else wire))
    for kind,base in entries:
        add(kind,base,'canonical',True)
        alias=copy.deepcopy(base); alias['request_id']+='\0shadow'; add(kind,base,'request-value-nul',False,alias)
        alias=copy.deepcopy(base); alias['request_id\0shadow']=alias.pop('request_id'); add(kind,base,'request-key-nul',False,alias)
        alias=copy.deepcopy(base); alias['extension']='prefix\0shadow'; add(kind,base,'unknown-value-nul',False,alias)
        add(kind,base,'raw-tail-nul',False,wire=json.dumps(base)+'\0ignored')
        alias=copy.deepcopy(base); alias['extension']=r'\u0000'; add(kind,base,'literal-backslash-u0000',True,alias)
        alias=copy.deepcopy(base); alias['extension']='\x01\n\t中文😀'; add(kind,base,'representable-controls-and-unicode',True,alias)
        # A full JSON string value must not become its valid enum/ID prefix.
        alias=copy.deepcopy(base)
        def suffix_first(value):
            if isinstance(value,dict):
                for key,item in value.items():
                    if isinstance(item,str) and item:
                        value[key]=item+'\0shadow'; return True
                    if suffix_first(item): return True
            elif isinstance(value,list):
                for item in value:
                    if suffix_first(item): return True
            return False
        assert suffix_first(alias['data'])
        add(kind,base,'business-value-nul',False,alias)
        alias=copy.deepcopy(base)
        def suffix_id(value):
            if isinstance(value,dict):
                for key,item in value.items():
                    if key.endswith('_id') and isinstance(item,str) and item:
                        value[key]=item+'\0shadow'; return True
                    if suffix_id(item): return True
            elif isinstance(value,list):
                for item in value:
                    if suffix_id(item): return True
            return False
        if suffix_id(alias['data']): add(kind,base,'domain-id-nul',False,alias)
    api=source('main/wqn_api.cpp')
    fixture=(root/'scripts/testdata/json_string_boundaries.cpp').read_text()
    def cpp(text):
        data=text.encode('utf-8')
        return 'std::string("'+''.join(f'\\x{b:02x}' for b in data)+f'",{len(data)})'
    rows=['{'+','.join((json.dumps(row['parser']),json.dumps(row['name']),cpp(row['request_id']),
          cpp(row['wire']),str(row['valid']).lower()))+'}' for row in cases]
    fixture=fixture.replace('@@CASES@@',',\n'.join(rows)).replace('@@WORD_CALLER@@',function(api,'esp_err_t FetchWordPackManifest(')).replace('@@BOOTSTRAP_CALLER@@',function(api,'esp_err_t BootstrapDeviceControlV3('))
    fixture=fixture.replace('@@REPLY_CALLER@@',function(api,'esp_err_t PostAiInteractionReply(')).replace('@@SUCCESS@@',function(api,'bool GetSuccess('))
    portal=source('components/wqn_wifi_provision/wifi_provision_portal.cpp')
    portal_body=function(portal,'esp_err_t WifiProvisionPortal::HandleSubmit(')
    if args.mutation=='portal-boundary':
        assert portal_body.count('protocol::JsonInputWithinLimits(')==1
        portal_body=portal_body.replace('protocol::JsonInputWithinLimits(','protocol::JsonNestingWithinLimit(')
    portal_helper=(function(portal,'bool JsonNestingWithinLimit(')
                   if 'bool JsonNestingWithinLimit(' in portal else '')
    fixture=fixture.replace('@@PORTAL_HELPER@@',portal_helper).replace('@@PORTAL_SUBMIT@@',portal_body)
    if args.cases_json: args.cases_json.write_text(json.dumps(cases,ensure_ascii=True,indent=2)+'\n')
    cjson=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5'))/'components/json/cJSON'
    flags=[]
    for contract,macro in [('device-control-v3','DEVICE_CONTROL'),('word-study-v1','WORD_STUDY'),('note-study-v1','NOTE_STUDY'),('problem-study-v1','PROBLEM_STUDY')]:
        digest=hashlib.sha256((root/f'contracts/{contract}/{contract}.schema.json').read_bytes()).hexdigest()
        flags.append(f'-DWQN_{macro}_SCHEMA_SHA256="{digest}"')
    with tempfile.TemporaryDirectory(prefix='wqn-json-boundary-') as directory:
        tmp=Path(directory); (tmp/'device_protocol').mkdir()
        guard=source('components/device_protocol/include/device_protocol/json_depth_guard.h')
        if args.mutation in ('raw-nul','escaped-nul','escape-parity'):
            start=guard.index('inline bool JsonStringsRepresentable(')
            prefix,tail=guard[:start],guard[start:]
            changes={'raw-nul':("if (c == '\\0') return false;","if (false && c == '\\0') return false;"),
                     'escaped-nul':("if (c == 'u' &&","if (false && c == 'u' &&"),
                     'escape-parity':('            escaped = false;',"            escaped = c == '\\\\';")}
            old,new=changes[args.mutation]; assert tail.count(old)==1
            guard=prefix+tail.replace(old,new)
        flags.append('-DHAS_JSON_INPUT_GUARD='+str(int('inline bool JsonInputWithinLimits(' in guard)))
        (tmp/'device_protocol/json_depth_guard.h').write_text(guard)
        (tmp/'esp_check.h').write_text('#pragma once\n#include "esp_err.h"\n#define ESP_RETURN_ON_ERROR(x,...) do { const auto e_=(x); if(e_!=ESP_OK) return e_; } while(false)\n#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))\n')
        (tmp/'test.cpp').write_text(fixture)
        units=[]
        for name in ('v3','word_study','note_study','problem_study'):
            code=source('components/device_protocol/'+name+'.cpp')
            if args.mutation=='word-boundary' and name=='word_study': code=code.replace('JsonInputWithinLimits(','JsonNestingWithinLimit(')
            p=tmp/(name+'.cpp'); p.write_text(code); units.append(str(p))
        sse=source('main/sse_chunk.cpp')
        if args.mutation=='sse-boundary': sse=sse.replace('JsonInputWithinLimits(','JsonNestingWithinLimit(')
        (tmp/'sse_chunk.cpp').write_text(sse); units.append(str(tmp/'sse_chunk.cpp'))
        binary=tmp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie',*flags,
            '-I'+str(tmp),'-I'+str(root/'scripts/testdata/sync_host'),'-I'+str(root/'main'),
            '-I'+str(root/'components/device_protocol/include'),'-I'+str(cjson),str(tmp/'test.cpp'),
            *units,str(cjson/'cJSON.c'),'-o',str(binary)],check=True,timeout=60)
        return subprocess.run([str(binary)],timeout=30,env=dict(os.environ,
            ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__=='__main__':
    raise SystemExit(main())
