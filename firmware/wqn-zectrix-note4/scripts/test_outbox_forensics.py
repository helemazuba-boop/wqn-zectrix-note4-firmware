#!/usr/bin/env python3
"""R-08 preservation/analysis against actual Word/Note encoders, scanners and SDK CRC.

Faults use local exports and libc/Python file boundaries, not devices or NOR.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

from test_binary_outbox_recovery import ERR, HEAP
from test_word_outbox_quota import section

ROOT = Path(__file__).resolve().parents[1]
FORENSICS = None
EVIDENCE_DIR = None


def function(body, marker):
    start = body.index(marker)
    brace = body.index('{', start)
    depth = 1
    at = brace + 1
    while depth:
        depth += (body[at] == '{') - (body[at] == '}')
        at += 1
    return body[start:at]


def build_oracle(domain, directory, crc_object):
    word = domain == 'word'
    body = (ROOT/'main'/('word_study_store.cpp' if word else 'note_store.cpp')).read_text()
    header = 'word_study_store.h' if word else 'note_store.h'
    records = section(body, '#pragma pack(push, 1)\nstruct SessionHeader {',
                      '\n// Every public operation below' if word else '\n// Parsed journal cached')
    functions = ['uint32_t Crc32(', 'bool CopyFixedText(', 'bool CopyField(', 'uint32_t RecordCrc(',
                 'bool ValidMode(', 'esp_err_t BuildObservationRecord(', 'esp_err_t BuildSuspendRecord(',
                 'esp_err_t ScanOutboxFile(']
    if not word:
        functions.insert(5, 'bool ValidAction(')
    constants = '\n'.join(re.search(r'constexpr [^;\n]+ '+name+r'\[?[^;\n]*;', body).group(0)
                          for name in ('kOutboxMagic', 'kOutboxSchemaVersion'))
    descriptor_paths = ''
    if word:
        local = (ROOT/'main/word_local_session.cpp').read_text()
        descriptor_paths = 'namespace wqn {\n' + function(local, 'bool IsValidLocalSessionUuid(') + '\n}\n'
        descriptor_paths += re.search(r'constexpr size_t kLocalDescriptorTokenChars = [^;]+;', body).group(0)
        descriptor_paths += '\n' + function(body, 'bool DeriveLocalDescriptorPaths(')
        descriptor_paths += r'''
void EmitDescriptorPaths() {
    for(const char* sid:{"11111111-2222-3333-4444-555555555555", "ABCDEF01-2345-6789-ABCD-EF0123456789"}) {
        char primary[40]{}, temporary[40]{}, backup[40]{};
        if(!DeriveLocalDescriptorPaths(sid,primary,temporary,backup))std::abort();
        std::printf("{\"descriptor_session\":\"%s\",\"paths\":[\"%s\",\"%s\",\"%s\"]}\n",
            sid,primary,temporary,backup);
    }
}
'''
    fixture = (ROOT/'scripts/testdata/outbox_forensics_codec.cpp').read_text()
    for old, new in {
        '@@HEADER@@': f'#include "{header}"', '@@CONSTANTS@@': constants,
        '@@RECORDS@@': records,
        '@@FUNCTIONS@@': '\n'.join(function(body, name) for name in functions),
        '@@OBSERVATION_TYPE@@': f'wqn::Durable{"Word" if word else "Note"}Observation',
        '@@EXTRA@@': 'value.next_phase=wqn::WordPresentationPhase::kBack;' if word else 'value.next_sequence=sequence+1;',
        '@@MODE_MAX@@': 'wqn::protocol::word_study_v1::Mode::kMistakes' if word else 'wqn::protocol::note_study_v1::Mode::kRecent',
        '@@ACTION_MAX@@': 'wqn::protocol::word_study_v1::ObservationAction::kLookedUp' if word else 'wqn::protocol::note_study_v1::ObservationAction::kSessionPaused',
        '@@CAPACITY@@': 'wqn::kWordObservationOutboxCapacity' if word else 'wqn::kNoteObservationOutboxCapacity',
        '@@DESCRIPTOR_PATHS@@': descriptor_paths,
        '@@DESCRIPTOR_EMIT@@': 'EmitDescriptorPaths();' if word else '',
    }.items():
        if fixture.count(old) != 1:
            raise ValueError('native forensic oracle boundary drift')
        fixture = fixture.replace(old, new)
    unit, binary = directory/'oracle.cpp', directory/'oracle'
    unit.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
        '-fno-pie', '-no-pie', '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"',
        '-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"', '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"',
        '-I'+str(directory.parent), '-I'+str(ROOT/'main'),
        '-I'+str(ROOT/'components/device_protocol/include'), str(unit), str(crc_object),
        '-o', str(binary)], check=True, timeout=60)
    result = subprocess.run([str(binary), str(directory)], text=True, stdout=subprocess.PIPE,
                            check=True, timeout=90)
    rows = [json.loads(row) for row in result.stdout.splitlines()]
    rows[0]['descriptor_paths'] = [row for row in rows[1:] if 'descriptor_session' in row]
    return rows[0], [row for row in rows[1:] if 'file' in row]


class ForensicsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix='wqn-outbox-forensics-')
        cls.root = Path(cls.workspace.name)
        (cls.root/'esp_err.h').write_text(ERR)
        (cls.root/'esp_heap_caps.h').write_text(HEAP)
        idf = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))
        crc = idf/'components/esp_rom/linux/esp_rom_crc.c'
        if not crc.is_file():
            raise RuntimeError('SDK CRC oracle unavailable: activate ESP-IDF; do not skip CRC parity')
        obj = cls.root/'sdk-crc.o'
        subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-fno-pie',
                        '-fsanitize=address,undefined', '-I'+str(idf/'components/esp_rom/include'),
                        '-c', str(crc), '-o', str(obj)], check=True, timeout=30)
        cls.vectors = {}
        for domain in ('word', 'note'):
            directory = cls.root/domain
            directory.mkdir()
            cls.vectors[domain] = build_oracle(domain, directory, obj)
        if EVIDENCE_DIR is not None:
            EVIDENCE_DIR.mkdir()
            for domain, vectors in cls.vectors.items():
                (EVIDENCE_DIR/f'{domain}-native.json').write_text(json.dumps(vectors, indent=2)+'\n')
                shutil.copyfile(cls.root/domain/'oracle.cpp', EVIDENCE_DIR/f'{domain}-oracle.cpp')
            (EVIDENCE_DIR/'sdk-crc.json').write_text(json.dumps({
                'source': str(crc), 'sha256': hashlib.sha256(crc.read_bytes()).hexdigest()}, indent=2)+'\n')

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def test_native_layout_crc_and_replay(self):
        for domain, (layout, rows) in self.vectors.items():
            with self.subTest(domain=domain):
                self.assertEqual(FORENSICS.LAYOUTS[domain].record.size, layout['record_bytes'])
                self.assertEqual(FORENSICS.LAYOUTS[domain].request_offset, layout['request_offset'])
                self.assertEqual(FORENSICS.journal_crc(b'123456789'), layout['crc_vector'])
                self.assertEqual(FORENSICS.STORAGE_PROFILES[FORENSICS.CURRENT_PROFILE][domain], layout['capacity'])
            for row in rows:
                with self.subTest(domain=domain, file=row['file']):
                    report = FORENSICS.analyze(self.root/domain/row['file'], domain)
                    self.assertTrue(report['analysis_complete'])
                    self.assertEqual(report['native_error_code'], row['error'])
                    self.assertEqual(report['partial_tail'], row['partial_tail'])
                    for name, value in report['prefix_counts'].items():
                        self.assertEqual(value, row[name], name)
                    self.assertFalse(report['repair_authorized'])

    def test_fields_come_from_native_bytes(self):
        for domain in ('word', 'note'):
            report = FORENSICS.analyze(self.root/domain/'healthy.bin', domain)
            self.assertEqual(report['records'][0].get('sequence'), 7)
            self.assertEqual(report['records'][0].get('local_checkpoint'),
                {'next_position':17,'next_phase':1} if domain=='word' else {'next_position':17,'next_sequence':8})
            self.assertEqual(report['records'][3]['suspend_reason'], 2)
            self.assertTrue(report['records'][0]['identity_usable_for_lookup'])
            self.assertEqual(report['prefix_counts']['pending'], 1)
            self.assertEqual(report['prefix_counts']['suspended'], 1)
            for name in ('opaque-padding.bin', 'opaque-utf8.bin'):
                record = FORENSICS.analyze(self.root/domain/name, domain)['records'][0]
                self.assertEqual(record['integrity'], 'valid')
                self.assertFalse(record['identity_usable_for_lookup'])

    def test_corrupt_roles_are_opaque_and_stop_replay(self):
        for domain in ('word', 'note'):
            for name, index in [('bad-observation.bin', 0), ('bad-ack.bin', 1), ('bad-suspend.bin', 3)]:
                report = FORENSICS.analyze(self.root/domain/name, domain)
                damaged = report['records'][index]
                self.assertEqual(damaged['integrity'], 'crc-mismatch')
                self.assertNotIn('request_id', damaged)
                self.assertNotIn('kind', damaged)
                self.assertNotIn('local_checkpoint', damaged)
                self.assertEqual(report['validated_prefix_records'], index)
                self.assertEqual(report['first_fault_offset'], index * report['record_bytes'])
                self.assertEqual(report['integrity_counts'].get('valid', 0), 4)
                self.assertEqual(len(report['records']), 5)  # Later bytes retained, never replayed.

    def test_analysis_is_bounded_without_truncating_raw_capture(self):
        path = self.root/'word/over-capacity.bin'
        report = FORENSICS.analyze(path, 'word', limit=2)
        self.assertFalse(report['analysis_complete'])
        self.assertIsNone(report['native_error_code'])
        self.assertEqual(report['inspected_records'], 2)
        complete = FORENSICS.analyze(path, 'word')
        self.assertTrue(complete['analysis_complete'])
        self.assertFalse(complete['record_details_complete'])
        self.assertEqual(len(complete['records']), FORENSICS.MAX_RECORD_DETAILS)
        self.assertEqual(complete['native_error_code'], 260)
        for limit in (0, -1, FORENSICS.MAX_RECORDS + 1):
            with self.assertRaises(ValueError):
                FORENSICS.analyze(path, 'word', limit)

    def test_capture_inventory_matches_actual_store_paths(self):
        for domain, name in [('word', 'word_study_store.cpp'), ('note', 'note_store.cpp')]:
            code = re.sub(r'//[^\n]*', '', (ROOT/'main'/name).read_text())
            actual = set(re.findall(r'"/storage/([^"/]+)"', code))
            concrete = {name for name in actual if '%' not in name}
            self.assertEqual(set(FORENSICS.source_names(domain, concrete)), concrete)
            self.assertEqual({name for name in actual if '%' in name},
                {'wl%s.v1', 'wl%s.tmp', 'wl%s.bak'} if domain == 'word' else set())

    def test_native_descriptor_names_and_opaque_variants_are_preserved(self):
        for row in self.vectors['word'][0]['descriptor_paths']:
            names = [Path(name).name for name in row['paths']]
            for name in names:
                self.assertIsNotNone(FORENSICS.DESCRIPTOR_NAME.fullmatch(name))
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp); source = self.new_export(directory, 'word')
                # An unpublished temp alone still establishes a descriptor family.
                data = b'opaque descriptor bytes\0\xff'; (source/names[1]).write_bytes(data)
                opaque = 'wlcorrupt.future'; (source/opaque).write_bytes(b'future/corrupt descriptor')
                output = directory/'bundle'; taken = FORENSICS.capture(source, output, 'word', True)
                manifest = json.loads((output/'manifest.json').read_text())
                self.assertEqual(manifest['storage_profile'], FORENSICS.CURRENT_PROFILE)
                self.assertIn(names[1], manifest['files'])
                self.assertEqual((output/names[1]).read_bytes(), data)
                self.assertEqual(manifest['files'][names[1]]['category'], 'local-session-descriptor')
                self.assertNotIn('analysis', manifest['files'][names[1]])
                self.assertIn(names[0], manifest['missing']); self.assertIn(names[2], manifest['missing'])
                self.assertEqual(manifest['files'][opaque]['category'], 'opaque-word-descriptor')
                self.assertEqual((output/opaque).read_bytes(), (source/opaque).read_bytes())
                self.assertEqual(FORENSICS.verify(output, taken['manifest_sha256'])['status'], 'verified')
                self.assertFalse(manifest['repair_authorized'])

    def test_personal_bitmap_families_are_raw_evidence_without_repair_authority(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp); source=self.new_export(directory,'word')
            round_cache='we'+'a'*24+'.tmp'
            for name in ('welig.v1',round_cache,'weunknown.future'):
                (source/name).write_bytes(b'opaque-personal-cache\0\xff')
            output=directory/'bundle';taken=FORENSICS.capture(source,output,'word',True)
            manifest=json.loads((output/'manifest.json').read_text())
            for name in ('welig.v1',round_cache,'weunknown.future'):
                self.assertIn(name,manifest['files'], 'personal cache was omitted from capture')
                self.assertEqual((output/name).read_bytes(),(source/name).read_bytes())
                self.assertNotIn('analysis',manifest['files'][name])
            self.assertEqual(manifest['files'][round_cache]['category'],'personal-eligibility-cache')
            self.assertFalse(manifest['repair_authorized'])
            self.assertEqual(FORENSICS.verify(output,taken['manifest_sha256'])['status'],'verified')

    def test_descriptor_inventory_limits_and_unsafe_files_fail_closed(self):
        with self.assertRaises(ValueError):
            FORENSICS.source_names('word', ('wl../outside',))
        with self.assertRaises(ValueError):
            FORENSICS.source_names('word', (f'wlopaque-{i}' for i in range(FORENSICS.MAX_DESCRIPTOR_FILES+1)))
        for kind in ('symlink', 'fifo', 'directory'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                directory=Path(tmp); source=self.new_export(directory,'word'); unsafe=source/'wlcorrupt.future'
                if kind=='symlink': unsafe.symlink_to(self.root/'word/healthy.bin')
                elif kind=='fifo': os.mkfifo(unsafe)
                else: unsafe.mkdir()
                output=directory/'bundle'
                with self.assertRaises((ValueError,OSError)):
                    FORENSICS.capture(source,output,'word',True)
                self.assertTrue((output/'.incomplete').exists())

    def test_source_inventory_and_already_copied_files_cannot_change_unnoticed(self):
        for change in ('new-descriptor','replace-copied-file'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                directory=Path(tmp); source=self.new_export(directory,'word'); output=directory/'bundle'
                copy=FORENSICS.copy_preserved; copied=[]
                def changing_copy(original,target):
                    result=copy(original,target); copied.append(original)
                    if len(copied)==2:
                        if change=='new-descriptor': (source/'wlnew.future').write_bytes(b'new')
                        else: copied[0].write_bytes(b'changed after copying')
                    return result
                with patch.object(FORENSICS,'copy_preserved',side_effect=changing_copy), self.assertRaises(ValueError):
                    FORENSICS.capture(source,output,'word',True)
                self.assertTrue((output/'.incomplete').exists())
                self.assertFalse((output/'manifest.json').exists())

    def test_legacy_bundle_keeps_legacy_capacity_and_analysis_rules(self):
        for case in ('capacity','analysis-budget'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                directory=Path(tmp); source=self.new_export(directory,'word'); output=directory/'bundle'
                if case=='capacity':
                    shutil.copyfile(self.root/'word/legacy-limit-exceeded.bin',source/'wout.v1')
                else:
                    record=(self.root/'word/healthy.bin').read_bytes()[:FORENSICS.LAYOUTS['word'].record.size]
                    (source/'wout.v1').write_bytes(record*8193)
                FORENSICS.capture(source,output,'word',True)
                manifest=json.loads((output/'manifest.json').read_text())
                self.assertEqual(manifest['files']['wout.v1']['analysis']['native_error_code'],0)
                manifest['format']=FORENSICS.LEGACY_FORMAT; del manifest['storage_profile']
                # New capacity, checkpoint fields and analysis budgets must not
                # be silently substituted when checking a historical v1 report.
                (output/'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaises(ValueError): FORENSICS.verify(output)
                for name, entry in manifest['files'].items():
                    if entry['category'] in ('journal','rejected-journal'):
                        entry['analysis']=FORENSICS.analyze(output/name,'word',profile=FORENSICS.LEGACY_PROFILE)
                self.assertEqual(manifest['files']['wout.v1']['analysis']['native_error_code'],
                    260 if case=='capacity' else None)
                (output/'manifest.json').write_text(json.dumps(manifest))
                self.assertEqual(FORENSICS.verify(output)['status'],'verified')

    def test_unknown_storage_profiles_are_rejected(self):
        for profile in ('unknown',FORENSICS.LEGACY_PROFILE,{},None):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                directory=Path(tmp); source=self.new_export(directory,'word'); output=directory/'bundle'
                FORENSICS.capture(source,output,'word',True)
                manifest=json.loads((output/'manifest.json').read_text()); manifest['storage_profile']=profile
                (output/'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaises(ValueError): FORENSICS.verify(output)

    def test_large_export_is_preserved_beyond_analysis_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); source = directory/'source'; source.mkdir(); output = directory/'bundle'
            record = (self.root/'word/healthy.bin').read_bytes()[:FORENSICS.LAYOUTS['word'].record.size]
            original = record * (FORENSICS.MAX_RECORDS + 5)
            (source/'wout.v1').write_bytes(original)
            receipt = FORENSICS.capture(source, output, 'word', True)
            manifest = json.loads((output/'manifest.json').read_text())
            stored = manifest['files']['wout.v1']
            self.assertEqual(stored['bytes'], len(original))
            self.assertEqual((output/'wout.v1').read_bytes(), original)
            self.assertFalse(stored['analysis']['analysis_complete'])
            self.assertIsNone(stored['analysis']['native_error_code'])
            self.assertEqual(len(stored['analysis']['records']), FORENSICS.MAX_RECORD_DETAILS)
            self.assertEqual(FORENSICS.verify(output, receipt['manifest_sha256'])['status'], 'verified')

    def new_export(self, directory, domain, broken='bad-ack.bin'):
        source = directory/'source'
        source.mkdir()
        prefix = FORENSICS.LAYOUTS[domain].prefix
        for suffix, vector in [('v1', broken), ('rec', 'bad-suspend.bin'), ('bak', 'future.bin'), ('tmp', 'healthy.bin')]:
            shutil.copyfile(self.root/domain/vector, source/f'{prefix}.{suffix}')
        session = 'wsq.v1' if domain == 'word' else 'nses.v1'
        (source/session).write_bytes(b'opaque session\0preserve exact bytes\xff')
        return source

    def test_capture_and_pinned_verification_preserve_all_originals(self):
        for domain in ('word', 'note'):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                source = self.new_export(directory, domain)
                before = {p.name: p.read_bytes() for p in source.iterdir()}
                output = directory/'bundle'
                raw = directory/'flash.bin'; raw.write_bytes(bytes(range(256)) * 1000)
                taken = FORENSICS.capture(source, output, domain, True, raw)
                self.assertEqual(taken['status'], 'preserved')
                self.assertFalse(taken['repair_authorized'])
                self.assertEqual({p.name: p.read_bytes() for p in source.iterdir()}, before)
                for name, data in before.items():
                    self.assertEqual((output/name).read_bytes(), data)
                    self.assertEqual((output/name).stat().st_mode & 0o777, 0o600)
                self.assertEqual(output.stat().st_mode & 0o777, 0o700)
                self.assertEqual((output/'raw-image.bin').read_bytes(), raw.read_bytes())
                self.assertEqual(FORENSICS.verify(output, taken['manifest_sha256'])['integrity'], 'pinned-digest')
                self.assertEqual(FORENSICS.verify(output)['integrity'], 'self-consistent')
                with self.assertRaises(FileExistsError):
                    FORENSICS.capture(source, output, domain, True)

    def test_future_layout_and_temporaries_never_authorize_repair(self):
        for domain in ('word', 'note'):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp); source = self.new_export(directory, domain)
                output = directory/'bundle'; FORENSICS.capture(source, output, domain, True)
                manifest = json.loads((output/'manifest.json').read_text())
                self.assertFalse(manifest['complete_device_backup'])
                prefix = FORENSICS.LAYOUTS[domain].prefix
                self.assertEqual(manifest['files'][prefix+'.bak']['analysis']['native_error_code'], 265)
                self.assertEqual(manifest['files'][prefix+'.tmp']['analysis']['native_error_code'], 0)
                self.assertEqual(manifest['runtime_candidate_order'], [prefix+'.v1', prefix+'.rec', prefix+'.bak'])
                self.assertFalse(manifest['repair_authorized'])

    def test_incomplete_and_tampered_bundles_are_rejected(self):
        for damage in ('incomplete', 'bytes', 'analysis', 'claim', 'entry-claim', 'missing', 'extra', 'digest', 'symlink'):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp); source = self.new_export(directory, 'word')
                output = directory/'bundle'; result = FORENSICS.capture(source, output, 'word', True)
                manifest = json.loads((output/'manifest.json').read_text())
                if damage == 'incomplete': (output/'.incomplete').write_text('not committed')
                if damage == 'bytes': (output/'wout.v1').write_bytes(b'changed')
                if damage == 'analysis': manifest['files']['wout.v1']['analysis']['prefix_counts']['pending'] = 0
                if damage == 'claim': manifest['repair_authorized'] = True
                if damage == 'entry-claim': manifest['files']['wout.v1']['repair_authorized'] = True
                if damage == 'missing': manifest['missing'] = []
                if damage == 'extra': (output/'unlisted').write_text('unexpected')
                if damage == 'symlink':
                    (output/'wout.v1').unlink(); (output/'wout.v1').symlink_to(source/'wout.v1')
                (output/'manifest.json').write_text(json.dumps(manifest))
                # JSON formatting changes do not provide authenticity; pinning
                # catches even an otherwise self-consistent altered manifest.
                with self.assertRaises((ValueError, OSError)):
                    FORENSICS.verify(output, result['manifest_sha256'] if damage == 'digest' else None)

    def test_capture_failure_keeps_source_and_marks_partial_bundle(self):
        for fault in ('read', 'write', 'fsync', 'changed'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp); source = self.new_export(directory, 'word')
                before = {p.name: p.read_bytes() for p in source.iterdir()}; output = directory/'bundle'
                if fault == 'read':
                    active = patch.object(FORENSICS, 'open_regular', side_effect=OSError('injected read failure'))
                elif fault == 'write':
                    active = patch.object(FORENSICS, 'copy_preserved', side_effect=OSError('injected write failure'))
                elif fault == 'fsync':
                    active = patch.object(FORENSICS.os, 'fsync', side_effect=OSError('injected fsync failure'))
                else:
                    original = FORENSICS.identity
                    count = 0
                    def inconsistent(info):
                        nonlocal count
                        count += 1
                        value = original(info)
                        return value if count == 1 else (*value[:-1], value[-1]+1)
                    active = patch.object(FORENSICS, 'identity', side_effect=inconsistent)
                with active, self.assertRaises((ValueError, OSError)):
                    FORENSICS.capture(source, output, 'word', True)
                self.assertEqual({p.name: p.read_bytes() for p in source.iterdir()}, before)
                self.assertTrue((output/'.incomplete').exists())
                self.assertFalse((output/'manifest.json').exists())
                with self.assertRaises(ValueError): FORENSICS.verify(output)

    def test_cli_receipt_never_prints_business_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); source = self.new_export(directory, 'word'); output = directory/'bundle'
            stdout, stderr = io.StringIO(), io.StringIO()
            arguments = ['outbox_forensics.py', 'capture', '--domain', 'word', '--source-dir', str(source),
                         '--output', str(output), '--offline']
            with patch.object(sys, 'argv', arguments), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(FORENSICS.main(), 0)
            taken = json.loads(stdout.getvalue())
            self.assertEqual(set(taken), {'status', 'manifest_sha256', 'files', 'repair_authorized'})
            self.assertEqual(stderr.getvalue(), '')
            stdout, stderr = io.StringIO(), io.StringIO()
            arguments = ['outbox_forensics.py', 'verify', '--bundle', str(output), '--expected-manifest-sha256', 'wrong']
            with patch.object(sys, 'argv', arguments), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(FORENSICS.main(), 1)
            self.assertEqual(json.loads(stderr.getvalue())['status'], 'failed')
            self.assertNotIn('request-one', stdout.getvalue()+stderr.getvalue())

    def test_final_directory_sync_error_is_uncertain(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); source = self.new_export(directory, 'word'); output = directory/'bundle'
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            calls = 0
            original = FORENSICS.sync_directory
            def final_sync_fails(path):
                nonlocal calls
                calls += 1
                if calls == 2:
                    raise OSError('injected final directory sync failure')
                return original(path)
            with patch.object(FORENSICS, 'sync_directory', side_effect=final_sync_fails):
                with self.assertRaises(FORENSICS.CaptureCommitUncertain):
                    FORENSICS.capture(source, output, 'word', True)
            self.assertEqual({p.name: p.read_bytes() for p in source.iterdir()}, before)
            self.assertTrue((output/'manifest.json').exists())
            self.assertTrue((output/'.incomplete').exists())
            with self.assertRaises(ValueError): FORENSICS.verify(output)
            calls = 0
            stdout, stderr = io.StringIO(), io.StringIO()
            arguments = ['outbox_forensics.py', 'capture', '--domain', 'word', '--source-dir', str(source),
                         '--output', str(directory/'cli-bundle'), '--offline']
            with patch.object(FORENSICS, 'sync_directory', side_effect=final_sync_fails), \
                    patch.object(sys, 'argv', arguments), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(FORENSICS.main(), 1)
            self.assertEqual(stdout.getvalue(), '')
            self.assertEqual(json.loads(stderr.getvalue())['status'], 'uncertain')

    def test_malformed_manifest_does_not_pass(self):
        for malformed in ('[]', '{"format": "x", "format": "y"}', '{"data": NaN}'):
            with tempfile.TemporaryDirectory() as tmp:
                bundle = Path(tmp)
                (bundle/'manifest.json').write_text(malformed)
                with self.assertRaises(ValueError): FORENSICS.verify(bundle)

    def test_unsafe_inputs_and_live_exports_rejected(self):
        for kind in ('symlink', 'fifo', 'directory', 'live', 'nested-output'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp); source = directory/'source'; source.mkdir()
                journal = source/'wout.v1'; output = directory/'bundle'
                if kind == 'symlink': journal.symlink_to(self.root/'word/healthy.bin')
                elif kind == 'fifo': os.mkfifo(journal)
                elif kind == 'directory': journal.mkdir()
                else: journal.write_bytes(b'not a live journal')
                if kind == 'nested-output': output = source/'bundle'
                with self.assertRaises((ValueError, OSError)):
                    FORENSICS.capture(source, output, 'word', kind != 'live')


class SemanticControlResult(unittest.TextTestResult):
    def printErrors(self):
        # Assertion failures are semantic receipts. Keep real exceptions with
        # their tracebacks so the CI classifier rejects infrastructure errors.
        self.printErrorList('ERROR', self.errors)
        for test, failure in self.failures:
            self.stream.writeln('FAIL: ' + self.getDescription(test))
            self.stream.writeln(failure.strip().splitlines()[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('wrong-crc', 'trust-analysis', 'legacy-word-capacity', 'ignore-descriptors'))
    parser.add_argument('--evidence-dir', type=Path)
    args = parser.parse_args()
    body = (ROOT/'scripts/outbox_forensics.py').read_text()
    if args.mutation:
        old, new = {
            'wrong-crc': ('zlib.crc32(data, 0xFFFFFFFF) ^ 0xFFFFFFFF', 'zlib.crc32(data)'),
            'trust-analysis': ("if category in ('journal', 'rejected-journal') and analyze(bundle/name, domain, profile=profile) != expected.get('analysis'):", 'if False:'),
            'legacy-word-capacity': ("CURRENT_PROFILE: {'word': 8192,", "CURRENT_PROFILE: {'word': 1000,"),
            'ignore-descriptors': ('for name in observed_names:', 'for name in ():'),
        }[args.mutation]
        if body.count(old) != 1:
            raise ValueError('forensic mutation boundary drift')
        body = body.replace(old, new)
    global FORENSICS, EVIDENCE_DIR
    EVIDENCE_DIR = args.evidence_dir
    FORENSICS = types.ModuleType('wqn_outbox_forensics_under_test')
    sys.modules[FORENSICS.__name__] = FORENSICS
    exec(compile(body, str(ROOT/'scripts/outbox_forensics.py'), 'exec'), FORENSICS.__dict__)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ForensicsTests)
    result = unittest.TextTestRunner(verbosity=2,
        resultclass=SemanticControlResult if args.mutation else unittest.TextTestResult).run(suite)
    print(f'Outbox forensics: {result.testsRun} scenarios, {len(result.failures)} failures, {len(result.errors)} errors (NOT HIL)', flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
