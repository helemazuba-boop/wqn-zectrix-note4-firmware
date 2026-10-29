#!/usr/bin/env python3
"""Check complete production v3 sync parsing against bounded UUID fixtures.

Real SDK cJSON, ASan/UBSan/LSan and C++ allocation observation; no I/O APIs.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from jsonschema import Draft202012Validator, FormatChecker


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before',help='compile v3.cpp from a git revision')
    parser.add_argument('--mutation',choices=('unbounded-count','skip-uuid','ignore-json-nul',
                                             'reserve-before-validation','publish-partial'))
    args=parser.parse_args()
    contract=root/'contracts/device-control-v3'
    schema=json.loads((contract/'device-control-v3.schema.json').read_text())
    validator=Draft202012Validator({**schema,'$ref':'#/$defs/successEnvelopeSync'},format_checker=FormatChecker())
    maximum=schema['$defs']['syncData']['properties']['summaries']['properties']['due_problem_ids']['maxItems']
    assert maximum==100
    canonical=json.loads((contract/'fixtures/valid/sync-response.json').read_text())
    base=copy.deepcopy(canonical); base['data']['content_manifest']=[]
    uuid='33333333-3333-4333-8333-333333333333'
    uppercase='ABCDEFAB-CDEF-4ABC-8DEF-ABCDEFABCDEF'
    cases=[]

    def add(name,ids=None,valid=True,no_ids_allocation=False,modify=None,schema_valid=None,wire=None):
        value=copy.deepcopy(base)
        if ids is not None: value['data']['summaries']['due_problem_ids']=ids
        if modify: modify(value)
        if wire is None: wire=json.dumps(value,ensure_ascii=True,separators=(',',':'))
        if schema_valid is None: schema_valid=valid
        cases.append({'name':name,'value':value,'wire':wire,'valid':valid,
                      'no_ids_allocation':no_ids_allocation,'schema_valid':schema_valid})

    add('empty',[],no_ids_allocation=True)
    for count in (1,maximum-1,maximum,maximum+1,1000):
        add(f'count-{count}',[uuid]*count,valid=count<=maximum,no_ids_allocation=count>maximum)
    add('uppercase-nil-max-order',[uuid,uppercase,'00000000-0000-0000-0000-000000000000',
                                  'ffffffff-ffff-ffff-ffff-ffffffffffff',uuid])
    invalid=['',uuid[:-1],uuid+'0',uuid.replace('-',''),uuid.replace('-','_'),
             'G'+uuid[1:],' '+uuid,uuid+' ',uuid+'\n','{'+uuid+'}',
             'urn:uuid:'+uuid,'３'+uuid[1:],uuid+'\0suffix',uuid+'\0',
             uuid[:8]+'\0'+uuid[9:]]
    for index,value in enumerate(invalid):
        add(f'bad-uuid-{index}',[value],valid=False,no_ids_allocation=True)
        if index==11:
            # Python's UUID integer conversion accepts fullwidth digits. Do
            # not use that format checker as the oracle for the ASCII guard.
            cases[-1]['schema_valid']=None
    add('bad-last-of-100',[uuid]*(maximum-1)+['not-a-uuid'],valid=False,no_ids_allocation=True)
    for index,value in enumerate((None,7,True,{},[])):
        add(f'non-string-{index}',[value],valid=False,no_ids_allocation=True)
    for index,value in enumerate((None,'not-an-array',{},7)):
        add(f'non-array-{index}',valid=False,no_ids_allocation=True,
            modify=lambda v,item=value:v['data']['summaries'].__setitem__('due_problem_ids',item))
    add('missing-array',valid=False,no_ids_allocation=True,
        modify=lambda v:v['data']['summaries'].pop('due_problem_ids'))
    add('canonical-fixture',modify=lambda v:v.update(copy.deepcopy(canonical)))
    add('legacy-no-mistake',modify=lambda v:v['data']['summaries'].pop('word_mistake_count'),schema_valid=False)
    add('legacy-no-manifest',modify=lambda v:v['data'].pop('content_manifest'),schema_valid=False)
    add('unknown-additive-kind',modify=lambda v:v['data']['content_manifest'].append(
        {'kind':'future-kind','revision':99}),schema_valid=False)
    add('invalid-manifest-after-valid-ids',valid=False,modify=lambda v:v['data'].__setitem__(
        'content_manifest',[{'kind':'problem_packs','revision':-1}]))
    add('duplicate-manifest-after-valid-ids',valid=False,modify=lambda v:v['data'].__setitem__(
        'content_manifest',[{'kind':'problem_packs','revision':1}]*2),schema_valid=True)
    # Opaque cursor strings cannot be losslessly represented by cJSON with a
    # decoded NUL. Reject those explicitly; literal backslash-u remains valid.
    add('cursor-escaped-nul',valid=False,no_ids_allocation=True,schema_valid=True,
        modify=lambda v:v['data'].__setitem__('content_manifest',[
            {'kind':'problem_packs','revision':1,'cursor':'prefix\0suffix'}]))
    add('cursor-literal-backslash-u',modify=lambda v:v['data'].__setitem__('content_manifest',[
        {'kind':'problem_packs','revision':1,'cursor':r'prefix\u0000suffix'}]))
    add('cursor-escaped-backslash-then-nul',valid=False,no_ids_allocation=True,schema_valid=True,
        modify=lambda v:v['data'].__setitem__('content_manifest',[
            {'kind':'problem_packs','revision':1,'cursor':'prefix\\\0suffix'}]))
    add('raw-nul-after-document',valid=False,no_ids_allocation=True,
        wire=json.dumps(base)+'\0ignored-by-c-parser')
    cases[-1]['schema_valid']=None
    add('valid-unicode-escaped-uuid',[uuid],wire=json.dumps(base).replace(uuid,r'\u0033'+uuid[1:]))

    checked=0
    for case in cases:
        if case['schema_valid'] is not None:
            actual=json.loads(case['wire'])
            if validator.is_valid(actual)!=case['schema_valid']:
                raise SystemExit('FAIL: schema/format expectation: '+case['name'])
            checked+=1
    print(f'Pinned schema/UUID format: {checked} cases checked; compatibility and stricter NUL policy are explicit.',flush=True)
    code=(subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/components/device_protocol/v3.cpp'],
                                  cwd=root,text=True) if args.before else (root/'components/device_protocol/v3.cpp').read_text())
    guard_path='components/device_protocol/include/device_protocol/json_depth_guard.h'
    guard=(subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{guard_path}'],cwd=root,text=True)
           if args.before else (root/guard_path).read_text())
    shared_nul_mutation=args.mutation=='ignore-json-nul' and 'HasUnrepresentableJsonNul(body)' not in code
    if shared_nul_mutation:
        old='JsonStringsRepresentable(data, length);'
        assert guard.count(old)==1, 'shared NUL boundary drift'
        guard=guard.replace(old,'true;')
    if args.mutation and not shared_nul_mutation:
        changes={
            'unbounded-count':('if (count > 100)','if (false && count > 100)'),
            'skip-uuid':('!IsUuid(item->valuestring)','(!IsUuid(item->valuestring) && false)'),
            'ignore-json-nul':('if (HasUnrepresentableJsonNul(body))','if (HasUnrepresentableJsonNul(body) && false)'),
            'reserve-before-validation':('    // Validate the entire bounded collection before reserving/copying any IDs.',
                                          '    parsed.due_problem_ids.reserve(count);\n    // Validate the entire bounded collection before reserving/copying any IDs.'),
            'publish-partial':('    SyncData parsed;','    SyncData& parsed = *data;'),
        }
        old,new=changes[args.mutation]
        if code.count(old)!=1: raise SystemExit('FAIL: v3 sync mutation boundary drift')
        code=code.replace(old,new)
        if args.mutation=='publish-partial': code=code.replace('*data = std::move(parsed);','/* direct output mutation */')

    def cpp_string(text):
        encoded=text.encode('utf-8')
        return 'std::string("'+''.join(f'\\x{byte:02x}' for byte in encoded)+f'",{len(encoded)})'

    rows=[]
    known={'problems','todos','words','word_packs','note_packs','problem_packs'}
    for case in cases:
        value=case['value']['data']
        ids=value['summaries']['due_problem_ids'] if case['valid'] else []
        expected='{'+','.join(cpp_string(item) for item in ids)+'}'
        targets=sum(item['kind'] in known for item in value.get('content_manifest',[])) if case['valid'] else 0
        rows.append('{'+','.join([json.dumps(case['name']),cpp_string(case['wire']),
            str(case['valid']).lower(),str(case['no_ids_allocation']).lower(),expected,
            str(value['summaries'].get('word_mistake_count',-1)),str(targets)])+'}')
    fixture=(root/'scripts/testdata/v3_sync_response.cpp').read_text().replace('@@CASES@@',',\n'.join(rows))
    sdk=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5'))
    cjson=sdk/'components/json/cJSON'
    digest=hashlib.sha256((contract/'device-control-v3.schema.json').read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='wqn-v3-sync-') as directory:
        tmp=Path(directory); binary=tmp/'test'
        (tmp/'device_protocol').mkdir()
        (tmp/'device_protocol/json_depth_guard.h').write_text(guard)
        (tmp/'v3.cpp').write_text(code); (tmp/'test.cpp').write_text(fixture)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                        '-I'+str(tmp),'-I'+str(root/'scripts/testdata/sync_host'),
                        '-I'+str(root/'components/device_protocol/include'),'-I'+str(cjson),
                        f'-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="{digest}"',
                        str(tmp/'v3.cpp'),str(tmp/'test.cpp'),str(cjson/'cJSON.c'),'-o',str(binary)],
                       check=True,timeout=60)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)],env=env,timeout=30).returncode


if __name__=='__main__':
    raise SystemExit(main())
