#!/usr/bin/env python3
"""Native candidate production replay + raw evidence tests. NOT HIL."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import types
import unittest
from unittest.mock import patch

import outbox_candidate as baseline
import outbox_checkpoint as checkpoint
import outbox_forensics as f
import outbox_reconcile as api
import test_outbox_reconcile as fixtures

ROOT = Path(__file__).resolve().parents[1]
M = baseline


class CandidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixtures.M = api
        fixtures.ReconcileTests.setUpClass()
        cls.native = fixtures.ReconcileTests.native

    @classmethod
    def tearDownClass(cls):
        fixtures.ReconcileTests.tearDownClass()

    def context(self, domain='word', names=None):
        case = fixtures.ReconcileTests(); self.addCleanup(case.doCleanups)
        case.source(domain=domain, names=names)
        evidence, pin, _ = case.collect()
        plan = api.review_plan(case.bundle, case.pin, evidence, pin)
        plan_dir = case.root/'review'; api.start_output(plan_dir)
        plan_pin = api.finish_output(plan_dir, 'plan.json', plan)
        self.case = case
        self.common = (api, case.bundle, case.pin, evidence, pin, plan_dir, plan_pin)
        self.output = case.root/'candidate'
        return plan

    def generate(self):
        result = M.generate(*self.common, self.output)
        self.pin = result['candidate_manifest_sha256']
        return result

    def verify(self, pin=None):
        return M.verify(*self.common, self.output, pin or self.pin)

    def test_word_candidate_preserves_full_raw_prefix_and_suspend(self):
        plan = self.context()
        before = {p.name:p.read_bytes() for p in self.case.bundle.iterdir()}
        self.generate()
        raw = (self.output/'journal.v1').read_bytes()
        source = (self.case.bundle/'wout.v1').read_bytes()
        self.assertEqual(raw[:len(source)],source)
        self.assertEqual(len(raw),len(source)+200)
        report = f.analyze(self.output/'journal.v1','word')
        self.assertEqual(report['native_error_code'],0)
        self.assertEqual(report['prefix_counts']['suspended'],1)
        self.assertEqual(report['prefix_counts']['pending'],0)
        self.assertEqual(report['prefix_counts']['acknowledged'],2)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.case.bundle.iterdir()})
        state = checkpoint.decode_session((self.output/'checkpoint-00.v1').read_bytes(),'word')['value']
        self.assertEqual(state['next_sequence'],10);self.assertEqual(state['position'],1)
        self.assertEqual(plan['proposed_operations'][0]['expected_local_checkpoint']['next_phase'],state['phase'])
        manifest=json.loads((self.output/'manifest.json').read_text())
        self.assertFalse(manifest['installation_ready']);self.assertFalse(manifest['repair_authorized'])
        self.assertIn('word_nvs_pause_not_exported',manifest['blocking_preconditions'])
        self.assertIn('target_firmware_native_build_not_attested',manifest['blocking_preconditions'])
        for p in self.output.iterdir(): self.assertEqual(p.stat().st_mode&0o777,0o600)
        self.assertEqual(self.output.stat().st_mode&0o777,0o700)

    def test_note_replays_all_partitions_and_keeps_latest_pin(self):
        raw=bytearray((self.native/'note/healthy.bin').read_bytes());size=f.LAYOUTS['note'].record.size
        for index,second in [(0,1),(2,2),(4,3)]:
            at=index*size;stamp=f'2026-11-05T00:00:0{second}Z'.encode()
            raw[at+169:at+202]=stamp+b'\0'*(33-len(stamp))
            struct.pack_into('<I',raw,at+size-4,f.journal_crc(raw[at:at+size-4]))
        self.context('note',{'nout.v1':bytes(raw),'nses.v1':'checkpoint-session.bin'})
        self.generate()
        state=checkpoint.decode_session((self.output/'checkpoint-00.v1').read_bytes(),'note')['value']
        self.assertEqual(state['items'][0]['last_opened_at'],'2026-11-05T00:00:03Z')
        self.assertEqual(state['next_sequence'],10)
        self.assertEqual(self.verify()['status'],'native_candidate_verified')

    def test_every_word_mode_uses_the_same_complete_candidate_journal(self):
        session=bytearray((self.native/'word/checkpoint-session.bin').read_bytes())
        other_sid='11111111-1111-1111-1111-111111111112'
        original_sid=b'11111111-1111-1111-1111-111111111111'
        at=session.index(original_sid);session[at:at+36]=other_sid.encode();session[16+3]=1
        struct.pack_into('<I',session,12,f.journal_crc(session[16:]))
        journal=bytearray((self.native/'word/healthy.bin').read_bytes());size=200
        for offset in range(0,len(journal),size):
            journal[offset+8]=1
            journal[offset+24]=ord('a')
            if journal[offset+6]==1: journal[offset+89:offset+125]=other_sid.encode()
            struct.pack_into('<I',journal,offset+196,f.journal_crc(journal[offset:offset+196]))
        self.context(names={'wout.v1':(self.native/'word/healthy.bin').read_bytes()+journal,
            'wsq.v1':'checkpoint-session.bin','wsr.v1':bytes(session)})
        self.generate()
        proof=json.loads((self.output/'proof.json').read_text())
        self.assertEqual(len(proof['sessions']),2)
        self.assertEqual(len((self.output/'journal.v1').read_bytes()),2400)
        self.assertEqual(sum(s['native']['matching_proposals'] for s in proof['sessions']),2)
        self.assertEqual(self.verify()['status'],'native_candidate_verified')

    def test_missing_checkpoint_has_no_native_candidate(self):
        self.context(names={'wout.v1':'healthy.bin'})
        with self.assertRaisesRegex(api.EvidenceError,'NATIVE_NO_ACK_PROPOSALS'):self.generate()
        self.assertFalse(self.output.exists())

    def test_unmodeled_session_bytes_cannot_be_normalized_away(self):
        session=bytearray((self.native/'word/checkpoint-session.bin').read_bytes());session[6]=1
        self.context(names={'wout.v1':'healthy.bin','wsq.v1':bytes(session)})
        with self.assertRaisesRegex(api.EvidenceError,'NATIVE_SESSION_ROUNDTRIP_NOT_EXACT'):self.generate()
        self.assertTrue((self.output/'.incomplete').exists())
        self.assertFalse((self.output/'manifest.json').exists())

    def test_forged_approval_and_new_hashes_do_not_bypass_recomputation(self):
        self.context();self.generate()
        path=self.output/'journal.v1';raw=bytearray(path.read_bytes());raw[-1]^=1;path.write_bytes(raw)
        manifest=json.loads((self.output/'manifest.json').read_text())
        manifest.update(installation_ready=True,repair_authorized=True,blocking_preconditions=[])
        manifest['files']['journal.v1']['sha256']=f.sha256(raw)
        saved=api.encoded(manifest);(self.output/'manifest.json').write_bytes(saved)
        with self.assertRaisesRegex(api.EvidenceError,'NATIVE_CANDIDATE_DIFFERS_FROM_RECOMPUTED_BYTES'):
            self.verify(f.sha256(saved))

    def test_changed_plan_cannot_rebind_the_candidate(self):
        self.context()
        saved=json.loads((self.common[5]/'plan.json').read_text());saved['proposed_operations']=[]
        raw=api.encoded(saved);(self.common[5]/'plan.json').write_bytes(raw)
        self.common=(*self.common[:-1],f.sha256(raw))
        with self.assertRaisesRegex(api.EvidenceError,'PLAN_DIFFERS_FROM_EVIDENCE'):self.generate()

    def test_unlisted_candidate_files_are_rejected(self):
        self.context();self.generate();(self.output/'extra').write_bytes(b'unknown')
        with self.assertRaisesRegex(api.EvidenceError,'NATIVE_CANDIDATE_FILE_INVENTORY_MISMATCH'):self.verify()

    def test_native_failure_preserves_incomplete_state(self):
        self.context()
        with patch.object(M,'Driver',side_effect=M.NativeError('NATIVE_BUILD_FAILED',b'synthetic compiler diagnosis')):
            with self.assertRaisesRegex(api.EvidenceError,'NATIVE_BUILD_FAILED'):self.generate()
        self.assertTrue((self.output/'.incomplete').exists())
        saved=json.loads((self.output/'native-error.json').read_text())
        self.assertEqual(saved['reason'],'NATIVE_BUILD_FAILED')
        self.assertEqual(saved['diagnostic'],'synthetic compiler diagnosis')
        self.assertFalse((self.output/'manifest.json').exists())

    def test_source_pin_is_checked_after_native_execution(self):
        self.context();run=M.Driver.run
        def changed(driver,*args):
            result=run(driver,*args)
            (self.case.bundle/'wout.v1').write_bytes(b'changed after native copy')
            return result
        with patch.object(M.Driver,'run',changed):
            with self.assertRaises(api.EvidenceError):self.generate()
        self.assertTrue((self.output/'.incomplete').exists())

    def test_cli_native_generation_and_offline_verification(self):
        self.context()
        common=['--source-bundle',str(self.common[1]),'--source-manifest-sha256',self.common[2],
            '--evidence',str(self.common[3]),'--evidence-manifest-sha256',self.common[4],
            '--plan',str(self.common[5]),'--plan-sha256',self.common[6]]
        base=[sys.executable,str(ROOT/'scripts/outbox_reconcile.py')]
        run=subprocess.run(base+['candidate',*common,'--output',str(self.output)],capture_output=True,text=True,timeout=90)
        self.assertEqual(run.returncode,0,run.stderr)
        pin=json.loads(run.stdout)['candidate_manifest_sha256']
        run=subprocess.run(base+['verify-candidate',*common,'--candidate',str(self.output),
            '--candidate-manifest-sha256',pin],capture_output=True,text=True,timeout=90)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertFalse(json.loads(run.stdout)['installation_ready'])


class SemanticResult(unittest.TextTestResult):
    def printErrors(self):
        self.printErrorList('ERROR',self.errors)
        for case,error in self.failures:
            self.stream.writeln('FAIL native-candidate: '+self.getDescription(case))
            self.stream.writeln(error.strip().splitlines()[-1][:200])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('trust-candidate','normalize-unknown-session'))
    args=parser.parse_args()
    global M
    if args.mutation:
        body=(ROOT/'scripts/outbox_candidate.py').read_text()
        if args.mutation=='trust-candidate':
            old='if actual != expected:'
            if body.count(old)!=1:raise ValueError('native verifier mutation boundary drift')
            body=body.replace(old,'if False:')
        M=types.ModuleType('native_candidate_under_test');M.__file__=str(ROOT/'scripts/outbox_candidate.py')
        exec(compile(body,M.__file__,'exec'),M.__dict__)
        if args.mutation=='normalize-unknown-session':
            template=M.native_template()
            old='SessionBytes(original) != original_session'
            if template.count(old)!=1:raise ValueError('native roundtrip mutation boundary drift')
            M.native_template=lambda:template.replace(old,'false')
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(CandidateTests)
    result=unittest.TextTestRunner(verbosity=2,resultclass=SemanticResult if args.mutation else unittest.TextTestResult).run(suite)
    print(f'Native outbox candidate: {result.testsRun} scenarios, {len(result.failures)} failures, {len(result.errors)} errors (NOT HIL)',flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':raise SystemExit(main())
