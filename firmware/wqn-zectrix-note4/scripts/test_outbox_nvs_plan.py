#!/usr/bin/env python3
"""Pinned raw image -> NVS -> review plan -> native candidate. Synthetic media; NOT HIL."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import patch

import nvs_context as ctx
import nvs_context_native as native
import outbox_candidate as candidate
import outbox_checkpoint as checkpoint
import outbox_nvs_context as adapter
import outbox_reconcile as api
import raw_flash_import as raw
import test_outbox_reconcile as fixtures
from test_nvs_context import IDF, IDF_PYTHON
from test_raw_flash_evidence import build as build_raw

ROOT=Path(__file__).resolve().parents[1]


class NvsPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixtures.M=api;fixtures.ReconcileTests.setUpClass()
        cls.temp=tempfile.TemporaryDirectory(prefix='wqn-nvs-plan-tools-');cls.tools=Path(cls.temp.name)
        table=cls.tools/'partition_table';table.mkdir()
        subprocess.run([IDF_PYTHON,str(IDF/'components/partition_table/gen_esp32part.py'),'--quiet',
            str(ROOT/'partitions/16m.csv'),str(table/'partition-table.bin')],check=True,timeout=30)
        config=cls.tools/'config.json';config.write_bytes(raw.encoded(dict(SPIFFS_PAGE_SIZE=256,SPIFFS_OBJ_NAME_LEN=32,
            SPIFFS_META_LENGTH=4,SPIFFS_USE_MAGIC=True,SPIFFS_USE_MAGIC_LENGTH=True,SPIFFS_PAGE_CHECK=True)))
        baseline=cls.tools/'baseline.json';baseline.write_bytes(raw.encoded(dict(target='esp32s3',flash_settings=dict(flash_size='16MB'),
            flash_files={'0x8000':'partition_table/partition-table.bin'},resolved_sdkconfig_sha256=raw.hash_file(config),
            embedded_app_description=dict(version='SYNTHETIC_NVS_PLAN',elf_sha256='33'*32),
            artifacts=[dict(path='partition_table/partition-table.bin',sha256=raw.hash_file(table/'partition-table.bin'))])))
        cls.profile=cls.tools/'profile.json'
        cls.pp=raw.create_profile(baseline,raw.hash_file(baseline),config,raw.hash_file(config),IDF,cls.profile)['profile_sha256']
        cls.sb=cls.tools/'spiffs-reader';cls.sp=raw.build(cls.profile,cls.pp,IDF,cls.sb)['build_manifest_sha256']
        cls.nb=cls.tools/'nvs-reader';cls.np=native.build(IDF,cls.nb)['build_manifest_sha256']
        rb=cls.tools/'raw-build';rb.mkdir();cls.producer=build_raw(ROOT,rb,None)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup();fixtures.ReconcileTests.tearDownClass()

    @staticmethod
    def cursor(sid,paused):
        payload=struct.pack('<IHH37s3s',0x57435552,1,int(paused),sid.encode(),b'\0'*3)
        return payload+struct.pack('<I',raw.forensic.journal_crc(payload))

    def make(self, *, modes=(0,), nvs_pause=True, snapshot_pause=False, legacy=None, state='valid', generation=None, marker=False, terminal=False):
        case=fixtures.ReconcileTests();self.addCleanup(case.doCleanups)
        names={};journal=bytearray();sessions={}
        def value(setting,mode):return setting[mode] if isinstance(setting,dict) else setting
        base=fixtures.ReconcileTests.native/'word'
        for mode in modes:
            session=bytearray((base/'checkpoint-session.bin').read_bytes())
            original=checkpoint.decode_session(session,'word')['value']
            sid=original['session_id'][:-1]+str(mode+1)
            session=session.replace(original['session_id'].encode(),sid.encode())
            session[17]=int(value(snapshot_pause,mode));session[19]=mode
            if terminal:session[23]=0  # has_more must be false to exercise terminal replay.
            struct.pack_into('<I',session,12,raw.forensic.journal_crc(session[16:]))
            sessions[mode]=checkpoint.decode_session(session,'word')['value']
            names[checkpoint.STEMS[mode]+'.v1']=bytes(session)
            records=bytearray((base/'healthy.bin').read_bytes())
            for at in range(0,len(records),200):
                records[at+8]=mode;records[at+24]=ord('a')+mode
                if records[at+6]==1:
                    records[at+89:at+125]=sid.encode()
                    if terminal:struct.pack_into('<I',records,at+20,sessions[mode]['items'][-1]['ordinal']+1)
                struct.pack_into('<I',records,at+196,raw.forensic.journal_crc(records[at:at+196]))
                if terminal and records[at+6]==1:
                    fact,_,_=raw.forensic.decode_record(bytes(records[at:at+200]),'word',at)
                    self.assertEqual(fact['sequence'],struct.unpack_from('<Q',(base/'healthy.bin').read_bytes(),at+12)[0])
                    self.assertEqual(fact['local_checkpoint']['next_position'],sessions[mode]['items'][-1]['ordinal']+1)
            journal.extend(records)
            if legacy is not None:
                names[checkpoint.STEMS[mode]+'.cur']=(value(legacy,mode) if isinstance(value(legacy,mode),bytes)
                    else self.cursor(sid,bool(value(legacy,mode))))
        names['wout.v1']=bytes(journal);case.source(names=names)
        path=case.root/'raw-context';path.mkdir()
        spiffs=path/'spiffs.bin'
        subprocess.run([IDF_PYTHON,str(IDF/'components/spiffs/spiffsgen.py'),str(8*1024*1024),str(case.root/'export'),str(spiffs)],check=True,timeout=30)
        first=next(iter(sessions.values()));rows=[('key','type','encoding','value'),('wqn','namespace','','')]
        observed_generation=first['scope_generation'] if generation is None else first['scope_generation']+1 if generation=='mismatch' else generation
        rows += [('word_deck_gen','data','u64',str(observed_generation)),
                 ('word_deck','data','string',first['deck_ids'][0])]
        if marker:rows.append(('word_deck_chg','data','string',str(first['scope_generation']+1)+'|'+first['deck_ids'][0]))
        if state!='absent':
            for mode,session in sessions.items():
                sid=session['session_id'] if state!='stale' else '22222222-2222-4333-8444-555555555555'
                data=self.cursor(sid,value(nvs_pause,mode))
                if state=='crc':data=data[:-1]+bytes([data[-1]^1])
                if state=='short':data=data[:-1]
                rows.append((ctx.KEYS[mode],'data','string' if state=='wrong-type' else 'hex2bin',
                             'opaque' if state=='wrong-type' else data.hex()))
        csvpath=path/'nvs.csv';nvs=path/'nvs.bin'
        with csvpath.open('w') as stream:csv.writer(stream).writerows(rows)
        subprocess.run([IDF_PYTHON,str(IDF/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'),
            'generate',str(csvpath),str(nvs),'0x4000','--version','2'],check=True,timeout=30,stdout=subprocess.DEVNULL)
        image=bytearray(b'\xff'*(16*1024*1024));image[0x8000:0x8c00]=(self.tools/'partition_table/partition-table.bin').read_bytes()
        image[0x9000:0xd000]=nvs.read_bytes();image[0x800000:]=spiffs.read_bytes()
        image[0x20000]=0xe9;struct.pack_into('<I',image,0x20020,0xabcd5432);image[0x200b0:0x200d0]=bytes.fromhex('33'*32)
        main=path/'main.bin';main.write_bytes(image);wire=path/'wire.bin'
        done=subprocess.run([str(self.producer),'supplied-image',str(wire),str(main)],capture_output=True,text=True,check=True,timeout=60)
        if not json.loads(done.stdout)['passed'] or done.stderr:raise RuntimeError('raw collector fixture failed')
        source=path/'source';source_pin=raw.raw.receive(wire,source,'11'*16,'22'*6,'33'*32,sensitive_confirmed=True)['manifest_sha256']
        imported=path/'imported';ip=raw.import_bundle(source,source_pin,self.profile,self.pp,self.sb,self.sp,imported,True)['manifest_sha256']
        self.assertEqual(raw.load(imported/'manifest.json')['forensics']['word'],case.pin)
        context=path/'context';cr=ctx.capture(source,source_pin,self.profile,self.pp,self.sb,self.sp,imported,ip,self.nb,self.np,context,True)
        descriptor=path/'inputs.json'
        spec=dict(format=adapter.FORMAT,source_bundle=str(source),source_manifest_sha256=source_pin,
            profile=str(self.profile),profile_sha256=self.pp,spiffs_build=str(self.sb),spiffs_build_manifest_sha256=self.sp,
            import_bundle=str(imported),import_manifest_sha256=ip,nvs_build=str(self.nb),nvs_build_manifest_sha256=self.np,
            context_bundle=str(context),context_manifest_sha256=cr['manifest_sha256'])
        descriptor.write_bytes(api.encoded(spec))
        evidence,ep,_=case.collect()
        return dict(case=case,sessions=sessions,spec=spec,descriptor=descriptor,context=context,
            options=dict(nvs_inputs=descriptor,nvs_inputs_pin=raw.hash_file(descriptor),sensitive_confirmed=True),
            common=(case.bundle,case.pin,evidence,ep),source_before=raw.file_inventory(source),source=source)

    def plan(self,data,**options):return api.review_plan(*data['common'],**{**data['options'],**options})

    def native(self,data,verify=True):
        case=data['case'];plan=self.plan(data);pd=case.root/'plan';api.start_output(pd)
        pin=api.finish_output(pd,'plan.json',plan);output=case.root/'candidate'
        common=(api,*data['common'],pd,pin)
        try:
            receipt=candidate.generate(*common,output,**data['options'])
            if verify:candidate.verify(*common,output,receipt['candidate_manifest_sha256'],**data['options'])
        except api.EvidenceError as error:
            if str(error) in ('NATIVE_NVS_PAUSE_NOT_PRESERVED','PLAN_DIFFERS_FROM_EVIDENCE'):
                self.fail('valid NVS candidate was not preserved/reverified: '+str(error))
            raise
        return output,receipt,common

    def test_false_overlay_is_not_treated_as_missing(self):
        data=self.make(nvs_pause=False,snapshot_pause=True,legacy=True);plan=self.plan(data)
        op=plan['proposed_operations'][0]
        self.assertTrue(op['nvs_session_context']['overlay_present']);self.assertIs(op['nvs_session_context']['paused'],False)
        self.assertEqual(op['checkpoint_candidate']['paused'],0)

    def test_plan_seven_modes_and_nvs_precedence(self):
        pauses={mode:mode%2==0 for mode in range(7)}
        data=self.make(modes=range(7),nvs_pause=pauses,snapshot_pause={m:not v for m,v in pauses.items()},legacy={m:not v for m,v in pauses.items()})
        plan=self.plan(data);self.assertEqual(len(plan['proposed_operations']),7)
        for op in plan['proposed_operations']:
            observed=op['nvs_session_context'];mode=observed['mode']
            self.assertIs(observed['paused'],pauses[mode]);self.assertEqual(op['checkpoint_candidate']['paused'],int(pauses[mode]))
            self.assertEqual(observed['pause_source'],'nvs')
        self.assertNotIn('word_nvs_pause_not_exported',plan['blocking_preconditions'])
        self.assertIn(adapter.REVALIDATION,plan['blocking_preconditions'])
        self.assertFalse(plan['installation_ready']);self.assertFalse(plan['repair_authorized'])

    def test_genuine_absence_legacy_and_stale_sid_policies(self):
        stale=self.cursor('22222222-2222-4333-8444-555555555555',True)
        for state,legacy,expected in [('absent',True,True),('absent',False,False),('absent',None,None),('stale',True,None),('absent',stale,None)]:
            with self.subTest(state=state,legacy=legacy):
                data=self.make(state=state,legacy=legacy,snapshot_pause=True);op=self.plan(data)['proposed_operations'][0]
                observed=op['nvs_session_context'];self.assertIs(observed['overlay_present'],expected is not None)
                self.assertIs(observed['paused'],expected)
                if expected is None:self.assertEqual(op['checkpoint_candidate']['paused'],1)

    def test_unresolved_media_and_scope_hold_operations(self):
        for options in [dict(state='wrong-type',legacy=True),dict(state='crc'),dict(state='short'),dict(generation=0x100000007),
                        dict(generation='mismatch'),dict(marker=True),dict(state='absent',legacy=b'unknown')]:
            with self.subTest(options=options):
                data=self.make(**options);plan=self.plan(data)
                self.assertEqual(plan['proposed_operations'],[])
                self.assertEqual(plan['local_checkpoint_checks'][0]['state'],'held')
                self.assertEqual(raw.file_inventory(data['source']),data['source_before'])

    def test_forged_derived_context_needs_original_replay(self):
        data=self.make();body=raw.load(data['context']/'context.json')
        body['values']['cur_seq']['cursor']['paused']=False
        (data['context']/'context.json').write_bytes(api.encoded(body))
        manifest=raw.load(data['context']/'manifest.json');info=manifest['artifacts']['context.json']
        info.update(sha256=raw.hash_file(data['context']/'context.json'),bytes=(data['context']/'context.json').stat().st_size)
        (data['context']/'manifest.json').write_bytes(api.encoded(manifest))
        data['spec']['context_manifest_sha256']=raw.hash_file(data['context']/'manifest.json')
        data['descriptor'].write_bytes(api.encoded(data['spec']));data['options']['nvs_inputs_pin']=raw.hash_file(data['descriptor'])
        with self.assertRaisesRegex(api.EvidenceError,'NVS_CONTEXT_VERIFICATION_FAILED'):self.plan(data)

    def test_wrong_forensic_source_cannot_share_context(self):
        a=self.make();b=self.make(snapshot_pause=True)
        self.assertNotEqual(a['common'][1],b['common'][1])
        with self.assertRaisesRegex(api.EvidenceError,'NVS_FORENSIC_SOURCE_MISMATCH'):
            api.review_plan(*b['common'],**a['options'])

    def test_consent_pins_and_note_compatibility(self):
        data=self.make()
        with self.assertRaisesRegex(api.EvidenceError,'NVS_SENSITIVE_SCOPE_REQUIRED'):self.plan(data,sensitive_confirmed=False)
        with self.assertRaisesRegex(api.EvidenceError,'NVS_CONTEXT_VERIFICATION_FAILED'):self.plan(data,nvs_inputs_pin='44'*32)
        with self.assertRaisesRegex(api.EvidenceError,'NVS_INPUTS_PAIR_REQUIRED'):self.plan(data,nvs_inputs_pin=None)
        with patch.object(ctx,'verify',side_effect=raw.native.NativeExecutionError('synthetic native failure')):
            with self.assertRaisesRegex(api.EvidenceError,'NVS_NATIVE_EXECUTION_FAILED'):self.plan(data)
        legacy=api.review_plan(*data['common'])
        self.assertEqual(legacy['format'],api.PLAN_FORMAT);self.assertNotIn('nvs_context',legacy)
        case=fixtures.ReconcileTests();self.addCleanup(case.doCleanups);case.source('note');evidence,pin,_=case.collect()
        self.assertEqual(api.review_plan(case.bundle,case.pin,evidence,pin)['format'],api.PLAN_FORMAT)
        with self.assertRaisesRegex(api.EvidenceError,'NVS_CONTEXT_WORD_ONLY'):
            api.review_plan(case.bundle,case.pin,evidence,pin,**data['options'])

    def test_output_cannot_enter_any_nvs_input_directory(self):
        data=self.make()
        for key in adapter.DIRECTORIES:
            with self.subTest(key=key),self.assertRaisesRegex(api.EvidenceError,'NVS_OUTPUT_INSIDE_INPUT'):
                self.plan(data,output=Path(data['spec'][key])/'new-output')

    def test_native_all_modes_preserve_true_and_false(self):
        pauses={m:m%2==0 for m in range(7)}
        data=self.make(modes=range(7),nvs_pause=pauses,snapshot_pause={m:not v for m,v in pauses.items()})
        output,_,_=self.native(data)
        proof=raw.load(output/'proof.json')
        self.assertEqual(len(proof['sessions']),7)
        for item in proof['sessions']:
            state=checkpoint.decode_session((output/item['candidate_session']).read_bytes(),'word')['value']
            self.assertEqual(state['paused'],int(pauses[state['mode']]))
        manifest=raw.load(output/'manifest.json');self.assertFalse(manifest['repair_authorized']);self.assertFalse(manifest['installation_ready'])

    def test_cli_all_four_operations_keep_false(self):
        data=self.make(nvs_pause=False,snapshot_pause=True);case=data['case'];base=[sys.executable,str(ROOT/'scripts/outbox_reconcile.py')]
        args=['--source-bundle',str(data['common'][0]),'--source-manifest-sha256',data['common'][1],
            '--evidence',str(data['common'][2]),'--evidence-manifest-sha256',data['common'][3],
            '--nvs-context-inputs',str(data['descriptor']),'--nvs-context-inputs-sha256',data['options']['nvs_inputs_pin'],'--sensitive-main-flash']
        def run(command,extra):
            done=subprocess.run([*base,command,*args,*extra],capture_output=True,text=True,timeout=90)
            if done.returncode!=0:raise RuntimeError('CLI did not complete: '+done.stderr)
            return json.loads(done.stdout)
        pd=case.root/'cli-plan';plan=run('plan',['--output',str(pd)])
        pargs=['--plan',str(pd),'--plan-sha256',plan['plan_sha256']]
        self.assertEqual(run('verify-plan',pargs)['status'],'review_plan_verified')
        output=case.root/'cli-candidate';made=run('candidate',[*pargs,'--output',str(output)])
        self.assertEqual(run('verify-candidate',[*pargs,'--candidate',str(output),'--candidate-manifest-sha256',made['candidate_manifest_sha256']])['status'],'native_candidate_verified')
        state=checkpoint.decode_session((output/'checkpoint-00.v1').read_bytes(),'word')['value'];self.assertEqual(state['paused'],0)

    def test_native_pause_overlay_follows_terminal_replay(self):
        data=self.make(modes=(0,5),terminal=True,nvs_pause=True,snapshot_pause=False)
        legacy_plan=api.review_plan(*data['common'])
        for op in legacy_plan['proposed_operations']:
            self.assertEqual(op['checkpoint_candidate']['active'],0)
            self.assertEqual(op['checkpoint_candidate']['paused'],0)
        plan=self.plan(data)
        self.assertEqual({op['nvs_session_context']['mode'] for op in plan['proposed_operations']},{0,5})
        for op in plan['proposed_operations']:
            self.assertEqual(op['checkpoint_candidate']['active'],0)
            self.assertEqual(op['checkpoint_candidate']['paused'],1)
        output,_,_=self.native(data)
        for item in raw.load(output/'proof.json')['sessions']:
            state=checkpoint.decode_session((output/item['candidate_session']).read_bytes(),'word')['value']
            self.assertEqual(state['active'],0);self.assertEqual(state['paused'],1)

    def test_candidate_cannot_drop_context_or_trust_modified_bytes(self):
        data=self.make();output,receipt,common=self.native(data)
        with self.assertRaisesRegex(api.EvidenceError,'PLAN_DIFFERS_FROM_EVIDENCE'):
            candidate.verify(*common,output,receipt['candidate_manifest_sha256'])
        path=output/'checkpoint-00.v1';body=bytearray(path.read_bytes());body[17]=0
        struct.pack_into('<I',body,12,raw.forensic.journal_crc(body[16:]));path.write_bytes(body)
        manifest=raw.load(output/'manifest.json');manifest['files'][path.name]['sha256']=raw.hash_file(path)
        (output/'manifest.json').write_bytes(api.encoded(manifest))
        with self.assertRaisesRegex(api.EvidenceError,'NATIVE_CANDIDATE_DIFFERS_FROM_RECOMPUTED_BYTES'):
            candidate.verify(*common,output,raw.hash_file(output/'manifest.json'),**data['options'])

    def test_changed_context_during_native_execution_stays_incomplete(self):
        data=self.make();case=data['case'];plan=self.plan(data);pd=case.root/'plan';api.start_output(pd)
        pin=api.finish_output(pd,'plan.json',plan);output=case.root/'candidate';original=candidate.Driver.run
        def changed(driver,*args,**kwargs):
            result=original(driver,*args,**kwargs);(data['context']/'context.json').write_bytes(b'changed context');return result
        with patch.object(candidate.Driver,'run',changed),self.assertRaisesRegex(api.EvidenceError,'NVS_CONTEXT_VERIFICATION_FAILED'):
            candidate.generate(api,*data['common'],pd,pin,output,**data['options'])
        self.assertTrue((output/'.incomplete').exists());self.assertFalse((output/'manifest.json').exists())


FACTS=[name for name in NvsPlanTests.__dict__ if name.startswith('test_') and name not in (
    'test_native_all_modes_preserve_true_and_false','test_cli_all_four_operations_keep_false',
    'test_candidate_cannot_drop_context_or_trust_modified_bytes','test_changed_context_during_native_execution_stays_incomplete',
    'test_native_pause_overlay_follows_terminal_replay')]
CANDIDATE=[name for name in NvsPlanTests.__dict__ if name.startswith('test_') and name not in FACTS]


class SemanticResult(unittest.TextTestResult):
    def printErrors(self):
        self.printErrorList('ERROR',self.errors)
        for case,error in self.failures:
            self.stream.writeln('FAIL nvs-plan: '+self.getDescription(case))
            self.stream.writeln(error.strip().splitlines()[-1][:200])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--part',choices=('all','facts','candidate'),default='all')
    parser.add_argument('--mutation',choices=('skip-replay','ignore-source-binding','drop-false','drop-native-overlay','drop-verify-context','ignore-output-boundary','pause-before-replay'))
    args=parser.parse_args();names=FACTS+CANDIDATE if args.part=='all' else FACTS if args.part=='facts' else CANDIDATE
    if args.mutation:
        if args.mutation in ('drop-native-overlay','pause-before-replay'):
            original=candidate.native_template
            def mutant():
                text=original();old="expected.paused = result.paused = argv[6][0] == '1';"
                if text.count(old)!=1:raise RuntimeError('native pause mutation drift')
                if args.mutation=='drop-native-overlay':return text.replace(old,'(void)argv;')
                text=text.replace(old,'(void)argv;')
                needle='    Replay(source, &expected);'
                if text.count(needle)!=1:raise RuntimeError('native pause order mutation drift')
                return text.replace(needle,"    if (argc == 7) { expected.paused = result.paused = argv[6][0] == '1'; }\n"+needle)
            candidate.native_template=mutant
        else:
            module=candidate if args.mutation=='drop-verify-context' else adapter
            source=Path(module.__file__).read_text()
            old,new={
                'skip-replay':("context.verify(*values, expected_manifest_sha256=spec['context_manifest_sha256'], sensitive_confirmed=True)",'None'),
                'ignore-source-binding':("if facts['binding']['forensics'].get('word') != self.source_pin:",'if False:'),
                'drop-false':("overlay_present=True, paused=cursor['paused'], pause_source='nvs'","overlay_present=bool(cursor['paused']), paused=cursor['paused'], pause_source='nvs'"),
                'drop-verify-context':('generate(api, source, source_pin, evidence, evidence_pin, plan_dir, plan_pin, rebuilt,\n            nvs_inputs=nvs_inputs, nvs_inputs_pin=nvs_inputs_pin, sensitive_confirmed=sensitive_confirmed)',
                    'generate(api, source, source_pin, evidence, evidence_pin, plan_dir, plan_pin, rebuilt)'),
                'ignore-output-boundary':('if output is not None and any(', 'if False and any('),
            }[args.mutation]
            if source.count(old)!=1:raise RuntimeError('NVS plan mutation drift')
            replacement=types.ModuleType(module.__name__);replacement.__file__=module.__file__
            exec(compile(source.replace(old,new),module.__file__,'exec'),replacement.__dict__)
            sys.modules[module.__name__]=replacement
            if module is candidate:globals()['candidate']=replacement
            else:globals()['adapter']=replacement
        names=[{'skip-replay':'test_forged_derived_context_needs_original_replay',
            'ignore-source-binding':'test_wrong_forensic_source_cannot_share_context',
            'drop-false':'test_false_overlay_is_not_treated_as_missing',
            'drop-native-overlay':'test_native_all_modes_preserve_true_and_false',
            'drop-verify-context':'test_native_all_modes_preserve_true_and_false',
            'ignore-output-boundary':'test_output_cannot_enter_any_nvs_input_directory',
            'pause-before-replay':'test_native_pause_overlay_follows_terminal_replay'}[args.mutation]]
    started=time.monotonic();suite=unittest.TestSuite(NvsPlanTests(name) for name in names)
    result=unittest.TextTestRunner(verbosity=2,resultclass=SemanticResult if args.mutation else unittest.TextTestResult).run(suite)
    print(f'NVS plan context: {result.testsRun} scenarios, {len(result.failures)} failures, {len(result.errors)} errors (actual SDK/raw/native; NOT HIL)',flush=True)
    print(f'Elapsed seconds: {time.monotonic()-started:.3f}',flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':raise SystemExit(main())
