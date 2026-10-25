#!/usr/bin/env python3
"""Exercise production problem JSON codec and journal recovery with libc faults.

Uses the selected ESP-IDF's cJSON source. Does not prove physical power loss.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_problem_empty_cache import section
from test_binary_outbox_recovery import ERR


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=root/'main/problem_store.cpp')
    parser.add_argument('--mutation',choices=('delete-primary','substring-id',
                                             'skip-tail-repair','allow-trailing-json',
                                             'unchecked-request','unchecked-problem',
                                             'unchecked-action','unchecked-time','leak-encoder'))
    parser.add_argument('--idf-path',type=Path,default=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5')))
    args=parser.parse_args()
    cjson=args.idf_path/'components/json/cJSON'
    if not (cjson/'cJSON.c').is_file():
        raise SystemExit('ESP-IDF cJSON source missing; pass --idf-path')
    cc,cxx=shutil.which('gcc'),shutil.which('g++')
    if not cc or not cxx:
        raise SystemExit('gcc/g++ unavailable')
    body=args.source.read_text()
    if args.mutation:
        if args.mutation in ('unchecked-request','unchecked-problem','unchecked-action','unchecked-time'):
            calls = {
                'unchecked-request': 'cJSON_AddStringToObject(root, "request_id", observation.request_id.c_str())',
                'unchecked-problem': 'cJSON_AddStringToObject(root, "problem_id", observation.problem_id.c_str())',
                'unchecked-action': 'cJSON_AddStringToObject(\n            root, "action",\n            wqn::protocol::problem_study_v1::ReviewActionName(observation.action))',
                'unchecked-time': 'cJSON_AddStringToObject(root, "occurred_at", observation.occurred_at.c_str())',
            }
            call = calls[args.mutation]
            old,new='!'+call,'('+call+', false)'
        elif args.mutation=='leak-encoder':
            old,new='''        cJSON_Delete(root);
        return std::string();''','''        (void)root;
        return std::string();'''
        elif args.mutation=='delete-primary':
            old,new='std::rename(kOutboxPath, kOutboxBackupPath)','std::remove(kOutboxPath)'
        elif args.mutation=='skip-tail-repair':
            old,new='if (needs_repair)', 'if (false)'
        elif args.mutation=='allow-trailing-json':
            old,new='cJSON_ParseWithOpts(line.c_str(), nullptr, true)', 'cJSON_Parse(line.c_str())'
        else:
            start=body.index('    for (const std::string& line : lines) {',body.index('esp_err_t CommitTransaction('))
            end=body.index('    if (lines.size() >=',start)
            old=body[start:end]
            new='''    for (const std::string& line : lines) {
        if (line.find(context->observation->request_id) != std::string::npos) return ESP_OK;
    }
'''
        if body.count(old)!=1:
            raise ValueError('problem mutation boundary drift')
        body=body.replace(old,new)
    fixture=(root/'scripts/testdata/problem_empty_cache.cpp').read_text()
    a=fixture.index('using esp_err_t = int;'); b=fixture.index('constexpr const char* kTag',a)
    fixture=fixture[:a]+'#include "esp_err.h"\n#define REAL_CODEC 1\n'+fixture[b:]
    a=fixture.index('namespace wqn {'); b=fixture.index('using Observation',a)
    names=(root/'components/device_protocol/problem_study.cpp').read_text()
    action=section(names,'const char* ReviewActionName(', '\nbool ParseProblemStatus(')
    fixture=fixture[:a]+'''#include "problem_store.h"
#include "device_protocol/json_depth_guard.h"
#include "cJSON.h"
namespace wqn::protocol::problem_study_v1 {
'''+action+'\n}\n'+fixture[b:]
    a=fixture.index('std::string EncodeObservationLine('); b=fixture.index('@@IO@@',a)
    fixture=fixture[:a]+section(body,'std::string EncodeObservationLine(', '// Reads every journal line (bounded);')+'\n'+fixture[b:]
    a=fixture.index('    return {std::string("fixture-request-")'); b=fixture.index('\n}',a)
    fixture=fixture[:a]+'''    Observation value;
    value.request_id=std::string("fixture-request-")+suffix;
    value.problem_id="00000000-0000-0000-0000-000000000001";
    value.occurred_at="2026-10-08T00:00:00Z";
    return value;'''+fixture[b:]
    parts={
        '@@STATE@@':section(body,'constexpr size_t kMaxRejectedRecords','\nbool IsValidRequestId('),
        '@@VALIDATION@@':section(body,'bool IsValidRequestId(', '\nstd::string EncodeObservationLine('),
        '@@IO@@':section(body,'// Reads every journal line (bounded);','\nstruct CommitContext {'),
        '@@TRANSACTIONS@@':section(body,'struct CommitContext {','\nesp_err_t ExecuteWithStorageLease('),
        '@@RESET_CACHE@@':'g_empty_problem_outbox_known=false;',
    }
    for marker,value in parts.items():
        assert fixture.count(marker)==1,marker
        fixture=fixture.replace(marker,value)
    with tempfile.TemporaryDirectory(prefix='note4-problem-recovery-') as tmp:
        directory=Path(tmp); (directory/'esp_err.h').write_text(ERR)
        unit=directory/'problem.cpp'; unit.write_text(fixture)
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run([cc,'-std=c99',*flags,'-I',str(cjson),'-c',str(cjson/'cJSON.c'),'-o',str(directory/'cJSON.o')],check=True,timeout=30)
        binary=directory/'test'
        subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-Werror','-fno-builtin',*flags,
                        '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"','-DWQN_PROBLEM_STUDY_SCHEMA_SHA256="host"',
                        '-I',str(directory),'-I',str(root/'main'),'-I',str(root/'components/device_protocol/include'),
                        '-I',str(cjson),str(unit),str(directory/'cJSON.o'),'-o',str(binary),
                        '-Wl,--wrap=fopen,--wrap=fclose,--wrap=fwrite,--wrap=rename,--wrap=remove,--wrap=fputc,--wrap=cJSON_CreateObject'],
                       check=True,timeout=60)
        return subprocess.run([str(binary),tmp],timeout=30).returncode


if __name__=='__main__':
    raise SystemExit(main())
