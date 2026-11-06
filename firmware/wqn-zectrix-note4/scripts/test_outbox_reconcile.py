#!/usr/bin/env python3
"""R-08 evidence/plan tests using native records and local HTTP fault servers. NOT HIL."""
import argparse
import base64
import copy
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

import outbox_forensics as f
import test_outbox_forensics as native
from test_outbox_checkpoint import build_oracle as build_checkpoint_oracle

ROOT = Path(__file__).resolve().parents[1]
M = None
TOKEN = 'c'*64
ACTOR = {'user_id': '00000000-0000-0000-0000-000000000001',
         'device_id': '22222222-2222-4222-8222-222222222222'}


class ReconcileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        native.ForensicsTests.setUpClass()
        cls.native = native.ForensicsTests.root
        (cls.native/'esp_err.h').write_text(native.ERR+'\nconstexpr int ESP_ERR_INVALID_VERSION=266;\n')
        for domain in ('word', 'note'):
            build_checkpoint_oracle(domain, cls.native/domain, cls.native/'sdk-crc.o')
        cls.wire = {}
        pieces = ['#include <cstdio>', '#include "device_protocol/word_study.h"', '#include "device_protocol/note_study.h"']
        for domain in ('word', 'note'):
            body = (ROOT/f'components/device_protocol/{domain}_study.cpp').read_text()
            namespace = f'wqn::protocol::{domain}_study_v1'
            pieces += [f'namespace {namespace} {{', native.function(body, 'const char* ModeName('),
                       native.function(body, 'const char* ObservationActionName('), '}']
        pieces.append('int main(){')
        for domain, modes, actions in [('word','kMistakes','kLookedUp'),('note','kRecent','kSessionPaused')]:
            ns=f'wqn::protocol::{domain}_study_v1'
            pieces.append(f'for(int i=0;i<=static_cast<int>({ns}::Mode::{modes});++i)std::printf("{domain} mode %s\\n",{ns}::ModeName(static_cast<{ns}::Mode>(i)));')
            pieces.append(f'for(int i=0;i<=static_cast<int>({ns}::ObservationAction::{actions});++i)std::printf("{domain} action %s\\n",{ns}::ObservationActionName(static_cast<{ns}::ObservationAction>(i)));')
        pieces.append('}')
        unit=cls.native/'wire.cpp';unit.write_text('\n'.join(pieces));binary=cls.native/'wire'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"','-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"',
            '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"','-I'+str(cls.native),
            '-I'+str(ROOT/'components/device_protocol/include'),str(unit),'-o',str(binary)],check=True,timeout=30)
        for line in subprocess.check_output([str(binary)],text=True).splitlines():
            domain,kind,value=line.split();cls.wire.setdefault((domain,kind),[]).append(value)

    @classmethod
    def tearDownClass(cls):
        native.ForensicsTests.tearDownClass()

    def source(self,domain='word',vector='healthy.bin',names=None):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        root=Path(temp.name);source=root/'export';source.mkdir()
        prefix=f.LAYOUTS[domain].prefix
        for name, value in (names or {prefix+'.v1':vector}).items():
            data=value if isinstance(value,bytes) else (self.native/domain/value).read_bytes()
            (source/name).write_bytes(data)
        if names is None:
            (source/('wsq.v1' if domain=='word' else 'nses.v1')).write_bytes(
                (self.native/domain/'checkpoint-session.bin').read_bytes())
        bundle=root/'source'; pin=f.capture(source,bundle,domain,True)['manifest_sha256']
        credential=root/'device-key';f.write_private(credential,TOKEN.encode())
        self.domain=domain;self.bundle=bundle;self.pin=pin;self.root=root;self.credential=credential
        self.facts={}
        for name in source.iterdir():
            raw=name.read_bytes();size=f.LAYOUTS[domain].record.size
            for at in range(0,len(raw),size):
                row,_,_=f.decode_record(raw[at:at+size],domain,at)
                if row.get('kind')=='observation' and row.get('identity_usable_for_lookup'):
                    self.facts.setdefault((row['session_id'].lower(),row['request_id']),row)
        return root

    def reply(self,request):
        entries=[]
        mode='sequential'
        for key in request['request_ids']:
            row=self.facts[(request['session_id'],key)]
            mode=self.wire[(self.domain,'mode')][row['mode']]
            entries.append({'request_id':key,'state':'found','observation':{
                'observation_id':'44444444-4444-4444-8444-444444444444',
                'request_id':key,'session_id':request['session_id'],'sequence':row['sequence'],
                'item_id':row['item_id'],'mode':mode,
                'action':self.wire[(self.domain,'action')][row['action']],
                'occurred_at':row['occurred_at'],'created_at':'2026-11-06T00:00:00Z'}})
        return {'ok':True,'request_id':request['request_id'],'server_time_ms':1793923200000,
            'data':{'contract':M.CONTRACT,'read_at':'2026-11-06T00:00:00Z','actor':ACTOR.copy(),
                'session':{'session_id':request['session_id'],'domain':self.domain,'mode':mode,
                    'status':'abandoned','next_sequence':900,'expires_at':'2026-10-27T00:00:00Z'},
                'receipts':entries,'repair_authorized':False}}

    def fake(self,endpoint,token,body,allow,timeout):
        self.assertEqual(token,TOKEN)
        reply=self.reply(json.loads(body))
        return {'http_status':200,'raw':base64.b64encode(M.encoded(reply)).decode(),'error':None}

    def collect(self,transport=None,output='evidence',**options):
        dest=self.root/output
        result=M.collect(self.bundle,self.pin,dest,'https://wqn.example'+M.ENDPOINT_PATH,
            self.credential,ACTOR,transport=transport or self.fake,**options)
        return dest,result['manifest_sha256'],result

    def rewrite_evidence(self,evidence,change):
        manifest=json.loads((evidence/'manifest.json').read_text())
        change(manifest)
        for name in manifest['files']:
            raw=(evidence/name).read_bytes();manifest['files'][name]={'bytes':len(raw),'sha256':f.sha256(raw)}
        raw=M.encoded(manifest);(evidence/'manifest.json').write_bytes(raw)
        return f.sha256(raw)

    def server(self,respond):
        case=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                body=self.rfile.read(int(self.headers['Content-Length']))
                server.seen.append((dict(self.headers),body))
                code,headers,payload=respond(json.loads(body))
                self.send_response(code)
                for key,value in headers.items():self.send_header(key,value)
                self.send_header('Content-Length',str(len(payload)));self.end_headers()
                try:self.wfile.write(payload)
                except (BrokenPipeError,ConnectionResetError):pass
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;server.seen=[]
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        self.addCleanup(lambda:(server.shutdown(),server.server_close(),thread.join(timeout=2)))
        return server,'http://127.0.0.1:'+str(server.server_port)+M.ENDPOINT_PATH

    def test_native_wire_enum_names_match_the_client(self):
        for domain in ('word','note'):
            self.assertEqual(tuple(self.wire[(domain,'mode')]),M.MODES[domain])
            self.assertEqual(tuple(self.wire[(domain,'action')]),M.ACTIONS[domain])

    def test_real_loopback_transport_binds_and_preserves_source(self):
        self.source();before={p.name:p.read_bytes() for p in self.bundle.iterdir()}
        server,endpoint=self.server(lambda req:(200,{},M.encoded(self.reply(req))))
        dest=self.root/'wire-evidence'
        result=M.collect(self.bundle,self.pin,dest,endpoint,self.credential,ACTOR,True)
        self.assertTrue(result['query_complete']);self.assertEqual(result['authenticated_batches'],1)
        self.assertEqual(len(server.seen),1)
        self.assertEqual(server.seen[0][0]['Authorization'],'Bearer '+TOKEN)
        self.assertEqual({p.name:p.read_bytes() for p in self.bundle.iterdir()},before)
        M.verify(self.bundle,self.pin,dest,result['manifest_sha256'])
        self.assertEqual(dest.stat().st_mode&0o777,0o700)
        for path in dest.iterdir():
            self.assertEqual(path.stat().st_mode&0o777,0o600)
            self.assertNotIn(TOKEN.encode(),path.read_bytes())
        self.assertIn('loopback_test_evidence_only',M.review_plan(self.bundle,self.pin,dest,result['manifest_sha256'])['blocking_preconditions'])

    def test_full_round_is_not_limited_to_manifest_preview(self):
        size=f.LAYOUTS['word'].record.size
        self.source(names={'wout.v1':(self.native/'word/at-capacity.bin').read_bytes()[:7014*size]})
        manifest=json.loads((self.bundle/'manifest.json').read_text())
        self.assertEqual(len(manifest['files']['wout.v1']['analysis']['records']),256)
        evidence,pin,result=self.collect()
        self.assertEqual(result['batches'],110)
        _,rows,assessment,_=M.verify(self.bundle,self.pin,evidence,pin)
        self.assertEqual(len(rows),7014)
        self.assertEqual(sum(r['state']=='matched' for r in assessment['records']),7014)

    def test_matching_fact_does_not_unpark_or_replace_checkpoint(self):
        self.source();evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertEqual(len(plan['proposed_operations']),1)
        op=plan['proposed_operations'][0]
        self.assertEqual(op['request_id'],'request-three-003')
        self.assertEqual(op['expected_local_checkpoint'],{'next_position':17,'next_phase':1})
        self.assertTrue(any(d['action']=='preserve_suspended' for d in plan['decisions']))
        self.assertTrue(any(d['action']=='preserve_acknowledged' for d in plan['decisions']))
        self.assertFalse(plan['installation_ready']);self.assertFalse(plan['repair_authorized'])
        self.assertEqual(op['checkpoint_candidate']['position'],1)
        self.assertEqual(op['checkpoint_session_source']['file'],'wsq.v1')
        self.assertIn('word_nvs_pause_not_exported',plan['blocking_preconditions'])

    def test_note_records_keep_independent_local_next_sequence(self):
        self.source('note');evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertEqual(plan['proposed_operations'][0]['expected_local_checkpoint'],{'next_position':17,'next_sequence':10})

    def test_matched_receipt_without_local_session_cannot_propose_ack(self):
        self.source(names={'wout.v1':'healthy.bin'})
        evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertEqual(plan['proposed_operations'],[])
        self.assertEqual(plan['local_checkpoint_checks'][0]['reason'],'session_material_missing_or_invalid')

    def test_receipt_match_does_not_prove_an_unrepresentable_checkpoint(self):
        raw=bytearray((self.native/'word/healthy.bin').read_bytes());size=f.LAYOUTS['word'].record.size
        struct.pack_into('<I',raw,4*size+20,55)
        struct.pack_into('<I',raw,5*size-4,f.journal_crc(raw[4*size:5*size-4]))
        self.source(names={'wout.v1':bytes(raw),'wsq.v1':'checkpoint-session.bin'})
        evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertEqual(plan['proposed_operations'],[])
        self.assertEqual(plan['local_checkpoint_checks'][0]['reason'],'checkpoint_ordinal_outside_candidate_window')

    def test_descriptor_corruption_blocks_a_matching_pending_fact(self):
        self.source(names={'wout.v1':'healthy.bin','wsq.v1':'checkpoint-local-session.bin',
            'wl'+('1'*24)+'.v1':b'corrupt descriptor'})
        evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertEqual(plan['proposed_operations'],[])
        self.assertEqual(plan['local_checkpoint_checks'][0]['reason'],'descriptor_missing_or_invalid')

    def test_missing_and_conflicting_receipts_never_become_ack_proposals(self):
        for state in ('not_found','conflict'):
            with self.subTest(state=state):
                self.source()
                def missing(endpoint,token,body,allow,timeout):
                    reply=self.reply(json.loads(body))
                    for row in reply['data']['receipts']:row.update(state=state,observation=None)
                    return {'http_status':200,'raw':base64.b64encode(M.encoded(reply)).decode(),'error':None}
                evidence,pin,_=self.collect(missing)
                self.assertEqual(M.review_plan(self.bundle,self.pin,evidence,pin)['proposed_operations'],[])

    def test_canonical_payload_must_match_every_field(self):
        for field,value in [('sequence',99),('item_id','aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'),
                ('action','known'),('occurred_at','2026-11-05T00:00:01Z')]:
            with self.subTest(field=field):
                self.source()
                def mismatch(endpoint,token,body,allow,timeout):
                    reply=self.reply(json.loads(body))
                    for row in reply['data']['receipts']:row['observation'][field]=value
                    return {'http_status':200,'raw':base64.b64encode(M.encoded(reply)).decode(),'error':None}
                evidence,pin,_=self.collect(mismatch)
                assessment=json.loads((evidence/'assessment.json').read_text())
                self.assertTrue(any(row['state']=='payload_mismatch' for row in assessment['records']))
                self.assertEqual(M.review_plan(self.bundle,self.pin,evidence,pin)['proposed_operations'],[])

    def test_wrong_actor_session_request_order_and_repair_claim_are_unavailable(self):
        for kind in ('actor','session','order','request','repair'):
            with self.subTest(kind=kind):
                self.source()
                def wrong(endpoint,token,body,allow,timeout):
                    reply=self.reply(json.loads(body));data=reply['data']
                    if kind=='actor':data['actor']['user_id']='aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
                    if kind=='session':data['session']['session_id']='aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
                    if kind=='order':data['receipts'].reverse()
                    if kind=='request':reply['request_id']='wrong_outer_request_01'
                    if kind=='repair':data['repair_authorized']=True
                    return {'http_status':200,'raw':base64.b64encode(M.encoded(reply)).decode(),'error':None}
                evidence,pin,result=self.collect(wrong)
                self.assertFalse(result['query_complete'])
                self.assertEqual(M.review_plan(self.bundle,self.pin,evidence,pin)['proposed_operations'],[])

    def test_timestamp_offsets_compare_exactly_without_float_loss(self):
        self.assertEqual(M.instant('2026-11-05T08:00:00.123456+08:00'),M.instant('2026-11-05T00:00:00.123456Z'))
        with self.assertRaises(M.EvidenceError):M.instant('2026-11-05T00:00:00.1234567Z')

    def test_same_request_with_different_local_checkpoint_is_held(self):
        raw=bytearray((self.native/'word/healthy.bin').read_bytes());size=f.LAYOUTS['word'].record.size
        at=4*size;struct.pack_into('<I',raw,at+20,55)
        struct.pack_into('<I',raw,at+size-4,f.journal_crc(raw[at:at+size-4]))
        self.source(names={'wout.v1':'healthy.bin','wout.bak':bytes(raw)})
        evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertEqual(plan['proposed_operations'],[])
        self.assertTrue(any(d['action']=='hold_conflicting_local_variants' for d in plan['decisions']))

    def test_empty_primary_and_temporary_do_not_select_older_facts(self):
        for names,selected in [({'wout.v1':b'','wout.bak':'healthy.bin'},'wout.v1'),({'wout.tmp':'healthy.bin'},None)]:
            with self.subTest(selected=selected):
                self.source(names=names);evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
                self.assertEqual(plan['native_source'],selected);self.assertEqual(plan['proposed_operations'],[])

    def test_corruption_and_partial_tail_are_preserved_not_inferred(self):
        for vector in ('bad-ack.bin','bad-suspend.bin','torn.bin','opaque-padding.bin'):
            with self.subTest(vector=vector):
                self.source(vector=vector);evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
                self.assertEqual(plan['proposed_operations'],[])
                self.assertTrue(plan['source_bytes_retained'])

    def test_source_and_evidence_pins_are_mandatory(self):
        self.source();evidence,pin,_=self.collect()
        with self.assertRaises(M.EvidenceError):M.verify(self.bundle,'a'*64,evidence,pin)
        with self.assertRaises(M.EvidenceError):M.verify(self.bundle,self.pin,evidence,'a'*64)
        (self.bundle/'wout.v1').write_bytes(b'changed')
        with self.assertRaises(M.EvidenceError):M.verify(self.bundle,self.pin,evidence,pin)

    def test_forged_cached_assessment_is_recomputed(self):
        self.source()
        def mismatch(endpoint,token,body,allow,timeout):
            reply=self.reply(json.loads(body))
            for row in reply['data']['receipts']:row['observation']['item_id']='aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
            return {'http_status':200,'raw':base64.b64encode(M.encoded(reply)).decode(),'error':None}
        evidence,pin,_=self.collect(mismatch)
        report=json.loads((evidence/'assessment.json').read_text())
        report['records'][-1].update(state='matched',observation_id='44444444-4444-4444-8444-444444444444')
        (evidence/'assessment.json').write_bytes(M.encoded(report))
        pin=self.rewrite_evidence(evidence,lambda manifest:None)
        with self.assertRaises(M.EvidenceError):M.verify(self.bundle,self.pin,evidence,pin)

    def test_query_coverage_cannot_drop_or_rebind_a_request(self):
        self.source();evidence,pin,_=self.collect()
        request=json.loads((evidence/'request-000000.json').read_text());request['request_ids'].pop()
        (evidence/'request-000000.json').write_bytes(M.encoded(request))
        pin=self.rewrite_evidence(evidence,lambda manifest:None)
        with self.assertRaises(M.EvidenceError):M.verify(self.bundle,self.pin,evidence,pin)

    def test_401_and_database_failure_are_gaps_not_absence(self):
        for status in (401,403,404,429,503):
            with self.subTest(status=status):
                self.source()
                def error(endpoint,token,body,allow,timeout):
                    reply={'ok':False,'request_id':json.loads(body)['request_id'],'error':{'code':'UNAVAILABLE','retryable':True}}
                    return {'http_status':status,'raw':base64.b64encode(M.encoded(reply)).decode(),'error':None}
                evidence,pin,result=self.collect(error)
                self.assertFalse(result['query_complete'])
                _,_,assessment,_=M.verify(self.bundle,self.pin,evidence,pin)
                self.assertFalse(any(r['state']=='not_found' for r in assessment['records']))

    def test_redirect_does_not_forward_device_credential(self):
        self.source();target,url=self.server(lambda req:(200,{},M.encoded(self.reply(req))))
        first,endpoint=self.server(lambda req:(302,{'Location':url},b'{}'))
        result=M.collect(self.bundle,self.pin,self.root/'redirect',endpoint,self.credential,ACTOR,True)
        self.assertFalse(result['query_complete']);self.assertEqual(len(first.seen),1);self.assertEqual(len(target.seen),0)

    def test_worker_deadline_bounds_a_stalled_response(self):
        self.source()
        def stall(req):time.sleep(2);return 200,{},M.encoded(self.reply(req))
        _,endpoint=self.server(stall);started=time.monotonic()
        result=M.collect(self.bundle,self.pin,self.root/'deadline',endpoint,self.credential,ACTOR,True,batch_timeout=0.3)
        self.assertLess(time.monotonic()-started,1.8);self.assertFalse(result['query_complete'])

    def test_cancellation_reaps_the_credential_using_worker(self):
        popen=subprocess.Popen;workers=[]
        def start(*args,**kwargs):
            worker=popen(*args,**kwargs);workers.append(worker)
            communicate=worker.communicate;calls=[]
            def interrupted(*a,**kw):
                calls.append(1)
                if len(calls)==1:raise KeyboardInterrupt()
                return communicate(*a,**kw)
            worker.communicate=interrupted
            return worker
        with patch.object(M.subprocess,'Popen',side_effect=start),self.assertRaises(KeyboardInterrupt):
            M.fetch('https://unused.invalid'+M.ENDPOINT_PATH,TOKEN,b'{}',False,10)
        self.assertIsNotNone(workers[0].poll())

    def test_reflected_credential_and_oversized_response_are_not_saved(self):
        for kind in ('secret','escaped-secret','oversize','duplicate-json'):
            with self.subTest(kind=kind):
                self.source()
                def malicious(req):
                    if kind=='secret':raw=M.encoded({'error':TOKEN})
                    elif kind=='escaped-secret':raw=('{"error":"'+'\\u0063'*64+'"}').encode()
                    elif kind=='oversize':raw=b' '*(M.MAX_RESPONSE+1)
                    else:raw=b'{"ok":true,"ok":false}'
                    return 200,{},raw
                _,endpoint=self.server(malicious);output=self.root/'fault'
                result=M.collect(self.bundle,self.pin,output,endpoint,self.credential,ACTOR,True)
                self.assertFalse(result['query_complete'])
                for p in output.iterdir():self.assertNotIn(TOKEN.encode(),p.read_bytes())

    def test_https_endpoint_and_explicit_literal_loopback_policy(self):
        for endpoint in ('http://example.com'+M.ENDPOINT_PATH,'http://localhost'+M.ENDPOINT_PATH,
                'https://user:secret@example.com'+M.ENDPOINT_PATH,'https://example.com'+M.ENDPOINT_PATH+'?token=x'):
            with self.subTest(endpoint=endpoint),self.assertRaises(M.EvidenceError):M.endpoint_url(endpoint,True)

    def test_source_change_keeps_collection_incomplete(self):
        self.source()
        def changed(*args):
            result=self.fake(*args);(self.bundle/'wout.v1').write_bytes(b'changed during query');return result
        with self.assertRaises(M.EvidenceError):self.collect(changed)
        self.assertTrue((self.root/'evidence/.incomplete').exists())

    def test_plan_command_is_offline_and_non_authorizing(self):
        self.source();evidence,pin,_=self.collect()
        with patch.object(M,'fetch',side_effect=AssertionError('plan must stay offline')):
            plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertFalse(plan['repair_authorized']);self.assertFalse(plan['installation_ready'])
        self.assertIn('controlled_publication_not_implemented',plan['blocking_preconditions'])

    def test_cli_plan_pin_rejects_forged_installation_approval(self):
        self.source();evidence,pin,_=self.collect()
        base=[sys.executable,str(ROOT/'scripts/outbox_reconcile.py')]
        args=['--source-bundle',str(self.bundle),'--source-manifest-sha256',self.pin,
              '--evidence',str(evidence),'--evidence-manifest-sha256',pin]
        dest=self.root/'plan'
        run=subprocess.run(base+['plan',*args,'--output',str(dest)],capture_output=True,text=True,timeout=20)
        self.assertEqual(run.returncode,0,run.stderr)
        receipt=json.loads(run.stdout)
        run=subprocess.run(base+['verify-plan',*args,'--plan',str(dest),'--plan-sha256',receipt['plan_sha256']],capture_output=True,text=True,timeout=20)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertFalse(json.loads(run.stdout)['installation_ready'])
        plan=json.loads((dest/'plan.json').read_text());plan['installation_ready']=True;plan['repair_authorized']=True
        raw=M.encoded(plan);(dest/'plan.json').write_bytes(raw)
        run=subprocess.run(base+['verify-plan',*args,'--plan',str(dest),'--plan-sha256',f.sha256(raw)],capture_output=True,text=True,timeout=20)
        self.assertEqual(run.returncode,1);self.assertNotIn(TOKEN,run.stdout+run.stderr)

    def test_failure_stops_following_batches_without_hiding_coverage(self):
        size=f.LAYOUTS['word'].record.size
        self.source(names={'wout.v1':(self.native/'word/at-capacity.bin').read_bytes()[:65*size]})
        calls=[]
        def failure(*args):
            calls.append(1)
            return {'http_status':None,'raw':None,'error':'TRANSPORT_FAILURE'}
        evidence,pin,result=self.collect(failure)
        self.assertEqual(len(calls),1);self.assertEqual(result['batches'],2)
        _,rows,assessment,_=M.verify(self.bundle,self.pin,evidence,pin)
        self.assertEqual(len(rows),65);self.assertFalse(assessment['query_complete'])
        self.assertEqual(assessment['batches'][1]['reason'],'NOT_QUERIED_AFTER_STOP')

    def test_budget_exhausted_primary_does_not_select_an_old_backup(self):
        size=f.LAYOUTS['word'].record.size
        record=(self.native/'word/healthy.bin').read_bytes()[:size]
        self.source(names={'wout.v1':record*(f.MAX_RECORDS+1),'wout.bak':'healthy.bin'})
        evidence,pin,_=self.collect();plan=M.review_plan(self.bundle,self.pin,evidence,pin)
        self.assertIsNone(plan['native_source']);self.assertEqual(plan['proposed_operations'],[])
        self.assertIn('runtime_source_not_fully_analyzed',plan['blocking_preconditions'])

    def test_failed_final_sync_leaves_uncertain_evidence_guard(self):
        self.source();sync=M.f.sync_directory;calls=[]
        def failing(path):
            calls.append(1)
            if len(calls)==2:raise OSError('injected directory sync failure')
            return sync(path)
        with patch.object(M.f,'sync_directory',side_effect=failing),self.assertRaises(f.CaptureCommitUncertain):
            self.collect()
        self.assertTrue((self.root/'evidence/.incomplete').exists())


class SemanticResult(unittest.TextTestResult):
    def printErrors(self):
        self.printErrorList('ERROR',self.errors)
        for case,error in self.failures:
            self.stream.writeln('FAIL reconcile: '+self.getDescription(case))
            self.stream.writeln(error.strip().splitlines()[-1][:200])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('trust-request-id','forget-suspension','trust-assessment'))
    args=parser.parse_args();body=(ROOT/'scripts/outbox_reconcile.py').read_text()
    if args.mutation:
        old,new={
            'trust-request-id':('if differences:', 'if False:'),
            'forget-suspension':("local[key].add('suspended' if row['kind'] == 'suspend' else 'acknowledged')", "local[key].add('pending' if row['kind'] == 'suspend' else 'acknowledged')"),
            'trust-assessment':("if saved != assessment or manifest['query_complete'] is not assessment['query_complete']:\n        fail('ASSESSMENT_DIFFERS_FROM_RAW_EVIDENCE')\n    return original, rows, assessment, manifest", "return original, rows, saved, manifest"),
        }[args.mutation]
        if body.count(old)!=1:raise ValueError('reconcile mutation boundary drift')
        body=body.replace(old,new)
    global M
    M=types.ModuleType('outbox_reconcile_under_test');M.__file__=str(ROOT/'scripts/outbox_reconcile.py')
    sys.modules[M.__name__]=M;exec(compile(body,M.__file__,'exec'),M.__dict__)
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(ReconcileTests)
    result=unittest.TextTestRunner(verbosity=2,resultclass=SemanticResult if args.mutation else unittest.TextTestResult).run(suite)
    print(f'Outbox reconciliation: {result.testsRun} scenarios, {len(result.failures)} failures, {len(result.errors)} errors (NOT HIL)',flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':raise SystemExit(main())
