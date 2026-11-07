#!/usr/bin/env python3
"""Import pinned WQR1 evidence through a read-only SDK SPIFFS child. No device access.

Profiles bind format assumptions to an operator-pinned build, not authenticated
hardware. Unknown/ciphertext/ambiguous sources remain held; never repairs media.
"""
from collections import Counter
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import outbox_forensics as forensic
import raw_flash_evidence_receive as raw
import storage_evidence_receive as wire
import spiffs_evidence_native as native

PROFILE='wqn-raw-import-profile-v1'
FORMAT='wqn-raw-flash-import-v1'
MAX_JSON=32*1024*1024
MAX_OBJECTS=32767
PIN=re.compile('[0-9a-f]{64}')
OBJECT=struct.Struct('<IIII32s4s')
TABLE_OFFSET=0x8000
STORAGE=dict(name='storage',type=1,subtype=130,offset=0x800000,bytes=8*1024*1024,flags=0)
NVS=dict(name='nvs',type=1,subtype=2,offset=0x9000,bytes=0x4000,flags=0)


def encoded(value):
    return (json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True)+'\n').encode()


def pairs(rows):
    out={}
    for key,value in rows:
        if key in out: raise ValueError('duplicate JSON key')
        out[key]=value
    return out


def load(path,pin=None,limit=MAX_JSON):
    with forensic.open_regular(path) as stream: data=stream.read(limit+1)
    if len(data)>limit: raise ValueError('JSON budget exceeded')
    if pin is not None:
        wire.pin(pin,32)
        if hashlib.sha256(data).hexdigest()!=pin.lower():raise ValueError('JSON pin mismatch')
    try:
        return json.loads(data,object_pairs_hook=pairs,parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON')))
    except RecursionError as error:
        raise ValueError('JSON nesting budget exceeded') from error


def hash_file(path):
    digest=hashlib.sha256()
    with forensic.open_regular(path) as stream:
        for block in iter(lambda:stream.read(65536),b''):digest.update(block)
    return digest.hexdigest()


def private_json(path,value):
    data=encoded(value)
    if len(data)>MAX_JSON:raise ValueError('manifest budget exceeded')
    wire.private(path,data);return hashlib.sha256(data).hexdigest()


def partition_table(data,image_bytes):
    if len(data)!=0xc00:raise ValueError('partition table size mismatch')
    entries=[];end=None
    for offset in range(0,len(data),32):
        row=data[offset:offset+32]
        magic,=struct.unpack_from('<H',row)
        if magic==0xebeb:
            if row[2:16]!=b'\xff'*14 or row[16:]!=hashlib.md5(data[:offset]).digest():
                raise ValueError('partition table MD5 mismatch')
            end=offset+32;break
        if magic!=0x50aa:raise ValueError('partition MD5 trailer missing or invalid entry')
        _,kind,subtype,start,size,name,flags=struct.unpack('<HBBII16sI',row)
        at=name.find(b'\0')
        if at<1 or any(name[at:]) or not re.fullmatch(rb'[A-Za-z0-9_-]{1,15}',name[:at]):
            raise ValueError('opaque partition name')
        if size==0 or start<TABLE_OFFSET+0x1000 or start>image_bytes or size>image_bytes-start or start%4096 or size%4096:
            raise ValueError('invalid partition range')
        if flags not in (0,1):raise ValueError('unsupported partition flags')
        entries.append(dict(name=name[:at].decode('ascii'),type=kind,subtype=subtype,offset=start,bytes=size,flags=flags))
    if end is None or any(value!=255 for value in data[end:]):raise ValueError('unknown partition trailer')
    if len({entry['name'] for entry in entries})!=len(entries):raise ValueError('duplicate partition name')
    ordered=sorted(entries,key=lambda entry:entry['offset'])
    if any(a['offset']+a['bytes']>b['offset'] for a,b in zip(ordered,ordered[1:])):
        raise ValueError('overlapping partitions')
    return entries


def create_profile(baseline,baseline_pin,config,config_pin,idf,output):
    release=load(baseline,baseline_pin)
    resolved=load(config,config_pin)
    if not isinstance(release,dict) or not isinstance(resolved,dict) or not isinstance(release.get('flash_settings'),dict) or not isinstance(release.get('flash_files'),dict) or not isinstance(release.get('embedded_app_description'),dict) or not isinstance(release.get('artifacts'),list):
        raise ValueError('invalid baseline/config shape')
    if not {'target','resolved_sdkconfig_sha256'}<=set(release) or not {'elf_sha256','version'}<=set(release['embedded_app_description']):
        raise ValueError('missing baseline fields')
    if any(not isinstance(item,dict) or not {'path','sha256'}<=set(item) or not isinstance(item['path'],str) for item in release['artifacts']):
        raise ValueError('invalid baseline artifact inventory')
    if config_pin!=release['resolved_sdkconfig_sha256']:raise ValueError('config differs from baseline fingerprint')
    expected={'SPIFFS_PAGE_SIZE':256,'SPIFFS_OBJ_NAME_LEN':32,'SPIFFS_META_LENGTH':4,
              'SPIFFS_USE_MAGIC':True,'SPIFFS_USE_MAGIC_LENGTH':True,'SPIFFS_PAGE_CHECK':True}
    if any(type(resolved.get(key)) is not type(value) or resolved.get(key)!=value for key,value in expected.items()):
        raise ValueError('unsupported SPIFFS format/config')
    if release['target']!='esp32s3' or release['flash_settings'].get('flash_size')!='16MB':
        raise ValueError('unsupported device baseline')
    layout=release['flash_files']
    if layout.get('0x8000')!='partition_table/partition-table.bin':raise ValueError('partition table baseline unavailable')
    table=Path(baseline).parent/'partition_table/partition-table.bin'
    artifacts={item['path']:item for item in release['artifacts']}
    if len(artifacts)!=len(release['artifacts']):raise ValueError('duplicate baseline artifact path')
    if artifacts.get('partition_table/partition-table.bin',{}).get('sha256')!=hash_file(table):
        raise ValueError('baseline partition binary differs')
    with forensic.open_regular(table) as source: entries=partition_table(source.read(0xc01),16*1024*1024)
    if STORAGE not in entries or NVS not in entries:raise ValueError('unsupported storage partition layout')
    value=dict(format=PROFILE,baseline_sha256=baseline_pin,firmware_elf_sha256=release['embedded_app_description']['elf_sha256'],
        firmware_version=release['embedded_app_description']['version'],resolved_sdkconfig_sha256=config_pin,
        image_bytes=16*1024*1024,partition_table_offset=TABLE_OFFSET,storage=STORAGE,nvs=NVS,disk_format=native.FORMAT,
        sdk_sources=native.sdk_sources(idf),profile_binding='operator_pinned_unattested',repair_authorized=False)
    pin=private_json(output,value);wire.sync_dir(Path(output).parent)
    return dict(status='profile_saved',profile_sha256=pin,identity_authenticated=False,repair_authorized=False)


def profile(path,pin):
    value=load(path,pin,16384)
    keys={'format','baseline_sha256','firmware_elf_sha256','firmware_version','resolved_sdkconfig_sha256','image_bytes',
          'partition_table_offset','storage','nvs','disk_format','sdk_sources','profile_binding','repair_authorized'}
    if not isinstance(value,dict) or set(value)!=keys or value['format']!=PROFILE or value['profile_binding']!='operator_pinned_unattested' or value['repair_authorized'] is not False:
        raise ValueError('invalid raw import profile')
    for key in ('baseline_sha256','firmware_elf_sha256','resolved_sdkconfig_sha256'):wire.pin(value[key],32)
    if not isinstance(value['firmware_version'],str) or not 1<=len(value['firmware_version'])<=31:
        raise ValueError('invalid profile firmware version')
    if value['image_bytes']!=16*1024*1024 or value['partition_table_offset']!=TABLE_OFFSET or encoded(value['storage'])!=encoded(STORAGE) or encoded(value['nvs'])!=encoded(NVS):
        raise ValueError('unsupported image/partition profile')
    if not isinstance(value['disk_format'],dict) or set(value['disk_format'])!=set(native.FORMAT) or any(type(value['disk_format'][key]) is not type(item) or value['disk_format'][key]!=item for key,item in native.FORMAT.items()):
        raise ValueError('unsupported disk format')
    if not isinstance(value['sdk_sources'],dict) or set(value['sdk_sources'])!=set(native.FILES) or any(not isinstance(item,str) or PIN.fullmatch(item) is None for item in value['sdk_sources'].values()):
        raise ValueError('invalid SDK source pin')
    return value


def build(profile_path,profile_pin,idf,output):
    expected=profile(profile_path,profile_pin)
    binary=native.build(idf,output,expected['sdk_sources'])
    value=dict(format='wqn-spiffs-reader-build-v1',profile_sha256=profile_pin,helper_source_sha256=hash_file(Path(native.__file__).with_name('native')/'spiffs_evidence_reader.c'),
               binary_sha256=hash_file(binary),sdk_sources=expected['sdk_sources'],readonly=True,
               host_adaptations=native.ADAPTATIONS,adapted_config_sha256=hash_file(Path(output)/'spiffs_config.h'),
               wrapper_source_sha256=hash_file(Path(native.__file__)))
    pin=private_json(Path(output)/'build.json',value);wire.sync_dir(output)
    return dict(status='reader_built',build_manifest_sha256=pin,identity_authenticated=False,repair_authorized=False)


def binary(build_dir,build_pin,expected,profile_pin):
    value=load(Path(build_dir)/'build.json',build_pin,16384)
    if not isinstance(value,dict) or set(value)!={'format','profile_sha256','helper_source_sha256','binary_sha256','sdk_sources','readonly','host_adaptations','adapted_config_sha256','wrapper_source_sha256'} or value['format']!='wqn-spiffs-reader-build-v1' or value['readonly'] is not True:
        raise ValueError('invalid reader build manifest')
    if value['profile_sha256']!=profile_pin or value['sdk_sources']!=expected['sdk_sources'] or value['helper_source_sha256']!=hash_file(Path(native.__file__).with_name('native')/'spiffs_evidence_reader.c'):
        raise ValueError('reader profile/source differs')
    if encoded(value['host_adaptations'])!=encoded(native.ADAPTATIONS) or value['adapted_config_sha256']!=hash_file(Path(build_dir)/'spiffs_config.h') or value['wrapper_source_sha256']!=hash_file(Path(native.__file__)):
        raise ValueError('reader host adaptation or process wrapper differs')
    result=Path(build_dir)/'reader'
    if hash_file(result)!=value['binary_sha256']:raise ValueError('reader binary differs')
    return result


def copy_bytes(source,target,count=None):
    fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    digest=hashlib.sha256();total=0
    with os.fdopen(fd,'wb') as destination:
        while count is None or total<count:
            data=source.read(65536 if count is None else min(65536,count-total))
            if not data:break
            destination.write(data);digest.update(data);total+=len(data)
        if count is not None and total!=count:raise ValueError('truncated object/source')
        destination.flush();os.fsync(destination.fileno())
    return dict(bytes=total,sha256=digest.hexdigest())


def name_bytes(value):
    at=value.find(b'\0')
    if at<0:return None
    text=value[:at]
    if text.startswith(b'/'):text=text[1:]
    if text in (b'.',b'..') or re.fullmatch(rb'[A-Za-z0-9_.-]{1,31}',text) is None:return None
    return text.decode('ascii')


def unpack_stream(path,output):
    (output/'objects').mkdir(mode=0o700);(output/'files').mkdir(mode=0o700)
    rows=[];ids=set();total=0
    with forensic.open_regular(path) as stream:
        if stream.read(6)!=b'WQSP1\n':raise ValueError('unknown SPIFFS stream')
        while True:
            kind=stream.read(1)
            if kind==b'\0':break
            if kind!=b'\1':raise ValueError('truncated SPIFFS inventory')
            header=stream.read(OBJECT.size)
            if len(header)!=OBJECT.size:raise ValueError('truncated object header')
            index,obj_id,size,page,name,metadata=OBJECT.unpack(header)
            if index!=len(rows)+1 or index>MAX_OBJECTS or obj_id in ids or not 0<obj_id<32768 or page>=32768 or size>16*1024*1024 or total+size>32*1024*1024:
                raise ValueError('invalid object inventory/budget')
            ids.add(obj_id);total+=size
            filename=f'objects/{index:05d}.bin'
            info=copy_bytes(stream,output/filename,size)
            rows.append(dict(index=index,object_id=obj_id,page_index=page,name_hex=name.hex(),metadata_hex=metadata.hex(),
                             canonical_name=name_bytes(name),path=filename,**info))
        trailer=stream.read(20)
        if len(trailer)!=20 or stream.read(1):raise ValueError('invalid SPIFFS finish')
        count,reads,writes,erases,denied=struct.unpack('<IIIII',trailer)
        if count!=len(rows) or writes or erases or denied!=3:raise ValueError('unverified read-only sandbox result')
    occurrences=Counter(row['canonical_name'] for row in rows if row['canonical_name'] is not None)
    ambiguous=sorted(name for name,count in occurrences.items() if count>1)
    opaque=[]
    for row in rows:
        name=row['canonical_name']
        if name is None:opaque.append(row['index']);continue
        if occurrences[name]!=1:continue
        with forensic.open_regular(output/row['path']) as source:copy_bytes(source,output/'files'/name,row['bytes'])
    wire.sync_dir(output/'objects');wire.sync_dir(output/'files')
    return rows,dict(read_callbacks=reads,source_write_attempts=writes,source_erase_attempts=erases,
        sandbox_denials_checked=denied,ambiguous_canonical_names=ambiguous,opaque_name_objects=opaque,
        logical_inventory_usable=not ambiguous and not opaque)


def import_bundle(source,source_pin,profile_path,profile_pin,build_dir,build_pin,output,sensitive_confirmed=False):
    if not sensitive_confirmed:raise ValueError('raw import requires explicit sensitive scope consent')
    expected=profile(profile_path,profile_pin);reader=binary(build_dir,build_pin,expected,profile_pin)
    raw.verify_bundle(source,source_pin,True)
    original=load(Path(source)/'manifest.json',source_pin)
    if original['raw_image_complete'] is not True or original['flash_encryption_enabled'] is not False:
        raise ValueError('partial or encrypted Flash is not interpretable by this reader')
    if original['data_bytes']!=expected['image_bytes'] or original['identity']['firmware_elf_sha256']!=expected['firmware_elf_sha256']:
        raise ValueError('raw source does not match pinned build/profile')
    output=Path(output);source=Path(source)
    if output.resolve().is_relative_to(source.resolve()):raise ValueError('output must be separate from raw evidence')
    output.mkdir(mode=0o700);wire.private(output/'.incomplete',b'raw import not finalized\n');wire.sync_dir(output);wire.sync_dir(output.parent)
    try:
        with forensic.open_regular(source/'main-flash.bin') as stream:info=copy_bytes(stream,output/'main-flash.bin',expected['image_bytes'])
        if info['sha256']!=original['image_sha256']:raise ValueError('copied image differs from pinned evidence')
        with forensic.open_regular(output/'main-flash.bin') as stream:
            stream.seek(TABLE_OFFSET);table=stream.read(0xc00);entries=partition_table(table,expected['image_bytes'])
            if expected['storage'] not in entries or expected['nvs'] not in entries:
                raise ValueError('source partition layout or encryption differs from profile')
            # App descriptors provide a consistency check only, never provenance.
            matches=[]
            for entry in entries:
                if entry['type']!=0:continue
                stream.seek(entry['offset']);header=stream.read(208)
                if len(header)==208 and header[0]==0xe9 and struct.unpack_from('<I',header,32)[0]==0xabcd5432 and header[176:208].hex()==expected['firmware_elf_sha256']:
                    matches.append(entry['name'])
            if not matches:raise ValueError('no app descriptor matches the declared ELF')
            stream.seek(NVS['offset']);nvs=copy_bytes(stream,output/'nvs-raw.bin',NVS['bytes'])
            native.run(reader,stream,STORAGE['offset'],STORAGE['bytes'],output/'spiffs-stream.bin')
        rows,inspection=unpack_stream(output/'spiffs-stream.bin',output)
        forensic_pins={};blockers=[]
        if inspection['logical_inventory_usable']:
            for domain in ('word','note'):
                try:
                    receipt=forensic.capture(output/'files',output/('forensics-'+domain),domain,True)
                    forensic_pins[domain]=receipt['manifest_sha256']
                    forensic.verify(output/('forensics-'+domain),receipt['manifest_sha256'])
                except ValueError:
                    blockers.append(domain+'_forensic_import_requires_review')
        else:blockers.append('opaque_or_ambiguous_logical_names')
        raw.verify_bundle(source,source_pin,True)
        if hash_file(output/'main-flash.bin')!=info['sha256']:raise ValueError('copied image changed')
        names=Counter(row['canonical_name'] for row in rows if row['canonical_name'] is not None)
        for row in rows:
            if hash_file(output/row['path'])!=row['sha256']:raise ValueError('derived object changed')
            if row['canonical_name'] is not None and names[row['canonical_name']]==1 and hash_file(output/'files'/row['canonical_name'])!=row['sha256']:
                raise ValueError('derived alias changed')
        if hash_file(output/'nvs-raw.bin')!=nvs['sha256']:raise ValueError('raw NVS copy changed')
        artifacts=file_inventory(output);artifacts.pop('.incomplete')
        value=dict(format=FORMAT,source_manifest_sha256=source_pin,profile_sha256=profile_pin,build_manifest_sha256=build_pin,
            source_identity=original['identity'],image=info,partition_table_sha256=hashlib.sha256(table).hexdigest(),partitions=entries,
            matching_app_descriptors=matches,nvs_raw=nvs,nvs_decoded=False,objects=rows,inspection=inspection,
            forensics=forensic_pins,analysis_blockers=blockers,checkpoint_context_verified=False,
            artifacts=artifacts,
            checkpoint_blockers=['nvs_context_not_decoded','source_identity_not_attested'],
            filesystem_consistency_proven=False,visible_file_enumeration_only=True,complete_device_backup=False,
            atomic_snapshot_proven=False,identity_authenticated=False,repair_authorized=False,installation_ready=False,
            source_binding='operator_pinned_unattested')
        pin=private_json(output/'manifest.json',value);wire.sync_dir(output)
        try:(output/'.incomplete').unlink();wire.sync_dir(output)
        except OSError:
            if not (output/'.incomplete').exists():wire.private(output/'.incomplete',b'raw import finalization uncertain\n')
            raise
        return dict(status='raw_import_preserved',manifest_sha256=pin,objects=len(rows),forensics=forensic_pins,
                    analysis_blockers=blockers,identity_authenticated=False,repair_authorized=False,installation_ready=False)
    except BaseException:
        # Raw copy and partial stream/objects remain private; do not overwrite.
        raise


def file_inventory(path):
    if Path(path).is_symlink() or not Path(path).is_dir():raise ValueError('invalid import directory')
    result={}
    for item in sorted(Path(path).rglob('*')):
        if item.is_symlink():raise ValueError('symlink in import output')
        if item.is_file():result[str(item.relative_to(path))]=dict(bytes=item.stat().st_size,sha256=hash_file(item))
        elif not item.is_dir():raise ValueError('unsupported import artifact type')
    return result


def verify(source,source_pin,profile_path,profile_pin,build_dir,build_pin,output,output_pin,sensitive_confirmed=False):
    if not sensitive_confirmed:raise ValueError('raw verification requires sensitive scope confirmation')
    if (Path(output)/'.incomplete').exists():raise ValueError('import incomplete')
    manifest=load(Path(output)/'manifest.json',output_pin)
    if not isinstance(manifest,dict) or manifest.get('format')!=FORMAT or manifest.get('repair_authorized') is not False or manifest.get('identity_authenticated') is not False:
        raise ValueError('invalid import manifest')
    before=file_inventory(output)
    if {key:value for key,value in before.items() if key!='manifest.json'}!=manifest.get('artifacts'):
        raise ValueError('derived artifact inventory differs from pinned manifest')
    with tempfile.TemporaryDirectory(prefix='wqn-raw-import-verify-') as directory:
        replay=Path(directory)/'replay'
        receipt=import_bundle(source,source_pin,profile_path,profile_pin,build_dir,build_pin,replay,True)
        if receipt['manifest_sha256']!=output_pin or file_inventory(replay)!=before:
            raise ValueError('import differs from independent source replay')
    if file_inventory(output)!=before:raise ValueError('import changed during replay')
    return dict(status='raw_import_verified',manifest_sha256=output_pin,identity_authenticated=False,repair_authorized=False,installation_ready=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    p=commands.add_parser('profile');p.add_argument('--baseline',type=Path,required=True);p.add_argument('--baseline-sha256',required=True)
    p.add_argument('--resolved-sdkconfig',type=Path,required=True);p.add_argument('--sdkconfig-sha256',required=True);p.add_argument('--idf-path',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p=commands.add_parser('build');p.add_argument('--profile',type=Path,required=True);p.add_argument('--profile-sha256',required=True);p.add_argument('--idf-path',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    for command in ('import','verify'):
        p=commands.add_parser(command)
        for name in ('source-bundle','profile','build-dir','output'):p.add_argument('--'+name,type=Path,required=True)
        for name in ('source-manifest-sha256','profile-sha256','build-manifest-sha256'):p.add_argument('--'+name,required=True)
        p.add_argument('--sensitive-main-flash',action='store_true')
        if command=='verify':p.add_argument('--expected-manifest-sha256',required=True)
    args=parser.parse_args()
    if args.command=='profile':result=create_profile(args.baseline,args.baseline_sha256,args.resolved_sdkconfig,args.sdkconfig_sha256,args.idf_path,args.output)
    elif args.command=='build':result=build(args.profile,args.profile_sha256,args.idf_path,args.output)
    else:
        values=(args.source_bundle,args.source_manifest_sha256,args.profile,args.profile_sha256,args.build_dir,args.build_manifest_sha256,args.output)
        result=import_bundle(*values,args.sensitive_main_flash) if args.command=='import' else verify(*values,args.expected_manifest_sha256,args.sensitive_main_flash)
    print(json.dumps(result,sort_keys=True))
    return 2 if result.get('analysis_blockers') else 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError,subprocess.SubprocessError,native.NativeExecutionError) as error:
        print(json.dumps(dict(status='raw_import_incomplete',error=str(error),repair_authorized=False)),file=sys.stderr)
        raise SystemExit(1)
