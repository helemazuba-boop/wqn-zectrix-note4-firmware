#!/usr/bin/env python3
"""Real pack readers, row parsers and index scans on temporary libc files.

Checks original-byte framing and cJSON identity with actual SDK cJSON; NOT HIL.
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
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--mutation',choices=('raw-span','word-json','note-json','problem-json','rollback'))
    args=parser.parse_args()
    def source(path):
        return (subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{path}'],cwd=root,text=True)
                if args.before else (root/path).read_text())
    units=[]
    uses_pack_reader=False
    for domain,reader,row,scan in [
        ('word','ReadBoundedPackLine','ParsePackEntryLine','ScanPackFile'),
        ('note','ReadBoundedNotePackLine','ParseNoteRecordLine','ScanNotePackFile'),
        ('problem','ReadBoundedProblemPackLine','ParseProblemRecordLine','ScanProblemPackFile')]:
        code=source('main/'+domain+'_pack.cpp')
        uses_pack_reader=uses_pack_reader or 'ReadBoundedPackTextLine(' in code
        constants='\n'.join(re.findall(r'^constexpr (?:char|size_t|uint64_t) .*?;',code,re.M|re.S))
        doc=function(code,'class JsonDocument {')+';'
        if args.mutation==domain+'-json':
            assert 'JsonInputWithinLimits(' in doc
            doc=doc.replace('JsonInputWithinLimits(','JsonNestingWithinLimit(')
        helpers=['std::string GetOptionalString(','bool GetExactUint64(','void CopyField(']
        if domain!='problem': helpers+=['bool GetExactInt32(']
        if domain!='word': helpers+=['void CopyTitleUtf8Safe(']
        if domain=='problem': helpers+=['bool ParsePartChoices(','bool IsImageIdArrayValid(']
        text='\n'.join(function(code,s) for s in helpers)
        row_body=function(code,'esp_err_t '+row+'(')
        read_body=function(code,'esp_err_t '+reader+'(')
        scan_body=function(code,'esp_err_t '+scan+'(')
        if args.mutation=='rollback':
            assert scan_body.count('if (!committed && index != nullptr)')==1
            scan_body=scan_body.replace('if (!committed && index != nullptr)','if (false)')
        if domain=='problem':
            text+='\n}\nnamespace wqn {\nusing namespace problem_fixture;\n'+row_body+'\n}\nnamespace problem_fixture {\n'
        else:
            text+='\n'+row_body
        path_name={'word':'PackPathForItem','note':'PackPathForNotebook','problem':'PackPathForSet'}[domain]
        path_type={'word':'WqnWordPackManifestItem','note':'WqnNotePackManifestNotebook','problem':'WqnProblemPackManifestSet'}[domain]
        units.append('namespace '+domain+'_fixture {\nusing namespace wqn;\n'+constants+'\n'+doc+'\n'+text+
            f'\nstd::string {path_name}(const wqn::{path_type}&) {{ return pack_path; }}\n'+read_body+'\n'+scan_body+'\n}\n')
    names=source('components/device_protocol/problem_study.cpp')
    status='namespace wqn::protocol::problem_study_v1 {\n'+function(names,'bool ParseProblemStatus(')+'\n}\n'
    fixture=(root/'scripts/testdata/pack_json_boundaries.cpp').read_text().replace('@@PRODUCTION@@',status+'\n'.join(units))
    cjson=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5'))/'components/json/cJSON'
    with tempfile.TemporaryDirectory(prefix='wqn-pack-json-') as directory:
        temp=Path(directory);(temp/'esp_err.h').write_text(ERR)
        (temp/'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\nconstexpr int MALLOC_CAP_SPIRAM=1;\ninline void* heap_caps_malloc(size_t n,int){return std::malloc(n);}\ninline void heap_caps_free(void* p){std::free(p);}\n')
        if uses_pack_reader:
            guard=source('main/pack_jsonl_reader.h')
            if args.mutation=='raw-span':
                assert guard.count('std::memchr(buffer->data(),')==1
                guard=guard.replace('std::memchr(buffer->data(),','false && std::memchr(buffer->data(),')
            (temp/'pack_jsonl_reader.h').write_text(guard)
        (temp/'test.cpp').write_text(fixture)
        binary=temp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-const-variable',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie',
            '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"','-DWQN_WORD_STUDY_SCHEMA_SHA256="host"',
            '-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"','-DWQN_PROBLEM_STUDY_SCHEMA_SHA256="host"',
            '-I'+str(temp),'-I'+str(root/'main'),'-I'+str(root/'components/device_protocol/include'),'-I'+str(cjson),
            str(temp/'test.cpp'),str(cjson/'cJSON.c'),'-o',str(binary)],check=True,timeout=60)
        return subprocess.run([str(binary),str(temp/'pack.jsonl')],timeout=30,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__=='__main__':
    raise SystemExit(main())
