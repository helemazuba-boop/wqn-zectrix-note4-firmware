#!/usr/bin/env python3
"""Actual raw collector -> SDK RO SPIFFS -> original outbox facts. NOT HIL."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

import raw_flash_import as imp
import raw_flash_evidence_receive as receiver
import spiffs_evidence_native as native
import outbox_reconcile as reconciliation
from test_raw_flash_evidence import build as build_raw
from test_outbox_forensics import ForensicsTests

PINS=('11'*16,'22'*6,'33'*32)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('ignore-encryption','ignore-table-digest','skip-replay','ignore-source-recheck'))
    parser.add_argument('--save-example',type=Path)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1];idf=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5'))
    checks=failures=0
    def check(good,label):
        nonlocal checks,failures
        checks+=1;failures+=not good
        print(('PASS raw-import: ' if good else 'FAIL raw-import: ')+label,flush=True)
    if args.mutation:
        source=Path(imp.__file__).read_text()
        old,new={
            'ignore-encryption':("original['flash_encryption_enabled'] is not False",'False'),
            'ignore-table-digest':("row[16:]!=hashlib.md5(data[:offset]).digest()",'False'),
            'ignore-source-recheck':("original['raw_image_complete'] is not True",'False'),
            'skip-replay':("receipt['manifest_sha256']!=output_pin or file_inventory(replay)!=before",'False'),
        }[args.mutation]
        if source.count(old)!=1:raise RuntimeError('raw import mutation boundary drift')
        import types
        module=types.ModuleType('raw_import_mutant');module.__file__=imp.__file__
        exec(compile(source.replace(old,new),imp.__file__,'exec'),module.__dict__)
        globals()['imp']=module
    with tempfile.TemporaryDirectory(prefix='wqn-raw-import-') as temp:
        directory=Path(temp)
        ForensicsTests.setUpClass()
        try:
            files=directory/'input-files';files.mkdir()
            for domain,prefix in (('word','wout'),('note','nout')):
                shutil.copy2(ForensicsTests.root/domain/'healthy.bin',files/(prefix+'.v1'))
            (files/'wout.tmp').write_bytes(b'opaque unpublished\x00\xff')
            (files/'future-data').write_bytes(b'PRIVATE_SYNTHETIC_BYTES\0\xff')
            (files/'empty').write_bytes(b'')
            spiffs=directory/'spiffs.bin'
            subprocess.run([sys.executable,str(idf/'components/spiffs/spiffsgen.py'),str(8*1024*1024),str(files),str(spiffs)],check=True,timeout=30)
            table=directory/'partition_table';table.mkdir()
            subprocess.run([sys.executable,str(idf/'components/partition_table/gen_esp32part.py'),'--quiet',str(root/'partitions/16m.csv'),str(table/'partition-table.bin')],check=True,timeout=30)
            config=directory/'config.json';config.write_bytes(imp.encoded(dict(SPIFFS_PAGE_SIZE=256,SPIFFS_OBJ_NAME_LEN=32,SPIFFS_META_LENGTH=4,SPIFFS_USE_MAGIC=True,SPIFFS_USE_MAGIC_LENGTH=True,SPIFFS_PAGE_CHECK=True)))
            baseline=directory/'baseline.json';baseline.write_bytes(imp.encoded(dict(target='esp32s3',flash_settings=dict(flash_size='16MB'),flash_files={'0x8000':'partition_table/partition-table.bin'},
                resolved_sdkconfig_sha256=imp.hash_file(config),embedded_app_description=dict(version='SYNTHETIC_TEST_ONLY',elf_sha256=PINS[2]),artifacts=[dict(path='partition_table/partition-table.bin',sha256=imp.hash_file(table/'partition-table.bin'))])))
            profile=directory/'profile.json'
            prof=imp.create_profile(baseline,imp.hash_file(baseline),config,imp.hash_file(config),idf,profile)
            pp=prof['profile_sha256'];build_dir=directory/'reader';bp=imp.build(profile,pp,idf,build_dir)['build_manifest_sha256']
            raw_dir=directory/'raw-build';raw_dir.mkdir();raw_binary=build_raw(root,raw_dir,None)
            image=bytearray(b'\xff'*(16*1024*1024));image[0x8000:0x8c00]=(table/'partition-table.bin').read_bytes()
            image[0x800000:]=spiffs.read_bytes();image[0x9000:0x9020]=b'RAW_NVS_OPAQUE_NOT_A_KEY_OR_CURSOR'.ljust(32,b'\0')[:32]
            image[0x20000]=0xe9;struct.pack_into('<I',image,0x20000+32,0xabcd5432);image[0x20000+176:0x20000+208]=bytes.fromhex(PINS[2])
            image_path=directory/'image.bin';image_path.write_bytes(image)
            def capture(label,data=None,encrypted=False,unstable=False):
                path=image_path
                if data is not None:
                    path=directory/(label+'-image.bin');path.write_bytes(data)
                wire=directory/(label+'-wire.bin')
                done=subprocess.run([str(raw_binary),'supplied-unstable' if unstable else 'supplied-encrypted' if encrypted else 'supplied-image',str(wire),str(path)],capture_output=True,text=True,timeout=60)
                if done.returncode!=0 or done.stderr or not json.loads(done.stdout)['passed']:raise RuntimeError('raw producer infrastructure failure '+done.stderr)
                bundle=directory/(label+'-source');receipt=receiver.receive(wire,bundle,*PINS,sensitive_confirmed=True)
                return bundle,receipt['manifest_sha256']
            source,sp=capture('normal');before=imp.file_inventory(source)
            def run(output,src=source,pin=sp,consent=True):return imp.import_bundle(src,pin,profile,pp,build_dir,bp,output,consent)
            imported=directory/'imported';receipt=run(imported)
            check(imp.file_inventory(source)==before,'source WQR1 bundle unchanged')
            check((imported/'nvs-raw.bin').read_bytes()==image[0x9000:0xd000],'NVS retained opaque without key/cursor inference')
            for path in files.iterdir():check((imported/'files'/path.name).read_bytes()==path.read_bytes(),'exact visible object '+path.name)
            check(set(receipt['forensics'])=={'word','note'},'forensic bridge for both domains')
            for domain in ('word','note'):
                manifest=imp.forensic.verify(imported/('forensics-'+domain),receipt['forensics'][domain])
                check(not manifest['repair_authorized'],'no repair authority '+domain)
            verified=imp.verify(source,sp,profile,pp,build_dir,bp,imported,receipt['manifest_sha256'],True)
            check(verified['status']=='raw_import_verified','independent source replay')
            manifest=imp.load(imported/'manifest.json',receipt['manifest_sha256'])
            check(manifest['inspection']['source_write_attempts']==manifest['inspection']['source_erase_attempts']==0 and manifest['inspection']['sandbox_denials_checked']==3,'SDK RO and syscall barriers exercised')
            check(manifest['nvs_decoded'] is False and not any(manifest[key] for key in ('identity_authenticated','atomic_snapshot_proven','repair_authorized','installation_ready','checkpoint_context_verified')),'claims remain bounded')
            # The existing receipt collector consumes the imported canonical
            # forensic bytes. Transport below is injected, never live auth.
            token=directory/'synthetic-token';imp.wire.private(token,b'c'*64)
            actor=dict(user_id='00000000-0000-0000-0000-000000000001',device_id='22222222-2222-4222-8222-222222222222')
            for domain in ('word','note'):
                bundle=imported/('forensics-'+domain);fp=receipt['forensics'][domain]
                facts={}
                for row in reconciliation.inventory(bundle,reconciliation.source_manifest(bundle,fp)):
                    if row.get('kind')=='observation' and row.get('identity_usable_for_lookup'):
                        facts[(row['session_id'].lower(),row['request_id'])]=row
                def transport(endpoint,credential,body,allow,timeout):
                    if credential!='c'*64:raise RuntimeError('unexpected synthetic credential')
                    request=json.loads(body);entries=[];mode='sequential'
                    for key in request['request_ids']:
                        row=facts[(request['session_id'],key)];mode=reconciliation.MODES[domain][row['mode']]
                        entries.append(dict(request_id=key,state='found',observation=dict(observation_id='44444444-4444-4444-8444-444444444444',request_id=key,
                            session_id=request['session_id'],sequence=row['sequence'],item_id=row['item_id'],mode=mode,
                            action=reconciliation.ACTIONS[domain][row['action']],occurred_at=row['occurred_at'],created_at='2026-11-07T00:00:00Z')))
                    reply=dict(ok=True,request_id=request['request_id'],server_time_ms=1794009600000,data=dict(contract=reconciliation.CONTRACT,read_at='2026-11-07T00:00:00Z',actor=actor,
                        session=dict(session_id=request['session_id'],domain=domain,mode=mode,status='abandoned',next_sequence=900,expires_at='2026-10-27T00:00:00Z'),receipts=entries,repair_authorized=False))
                    return dict(http_status=200,raw=base64.b64encode(reconciliation.encoded(reply)).decode(),error=None)
                evidence=directory/('receipt-'+domain)
                collected=reconciliation.collect(bundle,fp,evidence,'https://wqn.example'+reconciliation.ENDPOINT_PATH,token,actor,transport=transport)
                _,_,assessment,_=reconciliation.verify(bundle,fp,evidence,collected['manifest_sha256'])
                check(assessment['query_complete'] and sum(row['state']=='matched' for row in assessment['records'])==3,'imported '+domain+' facts reach existing receipt client (injected transport)')
                plan=reconciliation.review_plan(bundle,fp,evidence,collected['manifest_sha256'])
                check(not plan['repair_authorized'] and not plan['installation_ready'] and 'device_source_identity_not_attested' in plan['blocking_preconditions'],'imported '+domain+' facts do not bypass checkpoint/repair gates')
            def rejected(label,operation):
                try:operation()
                except (ValueError,OSError):check(True,'reject '+label)
                else:check(False,'reject '+label)
            rejected('no-sensitive-consent',lambda:run(directory/'no-consent',consent=False))
            check(not (directory/'no-consent').exists(),'consent before output creation')
            encrypted,ep=capture('encrypted',encrypted=True)
            rejected('encrypted-source',lambda:run(directory/'encrypted-result',encrypted,ep))
            unstable,up=capture('unverified-reread',unstable=True)
            rejected('unverified-source-reread',lambda:run(directory/'unverified-reread-result',unstable,up))
            bad=bytearray(image);bad[0x8000+6*32+16]^=1
            corrupted,cp=capture('table-digest',bad)
            rejected('partition-table-digest',lambda:run(directory/'table-result',corrupted,cp))
            # Valid table framing with encrypted storage is still held.
            flagged=bytearray(image);struct.pack_into('<I',flagged,0x8000+5*32+28,1)
            flagged[0x8000+6*32+16:0x8000+7*32]=hashlib.md5(flagged[0x8000:0x8000+6*32]).digest()
            encrypted_partition,ps=capture('encrypted-partition',flagged)
            rejected('encrypted-partition',lambda:run(directory/'encrypted-part-result',encrypted_partition,ps))
            unformatted=bytearray(image);unformatted[0x800000:]=b'\xff'*(8*1024*1024)
            broken,bs=capture('unformatted',unformatted)
            rejected('unformatted-spiffs',lambda:run(directory/'unformatted-result',broken,bs))
            check((directory/'unformatted-result/.incomplete').exists() and not (directory/'unformatted-result/manifest.json').exists(),'failed mount retains source copy and incomplete state')
            # Preserve original name padding and opaque/path-like names. Never
            # silently choose an alias when two on-disk names normalize alike.
            opaque=bytearray(image)
            for row in manifest['objects']:
                if row['canonical_name'] not in ('future-data','empty'):continue
                start=0x800000+row['page_index']*256
                at=bytes(opaque[start:start+256]).find(bytes.fromhex(row['name_hex']))
                if at<0:raise RuntimeError('SDK object name layout not found')
                name=(b'wout.v1\0'+b'\xa5').ljust(32,b'\0') if row['canonical_name']=='future-data' else b'/../../escape\0'.ljust(32,b'\0')
                opaque[start+at:start+at+32]=name
            ambiguous,ap=capture('opaque-names',opaque);ao=directory/'opaque-result';ar=run(ao,ambiguous,ap)
            am=imp.load(ao/'manifest.json',ar['manifest_sha256'])
            check(am['inspection']['ambiguous_canonical_names']==['wout.v1'] and len(am['inspection']['opaque_name_objects'])==1 and not ar['forensics'],'opaque/colliding names hold forensic interpretation')
            check(not (directory/'escape').exists() and not (ao/'files/wout.v1').exists(),'no path escape or ambiguous primary alias')
            check(any(bytes.fromhex(row['name_hex']).startswith(b'wout.v1\0\xa5') for row in am['objects']),'raw name padding preserved beyond SDK dirent normalization')
            check(imp.verify(ambiguous,ap,profile,pp,build_dir,bp,ao,ar['manifest_sha256'],True)['status']=='raw_import_verified','opaque import independently reproducible')
            with patch.object(imp,'copy_bytes',side_effect=OSError('injected destination failure')):
                rejected('destination-IO',lambda:run(directory/'write-error'))
            check((directory/'write-error/.incomplete').exists() and not (directory/'write-error/manifest.json').exists(),'destination failure cannot publish success')
            # Input/profile budgets and native child lifetime are real barriers.
            for label,change in (('array-profile',lambda v:[]),('wrong-sdk-shape',lambda v:{**v,'sdk_sources':[]}),
                                 ('boolean-disk-magic',lambda v:{**v,'disk_format':{**v['disk_format'],'magic':1}}),
                                 ('boolean-partition-type',lambda v:{**v,'storage':{**v['storage'],'type':True}})):
                value=change(imp.load(profile,pp));path=directory/(label+'.json');pin=imp.private_json(path,value)
                rejected(label,lambda path=path,pin=pin:imp.profile(path,pin))
            altered=imp.load(profile,pp);altered['sdk_sources']['spiffs/src/spiffs_nucleus.c']='44'*32
            bad_profile=directory/'altered-sdk-profile.json';bad_pin=imp.private_json(bad_profile,altered)
            rejected('SDK-fingerprint-mismatch',lambda:imp.build(bad_profile,bad_pin,idf,directory/'wrong-sdk-build'))
            check(not (directory/'wrong-sdk-build').exists(),'SDK mismatch rejected before compiler/output creation')
            old_hash=imp.hash_file(image_path)
            with open(image_path,'r+b') as writable:
                rejected('writable-input-fd',lambda:native.run(build_dir/'reader',writable,0x800000,8*1024*1024,directory/'writable-native.bin'))
            children=[];original_popen=native.subprocess.Popen
            def tracked(*positional,**keywords):
                child=original_popen(*positional,**keywords);children.append(child);return child
            with imp.forensic.open_regular(image_path) as incoming,patch.object(native.subprocess,'Popen',side_effect=tracked):
                try:native.run(build_dir/'reader',incoming,0x800000,8*1024*1024,directory/'timeout-native.bin',timeout=0.000001)
                except subprocess.TimeoutExpired:check(len(children)==1 and children[0].poll() is not None,'deadline kills and reaps the exact native child')
                else:check(False,'native deadline must expire in the test')
            check(imp.hash_file(image_path)==old_hash,'fd/deadline refusal cannot mutate source bytes')
            instrumented=native.build(idf,directory/'reader-instrumented',native.sdk_sources(idf),sanitize=True)
            for label,media,expected_stream in (('normal',image_path,imported/'spiffs-stream.bin'),('opaque',directory/'opaque-names-image.bin',ao/'spiffs-stream.bin')):
                with imp.forensic.open_regular(media) as incoming:
                    native.run(instrumented,incoming,0x800000,8*1024*1024,directory/('instrumented-'+label+'.bin'),sanitize=True)
                check(imp.hash_file(directory/('instrumented-'+label+'.bin'))==imp.hash_file(expected_stream),'instrumented decoder exact stream '+label)

            rejected('wrong-source-pin',lambda:run(directory/'wrong-source',source,'44'*32))
            rejected('wrong-profile-pin',lambda:imp.import_bundle(source,sp,profile,'44'*32,build_dir,bp,directory/'wrong-profile',True))
            rejected('wrong-build-pin',lambda:imp.import_bundle(source,sp,profile,pp,build_dir,'44'*32,directory/'wrong-build',True))
            rejected('existing-output',lambda:run(imported))
            rejected('output-inside-source',lambda:run(source/'nested'))
            forged=directory/'forged-consistent';shutil.copytree(imported,forged)
            fm=imp.load(forged/'manifest.json',receipt['manifest_sha256'])
            row=next(row for row in fm['objects'] if row['canonical_name']=='future-data')
            old=(forged/row['path']).read_bytes();changed=old[:-1]+bytes([old[-1]^1])
            for name in (row['path'],'files/future-data'):
                (forged/name).write_bytes(changed)
                fm['artifacts'][name]['sha256']=hashlib.sha256(changed).hexdigest()
            row['sha256']=hashlib.sha256(changed).hexdigest()
            encoded=imp.encoded(fm);(forged/'manifest.json').write_bytes(encoded)
            fp=hashlib.sha256(encoded).hexdigest()
            rejected('selfconsistent-derivation-forgery',lambda:imp.verify(source,sp,profile,pp,build_dir,bp,forged,fp,True))
            (imported/'files/wout.v1').write_bytes(b'forged')
            rejected('derived-file-tamper',lambda:imp.verify(source,sp,profile,pp,build_dir,bp,imported,receipt['manifest_sha256'],True))
            check(imp.file_inventory(source)==before,'all refusal paths preserve original source')
            if args.save_example:
                args.save_example.mkdir(mode=0o700)
                shutil.copytree(source,args.save_example/'synthetic-source')
                (args.save_example/'README.txt').write_text('SYNTHETIC HOST FIXTURE ONLY. Not actual device data or repair authorization.\n')
        finally:ForensicsTests.tearDownClass()
    print(f'Raw Flash import: {checks} checks, {failures} failures (SDK RO/native raw/forensics; synthetic media; NOT HIL)',flush=True)
    return 1 if failures else 0

if __name__=='__main__':raise SystemExit(main())
