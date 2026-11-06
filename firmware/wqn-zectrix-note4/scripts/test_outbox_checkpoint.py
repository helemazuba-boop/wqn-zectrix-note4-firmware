#!/usr/bin/env python3
"""Checkpoint codec parity against production C++ + SDK CRC. NOT HIL."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import types
import unittest

import outbox_checkpoint as baseline
import outbox_forensics as f
import test_outbox_forensics as native

ROOT = Path(__file__).resolve().parents[1]
M = baseline
SID = '11111111-1111-1111-1111-111111111111'
TOKEN = 'wl' + SID.replace('-', '')[:24]


def build_oracle(domain, directory, crc_object):
    word = domain == 'word'
    source = (ROOT/'main'/('word_study_store.cpp' if word else 'note_store.cpp')).read_text()
    constants = '\n'.join(re.search(r'constexpr [^;\n]+ '+name+r'\[?[^;\n]*;', source).group(0)
        for name in ('kSessionMagic', 'kSessionSchemaVersion', 'kMaxSessionPayloadBytes', 'kMaxSessionCursorBytes'))
    if not word:
        constants += '\n' + '\n'.join(re.search(r'constexpr [^;\n]+ '+name+r'[^;\n]*;', source).group(0)
            for name in ('kMaxSnapshot', 'kMaxSessionItems'))
    code = '\n'.join(native.function(source, marker) for marker in
        ('uint32_t Crc32(', 'bool CopyFixedText('))
    code += '\n' + native.section(source, 'template <typename T>\nvoid AppendScalar(', '\nesp_err_t AtomicWrite(')
    code += '\n' + native.function(source, 'esp_err_t LoadSessionFile(')
    if word:
        code += '\n' + native.function(source, 'bool SetSessionCursorOrdinal(')
    code += '\n' + native.function(source, 'void ReconcileSession(')
    if not word:
        code += '\n' + native.function(source, 'void ReconcileSessionFromOutbox(')
    extra, declarations = '', ''
    if word:
        local = (ROOT/'main/word_local_session.cpp').read_text()
        header = (ROOT/'main/word_local_session.h').read_text()
        declarations = 'namespace wqn { uint32_t GetDeckScopeGeneration(){return 0;} bool IsWordDeckScopeReady(){return true;}\n'
        declarations += native.function(local, 'bool IsValidLocalSessionUuid(') + '\n'
        declarations += native.function(header, 'struct LocalWordSessionDescriptor {') + ';\n}\n'
        extra += native.section(source, 'constexpr uint32_t kSessionCursorMagic =', '// [word-session-cursor-nvs] NVS keys')
        extra += '\n' + native.function(source, 'struct SessionPaths {') + ';\n'
        extra += '\n' + native.function(source, 'const char* GetSessionCursorNvsKey(')
        extra += '\n' + native.function(source, 'bool GetSessionCursorPaths(')
        declarations += '''
std::vector<uint8_t> g_cursor_bytes;
namespace wqn {
esp_err_t LoadWordSessionCursorNvs(const char*, void* out, size_t size, bool* found) {
    if (g_cursor_bytes.size() != size) return ESP_FAIL;
    std::memcpy(out, g_cursor_bytes.data(), size); *found = true; return ESP_OK;
}
esp_err_t SaveWordSessionCursorNvs(const char*, const void*, size_t) { std::abort(); }
}
'''
        extra += '\n' + native.function(source, 'bool ReadSessionCursorPaused(')
        extra += '\n'.join(re.search(r'constexpr [^;\n]+ '+name+r'[^;\n]*;', source).group(0)
            for name in ('kLocalDescriptorMagic', 'kLocalDescriptorVersion', 'kLocalDescriptorMaxFileBytes'))
        extra += '\n' + native.section(source, 'constexpr size_t kLocalDescriptorHeaderBytes =', '// Basename is')
        for marker in ('bool LocalDescriptorSemanticsValid(', 'esp_err_t EncodeLocalDescriptor('):
            extra += '\n' + native.function(source, marker)
        extra += '\n' + native.section(source, 'struct LocalDescriptorFields {', '// Read one descriptor file')
    records = native.section(source, '#pragma pack(push, 1)\nstruct SessionHeader {',
        '\n// Every public operation below' if word else '\n// Parsed journal cached')
    fixture = (ROOT/'scripts/testdata/outbox_checkpoint_codec.cpp').read_text()
    parts = {'@@HEADER@@': '#include "'+('word_study_store.h' if word else 'note_store.h')+'"',
        '@@CONSTANTS@@': constants, '@@RECORDS@@': records, '@@CODEC@@': code,
        '@@DESCRIPTOR@@': extra, '@@DECLARATIONS@@': declarations, '@@DOMAIN@@': 'Word' if word else 'Note',
        '@@LOAD_SCOPE@@': ', false' if word else '',
        '@@PIN_ID@@': 'std::snprintf(pin.'+('deck_id' if word else 'notebook_id')+', 37, "%s", kScope);',
        '@@ITEM_SCOPE@@': 'std::snprintf(item.'+('deck_id' if word else 'notebook_id')+', 37, "%s", kScope);'}
    for marker, value in parts.items():
        if marker not in fixture:
            raise ValueError('checkpoint native boundary drift')
        fixture = fixture.replace(marker, value)
    unit, binary = directory/'checkpoint-oracle.cpp', directory/'checkpoint-oracle'
    unit.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DWORD='+str(int(word)),
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
        '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"', '-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"',
        '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"', '-I'+str(directory.parent), '-I'+str(ROOT/'main'),
        '-I'+str(ROOT/'components/device_protocol/include'), str(unit), str(crc_object), '-o', str(binary)],
        check=True, timeout=60)
    return json.loads(subprocess.check_output([str(binary), str(directory)], text=True, timeout=30))


class CheckpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        native.ForensicsTests.setUpClass()
        cls.native = native.ForensicsTests.root
        (cls.native/'esp_err.h').write_text(native.ERR+'\nconstexpr int ESP_ERR_INVALID_VERSION=266;\n')
        cls.replay = {d: build_oracle(d, cls.native/d, cls.native/'sdk-crc.o') for d in ('word', 'note')}

    @classmethod
    def tearDownClass(cls):
        native.ForensicsTests.tearDownClass()

    def session(self, domain='word', local=False):
        return (self.native/domain/('checkpoint-local-session.bin' if local else 'checkpoint-session.bin')).read_bytes()

    def material(self, files, domain='word'):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        path = Path(temp.name); (path/'export').mkdir()
        for name, raw in files.items(): (path/'export'/name).write_bytes(raw)
        if not files:
            (path/'export'/(f.LAYOUTS[domain].prefix+'.v1')).write_bytes(b'')
        f.capture(path/'export', path/'bundle', domain, True)
        manifest = json.loads((path/'bundle/manifest.json').read_text())
        return M.Material(path/'bundle', manifest)

    def row(self, domain='word'):
        width = f.LAYOUTS[domain].record.size
        return f.decode_record((self.native/domain/'healthy.bin').read_bytes()[4*width:5*width], domain, 4*width)[0]

    def native_error(self, raw, domain, descriptor=False, expected=SID):
        with tempfile.NamedTemporaryFile() as file:
            file.write(raw); file.flush()
            command = [str(self.native/domain/'checkpoint-oracle'), 'descriptor' if descriptor else 'session', file.name]
            if descriptor: command.append(expected)
            return json.loads(subprocess.check_output(command, text=True, timeout=10))['native_error']

    def repack(self, raw, offset, value, kind='B'):
        raw = bytearray(raw); struct.pack_into('<'+kind, raw, 16+offset, value)
        struct.pack_into('<I', raw, 12, f.journal_crc(raw[16:])); return bytes(raw)

    def test_native_session_codecs_valid_and_corrupt(self):
        for domain in ('word', 'note'):
            raw = self.session(domain)
            cases = [raw, raw[:7], raw[:-1], raw+b'x', raw[:30],
                raw[:6]+b'\x01\x00'+raw[8:], self.repack(raw, 0, 2), self.repack(raw, 1, 2),
                self.repack(raw, 3 if domain=='word' else 2, 255), self.repack(raw, 8 if domain=='word' else 6, 501, 'I')]
            bad_crc = bytearray(raw); bad_crc[-1] ^= 1; cases.append(bytes(bad_crc))
            future = bytearray(raw); future[4] = 99; cases.append(bytes(future))
            for value in cases:
                with self.subTest(domain=domain, digest=f.sha256(value)):
                    self.assertEqual(M.decode_session(value, domain)['native_error'], self.native_error(value, domain))

    def test_replay_target_matches_production_window_and_note_position(self):
        for domain in ('word', 'note'):
            stem = 'wsq' if domain=='word' else 'nses'
            result = self.material({stem+'.v1':self.session(domain)}, domain).validate(self.row(domain))
            self.assertEqual(result['state'], 'structurally_replayable')
            self.assertEqual(result['candidate']['position'], self.replay[domain]['position'])
            self.assertEqual(result['candidate']['next_sequence'], self.replay[domain]['next_sequence'])
            self.assertEqual(result['before']['next_sequence'], 7)
            if domain=='note':
                self.assertEqual(result['candidate']['last_opened_pin_update']['after'],self.replay[domain]['last_opened_at'])

    def test_native_descriptor_header_payload_identity_and_semantics(self):
        raw = (self.native/'word/checkpoint-descriptor.bin').read_bytes()
        for value in (raw, raw[:12], raw[:-1], raw+b'x', raw[:61]+self.repack(self.session(),0,2)[16:]):
            self.assertEqual(M.decode_descriptor(value,SID)['native_error'], self.native_error(value,'word',True))
        wrong = '11111111-1111-1111-1111-111111111112'
        self.assertEqual(M.decode_descriptor(raw,wrong)['native_error'], self.native_error(raw,'word',True,wrong))

    def test_valid_local_descriptor_binds_window_and_start_index(self):
        material = self.material({'wsq.v1':self.session(local=True), TOKEN+'.v1':(self.native/'word/checkpoint-descriptor.bin').read_bytes()})
        row=self.row(); row['local_checkpoint']['next_position']=1
        check=material.validate(row)
        self.assertEqual(check['state'],'structurally_replayable')
        self.assertEqual(check['candidate']['position'],1)
        self.assertEqual(check['descriptor_source']['file'],TOKEN+'.v1')
        row['local_checkpoint']['next_position']=1001
        self.assertEqual(material.validate(row).get('reason'),'checkpoint_ordinal_outside_candidate_window')

    def test_missing_session_or_temporary_only_never_proves_checkpoint(self):
        for files in ({}, {'wsq.tmp':self.session()}):
            check=self.material(files).validate(self.row())
            self.assertEqual(check['state'],'held')
            self.assertEqual(check['reason'],'session_material_missing_or_invalid')

    def test_corrupt_primary_uses_backup_without_promotion(self):
        material=self.material({'wsq.v1':b'torn','wsq.bak':self.session()})
        before={p.name:p.read_bytes() for p in material.source.iterdir()}
        result=material.validate(self.row())
        self.assertEqual(result['session_source']['file'],'wsq.bak')
        self.assertEqual(result['session_attempts'][0]['native_error'],266)
        self.assertEqual(before,{p.name:p.read_bytes() for p in material.source.iterdir()})

    def test_valid_wrong_mode_primary_does_not_fall_back(self):
        raw=self.repack(self.session(),3,1)
        result=self.material({'wsq.v1':raw,'wsq.bak':self.session()}).validate(self.row())
        self.assertEqual(result['reason'],'session_slot_mode_mismatch')

    def test_identity_item_and_window_mismatch_are_held(self):
        material=self.material({'wsq.v1':self.session()})
        for field,value,reason in [('session_id','aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa','session_identity_mismatch'),
                ('item_id','aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa','observation_item_not_in_candidate_window')]:
            row=self.row(); row[field]=value
            self.assertEqual(material.validate(row)['reason'],reason)
        row=self.row(); row['local_checkpoint']['next_position']=55
        self.assertEqual(material.validate(row).get('reason'),'checkpoint_ordinal_outside_candidate_window')

    def test_note_local_sequence_and_position_are_independent(self):
        material=self.material({'nses.v1':self.session('note')},'note'); row=self.row('note')
        row['local_checkpoint']['next_sequence']=99
        self.assertEqual(material.validate(row)['reason'],'note_next_sequence_inconsistent')
        row=self.row('note'); row['local_checkpoint']['next_position']=99
        self.assertEqual(material.validate(row)['reason'],'checkpoint_position_outside_candidate_window')

    def test_newer_snapshot_does_not_rewind_for_old_fact(self):
        raw=self.repack(self.session(),20,20,'Q')
        result=self.material({'wsq.v1':raw}).validate(self.row())
        self.assertEqual(result['state'],'covered_by_retained_snapshot')
        self.assertEqual(result['candidate'],result['before'])

    def test_older_note_fact_still_requires_its_last_opened_pin(self):
        raw=self.repack(self.session('note'),14,20,'Q')
        result=self.material({'nses.v1':raw},'note').validate(self.row('note'))
        self.assertEqual(result['state'],'requires_last_opened_pin_checkpoint')
        self.assertEqual(result['candidate']['next_sequence'],20)
        self.assertEqual(result['candidate'].get('last_opened_pin_update',{}).get('after'),'2026-11-05T00:00:00Z')

    def test_note_latest_pin_is_independent_of_ack_pending_suspend_partition(self):
        result=json.loads(subprocess.check_output([str(self.native/'note/checkpoint-oracle'),'replay-sets'],
            text=True,timeout=10))
        self.assertEqual(result['next_sequence'],10)
        self.assertEqual(result['last_opened_at'],'2026-11-05T00:00:03Z')

    def test_note_latest_pin_uses_sequence_when_device_clock_moves_backwards(self):
        result=json.loads(subprocess.check_output([str(self.native/'note/checkpoint-oracle'),'replay-sets'],
            text=True,timeout=10))
        self.assertEqual(result['reversed_clock_pin'],'2026-11-05T00:00:00Z')

    def test_note_checkpointed_snapshot_does_not_rewrite_for_intermediate_old_pins(self):
        result=json.loads(subprocess.check_output([str(self.native/'note/checkpoint-oracle'),'replay-sets'],
            text=True,timeout=10))
        self.assertFalse(result['second_changed'])

    def test_legacy_pause_does_not_attest_current_nvs(self):
        cursor=(self.native/'word/checkpoint-cursor.bin').read_bytes()
        result=self.material({'wsq.v1':self.session(),'wsq.cur':cursor}).validate(self.row())
        self.assertTrue(result['legacy_cursor']['native_valid'])
        self.assertTrue(result['legacy_cursor']['paused'])
        self.assertFalse(result['legacy_cursor']['authoritative_nvs'])
        self.assertIn('word_nvs_pause_not_exported',result['unverified_context'])
        self.assertFalse(M.decode_cursor(cursor+b'x',SID)['native_valid'])
        self.assertFalse(M.decode_cursor(cursor,'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa')['native_valid'])

    def test_native_cursor_validation_parity(self):
        raw=(self.native/'word/checkpoint-cursor.bin').read_bytes()
        damaged=bytearray(raw);damaged[12]^=1
        for value in (raw,raw[:-1],raw+b'x',bytes(damaged)):
            with tempfile.NamedTemporaryFile() as file:
                file.write(value);file.flush()
                result=json.loads(subprocess.check_output([str(self.native/'word/checkpoint-oracle'),
                    'cursor',file.name,SID],text=True,timeout=10))
                self.assertEqual(M.decode_cursor(value,SID)['native_valid'],result['native_valid'])
                if result['native_valid']:
                    self.assertEqual(M.decode_cursor(value,SID)['paused'],result['paused'])

    def test_bad_descriptor_and_temporary_only_are_not_promoted(self):
        for files in ({TOKEN+'.v1':b'corrupt'}, {TOKEN+'.tmp':(self.native/'word/checkpoint-descriptor.bin').read_bytes()}):
            files['wsq.v1']=self.session(local=True)
            result=self.material(files).validate(self.row())
            self.assertEqual(result['reason'],'descriptor_missing_or_invalid')

    def test_descriptor_binding_rejects_different_window_and_scope(self):
        raw=self.session(local=True)
        raw=self.repack(raw,12,1001,'I')
        result=self.material({'wsq.v1':raw,TOKEN+'.v1':(self.native/'word/checkpoint-descriptor.bin').read_bytes()}).validate(self.row())
        self.assertEqual(result['reason'],'descriptor_session_binding_mismatch')


class SemanticResult(unittest.TextTestResult):
    def printErrors(self):
        self.printErrorList('ERROR',self.errors)
        for case,error in self.failures:
            self.stream.writeln('FAIL checkpoint: '+self.getDescription(case))
            self.stream.writeln(error.strip().splitlines()[-1][:200])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('allow-window-mismatch','trust-legacy-nvs'))
    args=parser.parse_args()
    global M
    if args.mutation:
        body=(ROOT/'scripts/outbox_checkpoint.py').read_text()
        old,new={
            'allow-window-mismatch':("return held('checkpoint_ordinal_outside_candidate_window')", "positions = [0]"),
            'trust-legacy-nvs':("'authoritative_nvs': False", "'authoritative_nvs': True"),
        }[args.mutation]
        if body.count(old)!=1: raise ValueError('checkpoint mutation boundary drift')
        M=types.ModuleType('outbox_checkpoint_under_test');M.__file__=str(ROOT/'scripts/outbox_checkpoint.py')
        exec(compile(body.replace(old,new),M.__file__,'exec'),M.__dict__)
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(CheckpointTests)
    result=unittest.TextTestRunner(verbosity=2,resultclass=SemanticResult if args.mutation else unittest.TextTestResult).run(suite)
    print(f'Outbox checkpoint: {result.testsRun} scenarios, {len(result.failures)} failures, {len(result.errors)} errors (NOT HIL)',flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':raise SystemExit(main())
