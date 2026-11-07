#!/usr/bin/env python3
"""Actual SDK NVS/CRC, official generator and pinned raw import; NOT HIL."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import zlib
from unittest.mock import patch

import nvs_context as ctx
import nvs_context_native as native
import raw_flash_import as raw
from test_raw_flash_evidence import build as build_raw
from test_outbox_forensics import ForensicsTests

SID='11111111-2222-4333-8444-555555555555'
CANARY='SYNTHETIC_CREDENTIAL_NOT_IN_CONTEXT'
IDF=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5'))
IDF_PYTHON=os.environ.get('NVS_GENERATOR_PYTHON','/home/unknow/.espressif/python_env/idf5.5_py3.12_env/bin/python')


def main():
    global ctx
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('ignore-flags','truncate-generation-policy','ignore-cursor-crc','ignore-session-binding','skip-replay'))
    args=parser.parse_args()
    checks=failures=0
    def check(good,label):
        nonlocal checks,failures
        checks+=1;failures+=not good
        print(('PASS nvs-context: ' if good else 'FAIL nvs-context: ')+label,flush=True)
    def rejected(label,operation):
        try:operation()
        except (ValueError,OSError):check(True,'reject '+label)
        else:check(False,'reject '+label)
    if args.mutation:
        source=Path(ctx.__file__).read_text()
        old,new={
            'ignore-flags':("result.get('flags')==0",'True'),
            'truncate-generation-policy':('number<=0xffffffff','True'),
            'ignore-cursor-crc':("raw.forensic.journal_crc(data[:48])==crc",'True'),
            'ignore-session-binding':("observation.get('session_id')==session.get('session_id')",'True'),
            'skip-replay':("receipt['manifest_sha256']!=expected_manifest_sha256 or raw.file_inventory(replay)!=before",'False'),
        }[args.mutation]
        if source.count(old)!=1:raise RuntimeError('NVS mutation boundary drift')
        module=types.ModuleType('nvs_context_mutant');module.__file__=ctx.__file__
        exec(compile(source.replace(old,new),ctx.__file__,'exec'),module.__dict__);ctx=module
    with tempfile.TemporaryDirectory(prefix='wqn-nvs-context-') as directory:
        temp=Path(directory);root=Path(__file__).resolve().parents[1]
        build=temp/'reader';bp=native.build(IDF,build)['build_manifest_sha256'];reader=native.binary(build,bp)
        def cursor(paused=1):
            payload=struct.pack('<IHH37s3s',0x57435552,1,paused,SID.encode(),b'\0'*3)
            return payload+struct.pack('<I',raw.forensic.journal_crc(payload))
        def generate(label,extra=(),version=2,scope=17,blob=None,only_other=False,padding=False,replacement=None):
            rows=[('key','type','encoding','value'),('other','namespace','',''),('api_token','data','string',CANARY)]
            rows+=[('cur_seq','data','hex2bin',CANARY.encode().hex())]
            if padding:rows+=[('padding','data','hex2bin','a5'*4200)]
            if not only_other:
                rows+=[('wqn','namespace','','')]
                rows +=[(key,'data','hex2bin',(cursor() if blob is None else blob).hex()) for key in ctx.KEYS[:7]]
                rows +=[('word_deck_gen','data','u64',str(scope)),('word_deck','data','string',SID),
                    ('word_seq_idx','data','u64','9007199254740993'),*extra]
            if replacement:rows=[replacement if row[0]==replacement[0] else row for row in rows]
            source=temp/(label+'.csv');output=temp/(label+'.bin')
            with source.open('w') as stream:csv.writer(stream).writerows(rows)
            subprocess.run([IDF_PYTHON,str(IDF/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'),
                'generate',str(source),str(output),'0x4000','--version',str(version)],check=True,timeout=30,stdout=subprocess.DEVNULL)
            return output
        counter=0
        def read(path,instrumented=None):
            nonlocal counter
            counter+=1;output=temp/f'native-{counter}.json'
            before=raw.hash_file(path)
            with raw.forensic.open_regular(path) as incoming:
                result=native.run(instrumented or reader,incoming,output,sanitize=instrumented is not None)
            check(raw.hash_file(path)==before,'immutable source '+str(counter))
            exported=output.read_text()
            check(CANARY not in exported and CANARY.encode().hex() not in exported,'allowlist excludes credential '+str(counter))
            return result,ctx.assess(result)
        normal=generate('normal');native_normal,context=read(normal)
        check(context['selected_values_read_complete'] and context['scope_context_usable'],'eleven selected values read through SDK')
        check(native_normal['write_attempts']==native_normal['erase_attempts']==0 and native_normal['sandbox_denials']==3,'no source writes and syscall barriers exercised')
        check(context['values']['word_seq_idx']['u64_decimal']=='9007199254740993','u64 stays exact beyond JS safe integer')
        for key in ctx.KEYS[:7]:check(context['values'][key]['cursor']['paused'] and context['values'][key]['cursor']['session_id']==SID,'cursor bytes and session '+key)
        for mode in range(7):
            binding=ctx.session_context(context,dict(mode=mode,session_id=SID,scope_generation=17))
            check(binding['pause_bound_to_retained_session'] and binding['paused'] and binding['committed_scope_generation_matches_retained_session'],'same-session NVS facts '+str(mode))
        binding=ctx.session_context(context,dict(mode=0,session_id='22222222-2222-4333-8444-555555555555',scope_generation=17))
        check(not binding['pause_bound_to_retained_session'] and binding['paused'] is None,'old session cursor cannot override retained session')
        check(not ctx.session_context(context,dict(mode=0,session_id=SID,scope_generation=18))['committed_scope_generation_matches_retained_session'],'scope mismatch stays unresolved')
        check(not any(context[k] for k in ('repair_authorized','installation_ready','identity_authenticated','checkpoint_context_verified','atomic_snapshot_proven')),'local NVS facts grant no repair or identity authority')
        rejected('non-object-native-result',lambda:ctx.assess([]))
        rejected('boolean-native-flags',lambda:ctx.assess({**native_normal,'flags':False}))
        rejected('non-object-native-value',lambda:ctx.assess({**native_normal,'values':[None,*native_normal['values'][1:]]}))
        rejected('boolean-session-mode',lambda:ctx.session_context(context,dict(mode=False,session_id=SID,scope_generation=17)))
        _,legacy=read(generate('legacy',version=1))
        check(legacy['selected_values_read_complete'] and legacy['values']['cur_seq']['cursor']['paused'],'SDK legacy blob format')
        multiple,mc=read(generate('multi-page',padding=True))
        check(mc['selected_values_read_complete'] and mc['scope_context_usable'] and sum(page['state']!=0xffffffff for page in multiple['pages'])>=2,'SDK multi-page store with unrelated large blob')
        multi_bytes=(temp/'multi-page.bin').read_bytes()
        multi_bad=bytearray(multi_bytes);struct.pack_into('<I',multi_bad,4096+4,0)
        struct.pack_into('<I',multi_bad,4096+28,zlib.crc32(multi_bad[4096+4:4096+28],0xffffffff))
        multi_file=temp/'duplicate-page-sequence.bin';multi_file.write_bytes(multi_bad)
        _,bad=read(multi_file)
        check(not bad['selected_values_read_complete'],'duplicate page sequences cannot establish NVS context')
        _,empty=read(generate('missing-namespace',only_other=True))
        check(empty['selected_values_read_complete'] and all(row['state']=='absent' for row in empty['values'].values()) and not empty['scope_context_usable'],'genuine namespace absence distinguished from ready scope')
        _,high=read(generate('high-scope',scope=0x100000011))
        check(high['values']['word_deck_gen']['u64_decimal']=='4294967313' and high['values']['word_deck_gen']['state']=='present_opaque' and not high['scope_context_usable'],'high generation preserved without u32 truncation')
        for label,extra in [('pending',('word_deck_chg','data','string','18|'+SID)),('bad-marker',('word_deck_chg','data','string','invalid'))]:
            _,pending=read(generate(label,extra=(extra,)))
            check(not pending['scope_context_usable'] and 'pending_scope_marker_present_or_unknown' in pending['blockers'],'marker never authorizes current scope '+label)
        for label,data in [('short-cursor',cursor()[:-1]),('empty-cursor',b''),('cursor-crc',cursor()[:-1]+bytes([cursor()[-1]^1]))]:
            _,bad=read(generate(label,blob=data))
            check(not bad['selected_values_read_complete'] and bad['values']['cur_seq']['state']=='present_opaque','invalid cursor retained '+label)
        duplicate=generate('duplicate-key',extra=(('word_deck_gen','data','u64','18'),))
        _,bad=read(duplicate)
        check(not bad['selected_values_read_complete'] and bad['values']['word_deck_gen']['state']=='unknown','duplicate key never selects first or latest value')
        _,wrong_type=read(generate('wrong-type',replacement=('word_deck_gen','data','u32','17')))
        # Storage::findItem may collapse page-level TYPE_MISMATCH into NOT_FOUND.
        # The independent entry inventory must still prevent an absence claim.
        check(wrong_type['values']['word_deck_gen']['state']=='unknown' and wrong_type['values']['word_deck_gen']['sdk_error'] in (0x1102,0x1103),'wrong type never becomes a missing key')
        image=normal.read_bytes()
        def changed(label,mutate):
            data=bytearray(image);mutate(data);path=temp/(label+'.bin');path.write_bytes(data);return path
        def item_offset(key,kind=None):
            row=next(row for row in native_normal['values'] if row['key']==key)
            place=next(place for place in row['locations'] if kind is None or place['type']==kind)
            return place['page']*4096+64+place['entry']*32,place
        def bitmap(data,place,state):
            at=place['page']*4096+32+place['entry']//4;shift=2*(place['entry']%4)
            data[at]=(data[at]&~(3<<shift))|(state<<shift)
        mutations={
            'bad-page-crc':lambda data:data.__setitem__(28,data[28]^1),
            'freeing-page':lambda data:struct.pack_into('<I',data,0,0xfffffff8),
            'future-version':lambda data:(data.__setitem__(8,0xfd),struct.pack_into('<I',data,28,zlib.crc32(data[4:28],0xffffffff))),
            'illegal-bitmap':lambda data:bitmap(data,item_offset('word_deck_gen')[1],1),
            'bad-blob-data':lambda data:data.__setitem__(item_offset('cur_seq',0x42)[0]+32,data[item_offset('cur_seq',0x42)[0]+32]^1),
            'orphan-blob':lambda data:bitmap(data,item_offset('cur_seq',0x48)[1],0),
        }
        for label,mutate in mutations.items():
            result,bad=read(changed(label,mutate))
            check(not bad['selected_values_read_complete'] and not bad['scope_context_usable'] and all(row['state']=='unknown' for row in bad['values'].values()),'ambiguous/corrupt media holds context '+label)
            if label in ('bad-blob-data','orphan-blob'):check(result['write_attempts']>0,'SDK repair attempt detected '+label)
        erased=bytearray(image)
        for place in native_normal['values'][0]['locations']:
            at=place['page']*4096+64+place['entry']*32
            for i in range(erased[at+2]):bitmap(erased,{**place,'entry':place['entry']+i},0)
        erased_file=temp/'erased-value.bin';erased_file.write_bytes(erased);_,ec=read(erased_file)
        check(ec['selected_values_read_complete'] and ec['values']['cur_seq']['state']=='absent','erased cursor bytes retained but never revived')
        alias=bytearray(image)
        ns_at=next(at for at in range(64,4096,32) if alias[at]==0 and alias[at+8:at+12]==b'wqn\0')
        alias[ns_at+24]=1
        struct.pack_into('<I',alias,ns_at+4,zlib.crc32(alias[ns_at:ns_at+4]+alias[ns_at+8:ns_at+32],0xffffffff))
        alias_file=temp/'namespace-alias.bin';alias_file.write_bytes(alias);_,bad=read(alias_file)
        check(not bad['selected_values_read_complete'],'namespace alias cannot attach another namespace to wqn')
        partial=bytearray(image)
        slot=next(i for i in range(126) if (partial[32+i//4]>>(2*(i%4)))&3==3)
        partial[64+slot*32+8]=0
        p=temp/'unpublished-slot.bin';p.write_bytes(partial);_,bad=read(p)
        check(not bad['selected_values_read_complete'],'partially written EMPTY slot cannot prove absence')
        rejected('wrong-reader-pin',lambda:native.binary(build,'44'*32))
        with normal.open('r+b') as incoming:
            rejected('writable-source',lambda:native.run(reader,incoming,temp/'writable.json'))
        truncated=temp/'truncated.bin';truncated.write_bytes(image[:-1])
        with raw.forensic.open_regular(truncated) as incoming:
            rejected('wrong-source-size',lambda:native.run(reader,incoming,temp/'truncated.json'))
        children=[];original_popen=native.subprocess.Popen
        def tracked(*positional,**keywords):
            child=original_popen(*positional,**keywords);children.append(child);return child
        with raw.forensic.open_regular(normal) as incoming,patch.object(native.subprocess,'Popen',side_effect=tracked):
            try:native.run(Path('/bin/sleep'),incoming,temp/'deadline.json',timeout=0.005)
            except subprocess.TimeoutExpired:check(len(children)==1 and children[0].poll() is not None,'deadline kills and reaps exact slow child')
            else:check(False,'slow child must hit deadline')
        instrumented_dir=temp/'instrumented';native.build(IDF,instrumented_dir,sanitize=True)
        instrumented,ic=read(normal,instrumented_dir/'reader')
        check(instrumented==native_normal and ic==context,'ASan UBSan exact clean SDK interpretation')
        instrumented,bad=read(temp/'bad-blob-data.bin',instrumented_dir/'reader')
        check(not bad['selected_values_read_complete'] and instrumented['write_attempts']>0,'ASan UBSan repair refusal')

        # Full production raw capture -> SPIFFS import -> NVS capture/replay.
        ForensicsTests.setUpClass()
        try:
            files=temp/'spiffs-files';files.mkdir()
            for domain,stem in [('word','wout'),('note','nout')]:shutil.copyfile(ForensicsTests.root/domain/'healthy.bin',files/(stem+'.v1'))
            spiffs=temp/'spiffs.bin'
            subprocess.run([sys.executable,str(IDF/'components/spiffs/spiffsgen.py'),str(8*1024*1024),str(files),str(spiffs)],check=True,timeout=30)
            table=temp/'partition_table';table.mkdir()
            subprocess.run([sys.executable,str(IDF/'components/partition_table/gen_esp32part.py'),'--quiet',str(root/'partitions/16m.csv'),str(table/'partition-table.bin')],check=True,timeout=30)
            config=temp/'config.json';config.write_bytes(raw.encoded(dict(SPIFFS_PAGE_SIZE=256,SPIFFS_OBJ_NAME_LEN=32,SPIFFS_META_LENGTH=4,SPIFFS_USE_MAGIC=True,SPIFFS_USE_MAGIC_LENGTH=True,SPIFFS_PAGE_CHECK=True)))
            baseline=temp/'baseline.json';baseline.write_bytes(raw.encoded(dict(target='esp32s3',flash_settings=dict(flash_size='16MB'),flash_files={'0x8000':'partition_table/partition-table.bin'},resolved_sdkconfig_sha256=raw.hash_file(config),embedded_app_description=dict(version='SYNTHETIC_TEST_ONLY',elf_sha256='33'*32),artifacts=[dict(path='partition_table/partition-table.bin',sha256=raw.hash_file(table/'partition-table.bin'))])))
            profile=temp/'profile.json';pp=raw.create_profile(baseline,raw.hash_file(baseline),config,raw.hash_file(config),IDF,profile)['profile_sha256']
            sb=temp/'spiffs-reader';sp=raw.build(profile,pp,IDF,sb)['build_manifest_sha256']
            main=bytearray(b'\xff'*(16*1024*1024));main[0x8000:0x8c00]=(table/'partition-table.bin').read_bytes();main[0x9000:0xd000]=image;main[0x800000:]=spiffs.read_bytes()
            main[0x20000]=0xe9;struct.pack_into('<I',main,0x20020,0xabcd5432);main[0x200b0:0x200d0]=bytes.fromhex('33'*32)
            main_path=temp/'main.bin';main_path.write_bytes(main)
            rb=temp/'raw-build';rb.mkdir();producer=build_raw(root,rb,None);wire=temp/'wire.bin'
            done=subprocess.run([str(producer),'supplied-image',str(wire),str(main_path)],capture_output=True,text=True,check=True,timeout=60)
            if not json.loads(done.stdout)['passed']:raise RuntimeError('production raw fixture failed')
            source=temp/'source';source_pin=raw.raw.receive(wire,source,'11'*16,'22'*6,'33'*32,sensitive_confirmed=True)['manifest_sha256']
            imported=temp/'imported';import_pin=raw.import_bundle(source,source_pin,profile,pp,sb,sp,imported,True)['manifest_sha256']
            output=temp/'context'
            values=(source,source_pin,profile,pp,sb,sp,imported,import_pin,build,bp,output)
            before=raw.file_inventory(source)
            rejected('missing-sensitive-consent',lambda:ctx.capture(*values))
            check(not output.exists(),'sensitive consent before output creation')
            receipt=ctx.capture(*values,True)
            check(receipt['selected_values_read_complete'] and receipt['scope_context_usable'],'full raw source to usable NVS context')
            extracted=raw.load(output/'context.json')
            check(extracted['binding']['forensics']==raw.load(imported/'manifest.json')['forensics'] and extracted['binding']['raw_import_manifest_sha256']==import_pin,'context bound to both forensic bundles and original import')
            check(ctx.verify(*values,expected_manifest_sha256=receipt['manifest_sha256'],sensitive_confirmed=True)['status']=='nvs_context_verified','independent original-image context replay')
            rejected('existing-output',lambda:ctx.capture(*values,True))
            rejected('wrong-import-pin',lambda:ctx.capture(*values[:7],'44'*32,*values[8:-1],temp/'bad-import',True))
            forged=temp/'forged';shutil.copytree(output,forged)
            body=raw.load(forged/'context.json');body['values']['cur_seq']['cursor']['paused']=False
            (forged/'context.json').write_bytes(raw.encoded(body));manifest=raw.load(forged/'manifest.json')
            manifest['artifacts']['context.json']['sha256']=raw.hash_file(forged/'context.json');manifest['artifacts']['context.json']['bytes']=(forged/'context.json').stat().st_size
            (forged/'manifest.json').write_bytes(raw.encoded(manifest));forged_pin=raw.hash_file(forged/'manifest.json')
            rejected('self-consistent-context-forgery',lambda:ctx.verify(*values[:-1],forged,expected_manifest_sha256=forged_pin,sensitive_confirmed=True))
            check(raw.file_inventory(source)==before,'all context paths leave WQR1 source unchanged')
        finally:ForensicsTests.tearDownClass()
    print(f'NVS context: {checks} checks, {failures} failures (actual SDK RO/CRC/raw import; synthetic media; NOT HIL)',flush=True)
    return 1 if failures else 0


if __name__=='__main__':raise SystemExit(main())
