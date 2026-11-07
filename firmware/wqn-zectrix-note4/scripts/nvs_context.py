#!/usr/bin/env python3
"""Pinned offline NVS context. No live device, credentials, migration or repair.

The SDK reads only the eleven storage-evidence keys in namespace wqn. Values
remain observations of an operator-pinned image, not proof of hardware origin.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

import raw_flash_import as raw
import nvs_context_native as native

KEYS=('cur_seq','cur_rnd','cur_dic','cur_rev','cur_int','cur_shf','cur_mis',
      'word_deck_gen','word_deck_chg','word_deck','word_seq_idx')
MISSING=0x1102
FORMAT='wqn-offline-nvs-context-v1'


def cursor(data):
    if len(data)!=52:return dict(valid=False,reason='cursor_length')
    magic,version,paused,sid,reserved,crc=struct.unpack('<IHH37s3sI',data)
    try:session=sid[:36].decode('ascii')
    except UnicodeDecodeError:session=''
    valid=(magic==0x57435552 and version==1 and paused in (0,1) and sid[36:]==b'\0' and
        raw.forensic.UUID.fullmatch(session) is not None and reserved==b'\0'*3 and raw.forensic.journal_crc(data[:48])==crc)
    if not valid:return dict(valid=False,reason='cursor_fields_or_crc')
    return dict(valid=True,session_id=session,paused=bool(paused))


def text(data):
    if not data or data[-1:]!=b'\0' or b'\0' in data[:-1] or len(data)>48:
        raise ValueError('noncanonical NVS text')
    try:return data[:-1].decode('ascii')
    except UnicodeDecodeError as error:raise ValueError('opaque NVS text') from error


def assess(result):
    expected={'format','flags','sdk_init_error','namespace_error','read_callbacks','write_attempts','erase_attempts','sandbox_denials','pages','values'}
    if not isinstance(result,dict) or set(result)!=expected or result.get('format')!='wqn-native-nvs-context-v1' or result.get('sandbox_denials')!=3:
        raise ValueError('invalid native NVS result')
    if any(type(result[name]) is not int for name in expected-{'format','pages','values'}) or not 0<=result['flags']<=255 or any(result[name]<0 for name in ('read_callbacks','write_attempts','erase_attempts','sandbox_denials')):
        raise ValueError('invalid native NVS counters')
    if not isinstance(result['pages'],list) or len(result['pages'])!=4:
        raise ValueError('incomplete native NVS page inventory')
    for index,page in enumerate(result['pages']):
        if not isinstance(page,dict) or set(page)!={'index','state','sequence','load_error'} or any(type(number) is not int for number in page.values()) or page['index']!=index or not 0<=page['state']<=0xffffffff or not 0<=page['sequence']<=0xffffffff:
            raise ValueError('invalid native NVS page inventory')
    values=result.get('values')
    if not isinstance(values,list) or len(values)!=len(KEYS) or any(not isinstance(value,dict) for value in values) or [value.get('key') for value in values]!=list(KEYS):
        raise ValueError('incomplete NVS allowlist result')
    global_valid=(result.get('flags')==0 and result.get('write_attempts')==0 and result.get('erase_attempts')==0 and
        result.get('sdk_init_error')==0 and result.get('namespace_error') in (0,MISSING))
    rows={};blockers=[]
    if not global_valid:blockers.append('nvs_structure_or_repair_attempt_unresolved')
    for value in values:
        if set(value)!={'key','sdk_error','parents','ambiguous','raw_hex','locations'} or type(value['sdk_error']) is not int or type(value['parents']) is not int or not 0<=value['parents']<=504 or type(value['ambiguous']) is not bool or not isinstance(value['locations'],list) or len(value['locations'])>504:
            raise ValueError('invalid native NVS row')
        for location in value['locations']:
            if not isinstance(location,dict) or set(location)!={'page','entry','type','chunk'} or any(type(number) is not int for number in location.values()) or not 0<=location['page']<4 or not 0<=location['entry']<126 or not 0<=location['type']<=255 or not 0<=location['chunk']<=255:
                raise ValueError('invalid native NVS location')
        key=value['key'];encoded=value.get('raw_hex')
        if not isinstance(encoded,str) or len(encoded)>32768 or re.fullmatch(r'(?:[0-9a-f]{2})*',encoded) is None:
            raise ValueError('invalid bounded NVS bytes')
        data=bytes.fromhex(encoded)
        row=dict(state='unknown',sdk_error=value['sdk_error'],raw_hex=encoded,locations=value['locations'])
        rows[key]=row
        if not global_valid or value['ambiguous']:
            continue
        if value['sdk_error']==MISSING:
            if value['parents']!=0 or value['locations'] or data:
                blockers.append(key+'_presence_disagreement');continue
            row['state']='absent';continue
        if value['sdk_error']!=0:
            blockers.append(key+'_read_error');continue
        row['state']='present_opaque'
        if key.startswith('cur_'):
            decoded=cursor(data)
            if decoded['valid']:row.update(state='present',cursor=decoded)
        elif key in ('word_deck_gen','word_seq_idx'):
            if len(data)==8:
                number=struct.unpack('<Q',data)[0]
                # Always retain u64 as decimal text: JSON/JS roundtrips must not
                # truncate high bits, even for an unsupported scope generation.
                row.update(u64_decimal=str(number))
                if key=='word_seq_idx' or number<=0xffffffff:row.update(state='present')
        else:
            try:value_text=text(data)
            except ValueError:pass
            else:
                if key=='word_deck':
                    if value_text=='' or raw.forensic.UUID.fullmatch(value_text):row.update(state='present',deck_id=value_text)
                else:
                    match=re.fullmatch(r'([0-9]{1,10})\|(.*)',value_text)
                    if match and 0<int(match[1])<=0xffffffff and (match[2]=='' or raw.forensic.UUID.fullmatch(match[2])):
                        row.update(state='present',pending_generation_decimal=str(int(match[1])),pending_deck_id=match[2])
        if row['state']!='present':blockers.append(key+'_semantic_value_unresolved')
    generation=rows['word_deck_gen'];marker=rows['word_deck_chg'];deck=rows['word_deck']
    if generation['state']!='present':blockers.append('committed_scope_generation_not_observed')
    if marker['state']!='absent':blockers.append('pending_scope_marker_present_or_unknown')
    if deck['state'] not in ('absent','present'):blockers.append('default_deck_context_unresolved')
    complete=global_valid and all(row['state'] in ('present','absent') for row in rows.values())
    return dict(format=FORMAT,namespace='wqn',namespace_found=result['namespace_error']==0 if global_valid else None,
        selected_values_read_complete=complete,scope_context_usable=global_valid and generation['state']=='present' and
        marker['state']=='absent' and deck['state'] in ('absent','present'),values=rows,blockers=sorted(set(blockers)),
        source_binding='operator_pinned_unattested',identity_authenticated=False,atomic_snapshot_proven=False,checkpoint_context_verified=False,
        repair_authorized=False,installation_ready=False)


def session_context(context,session):
    """Bind an observed NVS pause/generation to the retained Word session.

    This resolves only local-image NVS facts, never identity, pack admission,
    live scope readiness, authorization, or candidate installation.
    """
    mode=session.get('mode')
    if type(mode) is not int or mode not in range(7):raise ValueError('invalid retained session mode')
    mapping={0:'cur_seq',1:'cur_rnd',2:'cur_dic',3:'cur_rev',4:'cur_int',5:'cur_shf',6:'cur_mis'}
    row=context['values'].get(mapping.get(mode,''),{})
    observation=row.get('cursor',{})
    pause=(context['selected_values_read_complete'] and row.get('state')=='present' and
        observation.get('session_id')==session.get('session_id'))
    generation=context['values']['word_deck_gen']
    scope=(context['scope_context_usable'] and generation.get('u64_decimal')==str(session.get('scope_generation')))
    return dict(pause_bound_to_retained_session=pause,paused=observation['paused'] if pause else None,
        committed_scope_generation_matches_retained_session=scope,source_binding='operator_pinned_unattested',
        identity_authenticated=False,repair_authorized=False,installation_ready=False)


def capture(source,source_pin,profile,profile_pin,spiffs_build,spiffs_build_pin,imported,import_pin,
            nvs_build,nvs_build_pin,output,sensitive_confirmed=False):
    if not sensitive_confirmed:raise ValueError('NVS context needs explicit sensitive scope consent')
    reader=native.binary(nvs_build,nvs_build_pin)
    raw.verify(source,source_pin,profile,profile_pin,spiffs_build,spiffs_build_pin,imported,import_pin,True)
    im=raw.load(Path(imported)/'manifest.json',import_pin)
    output=Path(output)
    for parent in (Path(source),Path(imported)):
        if output.resolve().is_relative_to(parent.resolve()):raise ValueError('NVS output must be separate from source evidence')
    output.mkdir(mode=0o700);raw.wire.private(output/'.incomplete',b'NVS context incomplete\n')
    raw.wire.sync_dir(output);raw.wire.sync_dir(output.parent)
    with raw.forensic.open_regular(Path(imported)/'nvs-raw.bin') as incoming:
        info=raw.copy_bytes(incoming,output/'nvs-raw.bin',16384)
        if incoming.read(1):raise ValueError('NVS partition length differs')
    if info!=im['nvs_raw']:raise ValueError('NVS bytes differ from pinned raw import')
    with raw.forensic.open_regular(output/'nvs-raw.bin') as incoming:
        result=native.run(reader,incoming,output/'native.json')
    context=assess(result)
    context['binding']=dict(source_manifest_sha256=source_pin,raw_import_manifest_sha256=import_pin,
        raw_import_image_sha256=im['image']['sha256'],nvs_sha256=info['sha256'],forensics=im['forensics'],
        nvs_reader_build_manifest_sha256=nvs_build_pin,assessment_source_sha256=raw.hash_file(Path(__file__)),raw_import_independently_replayed=True)
    raw.private_json(output/'context.json',context)
    # Recheck the original source and import after all reads. NVS observations
    # cannot be attached to a changed main image or a different forensic bundle.
    raw.verify(source,source_pin,profile,profile_pin,spiffs_build,spiffs_build_pin,imported,import_pin,True)
    if raw.hash_file(output/'nvs-raw.bin')!=info['sha256']:raise ValueError('NVS evidence changed')
    artifacts=raw.file_inventory(output);artifacts.pop('.incomplete')
    manifest=dict(format='wqn-offline-nvs-context-bundle-v1',binding=context['binding'],artifacts=artifacts,
        selected_values_read_complete=context['selected_values_read_complete'],repair_authorized=False,
        identity_authenticated=False,installation_ready=False)
    pin=raw.private_json(output/'manifest.json',manifest);raw.wire.sync_dir(output)
    try:(output/'.incomplete').unlink();raw.wire.sync_dir(output)
    except OSError:
        if not (output/'.incomplete').exists():raw.wire.private(output/'.incomplete',b'NVS finalization uncertain\n')
        raise
    return dict(status='nvs_context_preserved',manifest_sha256=pin,selected_values_read_complete=context['selected_values_read_complete'],
        scope_context_usable=context['scope_context_usable'],blockers=context['blockers'],repair_authorized=False,installation_ready=False)


def verify(*values,expected_manifest_sha256,sensitive_confirmed=False):
    if not sensitive_confirmed:raise ValueError('NVS verification needs sensitive scope consent')
    output=Path(values[-1])
    if (output/'.incomplete').exists():raise ValueError('NVS context incomplete')
    manifest=raw.load(output/'manifest.json',expected_manifest_sha256)
    if not isinstance(manifest,dict) or manifest.get('format')!='wqn-offline-nvs-context-bundle-v1' or manifest.get('repair_authorized') is not False:
        raise ValueError('invalid NVS context manifest')
    before=raw.file_inventory(output)
    if {name:info for name,info in before.items() if name!='manifest.json'}!=manifest.get('artifacts'):
        raise ValueError('NVS context inventory differs')
    with tempfile.TemporaryDirectory(prefix='wqn-nvs-replay-') as temp:
        replay=Path(temp)/'replay'
        receipt=capture(*values[:-1],replay,True)
        if receipt['manifest_sha256']!=expected_manifest_sha256 or raw.file_inventory(replay)!=before:
            raise ValueError('NVS context differs from original source replay')
    if raw.file_inventory(output)!=before:raise ValueError('NVS evidence changed during replay')
    return dict(status='nvs_context_verified',manifest_sha256=expected_manifest_sha256,repair_authorized=False,installation_ready=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    p=commands.add_parser('build');p.add_argument('--idf-path',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    names=('source-bundle','source-manifest-sha256','profile','profile-sha256','spiffs-build','spiffs-build-manifest-sha256',
        'import-bundle','import-manifest-sha256','nvs-build','nvs-build-manifest-sha256','output')
    for command in ('capture','verify'):
        p=commands.add_parser(command)
        for name in names:p.add_argument('--'+name,required=True,type=str if name.endswith('sha256') else Path)
        p.add_argument('--sensitive-main-flash',action='store_true')
        if command=='verify':p.add_argument('--expected-manifest-sha256',required=True)
    args=parser.parse_args()
    if args.command=='build':result=native.build(args.idf_path,args.output)
    else:
        values=tuple(getattr(args,name.replace('-','_')) for name in names)
        if args.command=='capture':result=capture(*values,args.sensitive_main_flash)
        else:result=verify(*values,expected_manifest_sha256=args.expected_manifest_sha256,sensitive_confirmed=args.sensitive_main_flash)
    print(raw.encoded(result).decode(),end='')
    return 2 if result.get('blockers') else 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError,subprocess.SubprocessError,raw.native.NativeExecutionError) as error:
        print(raw.encoded(dict(status='nvs_context_incomplete',error=str(error),repair_authorized=False)).decode(),file=sys.stderr,end='')
        raise SystemExit(1)
