#!/usr/bin/env python3
"""Generate and independently recheck host-only native repair candidates.

Compiles the current production codec/scanner/replay plus the real SDK CRC.
Original exports never enter a native write path. Candidate bytes remain
non-installable until maintenance identity, NVS/scope/pins, publication and
physical acceptance exist.
"""
from collections import defaultdict
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

import outbox_checkpoint as checkpoint
import outbox_forensics as f

ROOT = Path(__file__).resolve().parents[1]
FORMAT = 'wqn-outbox-native-candidate-v1'
MAX_PACKAGE = 8 * 1024 * 1024


class NativeError(ValueError):
    def __init__(self, code, diagnostic=b''):
        super().__init__(code)
        self.diagnostic = diagnostic[-16384:]


def native_template():
    return (ROOT/'scripts/native_outbox_candidate.cpp').read_text()


def section(text, begin, end):
    if text.count(begin) != 1 or text.count(end) != 1:
        raise NativeError('NATIVE_SOURCE_BOUNDARY_CHANGED')
    return text[text.index(begin):text.index(end, text.index(begin))]


def function(text, marker):
    if text.count(marker) != 1:
        raise NativeError('NATIVE_SOURCE_BOUNDARY_CHANGED')
    start = text.index(marker)
    at, depth = text.index('{', start) + 1, 1
    while depth and at < len(text):
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    if depth:
        raise NativeError('NATIVE_SOURCE_BOUNDARY_CHANGED')
    return text[start:at]


def bounded(path, maximum):
    with f.open_regular(path) as stream:
        raw = stream.read(maximum + 1)
    if len(raw) > maximum:
        raise NativeError('NATIVE_INPUT_SIZE_LIMIT')
    return raw


class Driver:
    def __init__(self, domain, directory):
        self.domain, self.directory = domain, directory
        word = domain == 'word'
        sdk = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))
        crc = sdk/'components/esp_rom/linux/esp_rom_crc.c'
        err = sdk/'components/esp_common/include/esp_err.h'
        if not crc.is_file() or not err.is_file():
            raise NativeError('NATIVE_SDK_REQUIRED')
        store = ROOT/'main'/('word_study_store.cpp' if word else 'note_store.cpp')
        header = ROOT/'main'/('word_study_store.h' if word else 'note_store.h')
        body = store.read_text()
        names = ['kSessionMagic', 'kSessionSchemaVersion', 'kOutboxMagic', 'kOutboxSchemaVersion',
                 'kMaxSessionPayloadBytes', 'kMaxSessionCursorBytes']
        if not word:
            names += ['kMaxSnapshot', 'kMaxSessionItems']
        constants = []
        for name in names:
            found = re.search(r'constexpr [^;\n]+ '+name+r'[^;\n]*;', body)
            if not found:
                raise NativeError('NATIVE_SOURCE_BOUNDARY_CHANGED')
            constants.append(found.group(0))
        records = section(body, '#pragma pack(push, 1)\nstruct SessionHeader {',
            '\n// Every public operation below' if word else '\n// Parsed journal cached')
        functions = [function(body, name) for name in ('uint32_t Crc32(', 'bool CopyFixedText(')]
        functions += [section(body, 'template <typename T>\nvoid AppendScalar(', '\nesp_err_t AtomicWrite('),
                      function(body, 'esp_err_t LoadSessionFile(')]
        for name in ('uint32_t RecordCrc(', 'bool CopyField(', 'esp_err_t BuildObservationRecord(',
                     'esp_err_t ScanOutboxFile('):
            functions.append(function(body, name))
        if word:
            functions += [function(body, 'bool SetSessionCursorOrdinal(')]
        functions.append(function(body, 'void ReconcileSession('))
        functions.append(function(body, 'bool SessionCoversRecords(') if word else
                         function(body, 'void ReconcileSessionFromOutbox('))
        template = ROOT/'scripts/native_outbox_candidate.cpp'
        unit = native_template()
        replacements = {'@@HEADER@@': '#include "'+header.name+'"', '@@CONSTANTS@@': '\n'.join(constants),
            '@@RECORDS@@': records, '@@FUNCTIONS@@': '\n'.join(functions), '@@DOMAIN@@': 'Word' if word else 'Note',
            '@@DECLARATIONS@@': 'namespace wqn { uint32_t GetDeckScopeGeneration(){return 0;} }' if word else '',
            '@@LOAD_SCOPE@@': ', false' if word else ''}
        for marker, value in replacements.items():
            if marker not in unit:
                raise NativeError('NATIVE_TEMPLATE_CHANGED')
            unit = unit.replace(marker, value)
        error_defines = [line for line in err.read_text().splitlines() if re.match(r'#define ESP_(?:OK|FAIL|ERR_[A-Z_]+)\s', line)]
        f.write_private(directory/'esp_err.h', ('#pragma once\nusing esp_err_t = int;\n'+'\n'.join(error_defines)+'\n').encode())
        f.write_private(directory/'esp_heap_caps.h', b'''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1;
inline void* heap_caps_malloc(size_t n,unsigned){return std::malloc(n);}
inline void heap_caps_free(void* p){std::free(p);}
''')
        f.write_private(directory/'candidate.cpp', unit.encode())
        flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer', '-fno-pie']
        self.binary = directory/'candidate'
        try:
            subprocess.run(['gcc', *flags, '-I'+str(sdk/'components/esp_rom/include'), '-c', str(crc),
                '-o', str(directory/'crc.o')], check=True, capture_output=True, timeout=30)
            subprocess.run(['g++', '-std=c++17', *flags, '-no-pie', '-DWORD='+str(int(word)),
                '-DWQN_WORD_STUDY_SCHEMA_SHA256="host"', '-DWQN_NOTE_STUDY_SCHEMA_SHA256="host"',
                '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host"', '-I'+str(directory), '-I'+str(ROOT/'main'),
                '-I'+str(ROOT/'components/device_protocol/include'), str(directory/'candidate.cpp'),
                str(directory/'crc.o'), '-o', str(self.binary)], check=True, capture_output=True, timeout=60)
            compiler = subprocess.check_output(['g++', '-dumpfullversion'], text=True, timeout=5).strip()
        except (OSError, subprocess.SubprocessError) as error:
            if isinstance(error, subprocess.CalledProcessError):
                f.write_private(directory/'compiler-error.log', (error.stderr or b'')[-16384:])
            raise NativeError('NATIVE_BUILD_FAILED', getattr(error, 'stderr', b'') or b'') from error
        paths = [store, header, ROOT/'main/outbox_suspend_reason.h', template, Path(__file__),
                 ROOT/'components/device_protocol/include/device_protocol'/('word_study.h' if word else 'note_study.h'),
                 ROOT/'components/device_protocol/include/device_protocol/v3.h']
        self.receipt = {'compiler': compiler, 'sanitizers': ['address', 'undefined'],
            'inputs': {str(p.relative_to(ROOT)): f.sha256(p.read_bytes()) for p in paths},
            'sdk_crc_sha256': f.sha256(crc.read_bytes()), 'sdk_errors_sha256': f.sha256(err.read_bytes()),
            'sdk_crc_header_sha256': f.sha256((sdk/'components/esp_rom/include/esp_rom_crc.h').read_bytes()),
            'generated_unit_sha256': f.sha256(unit.encode()),
            'scope_generation_check': 'not_attested; native file decode only'}

    def run(self, session, journal, ids, job):
        job.mkdir(mode=0o700)
        for name, raw in [('source-session', session), ('source-journal', journal),
                          ('request-ids', ('\n'.join(ids)+'\n').encode())]:
            f.write_private(job/name, raw)
        try:
            done = subprocess.run([str(self.binary), str(job/'source-session'), str(job/'source-journal'),
                str(job/'request-ids'), str(job/'candidate-session'), str(job/'candidate-journal')],
                capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as error:
            raise NativeError('NATIVE_VALIDATION_DID_NOT_COMPLETE') from error
        if done.stderr or len(done.stdout) > 16384:
            raise NativeError('NATIVE_VALIDATION_DIAGNOSTIC', done.stderr)
        try:
            report = json.loads(done.stdout)
        except ValueError as error:
            raise NativeError('NATIVE_INVALID_REPORT') from error
        if done.returncode != 0:
            reason = report.get('error', '')
            raise NativeError(reason if re.fullmatch(r'NATIVE_[A-Z_]+', reason) else 'NATIVE_VALIDATION_FAILED')
        return (bounded(job/'candidate-session', 128*1024+16),
                bounded(job/'candidate-journal', 4*1024*1024), report)


def generate(api, source, source_pin, evidence, evidence_pin, plan_dir, plan_pin, output):
    api.check_pin(plan_pin)
    if plan_dir.is_symlink() or set(p.name for p in plan_dir.iterdir()) != {'plan.json'}:
        api.fail('PLAN_INCOMPLETE')
    saved, raw = api.read_json(plan_dir/'plan.json')
    plan = api.review_plan(source, source_pin, evidence, evidence_pin)
    if f.sha256(raw) != plan_pin or saved != plan:
        api.fail('PLAN_DIFFERS_FROM_EVIDENCE')
    if any(output.resolve().is_relative_to(p.resolve()) for p in (source, evidence, plan_dir)):
        api.fail('OUTPUT_INSIDE_EVIDENCE')
    proposals = plan['proposed_operations']
    if not proposals or not plan['native_source']:
        api.fail('NATIVE_NO_ACK_PROPOSALS')
    original = api.source_manifest(source, source_pin)
    domain = original['domain']
    info = original['files'][plan['native_source']]
    limit = f.STORAGE_PROFILES[original.get('storage_profile', f.LEGACY_PROFILE)]['analysis_records'] * f.LAYOUTS[domain].record.size
    if info['bytes'] + len(proposals)*f.LAYOUTS[domain].record.size > limit:
        api.fail('NATIVE_CANDIDATE_EXCEEDS_CAPTURE_BUDGET')
    groups = defaultdict(list)
    ids = []
    for op in proposals:
        if op['request_id'] in ids:
            api.fail('NATIVE_DUPLICATE_PROPOSAL')
        ids.append(op['request_id'])
        groups[op['checkpoint_session_source']['file']].append(op)
    def pinned(name, maximum):
        value = bounded(source/name, maximum)
        if len(value) != original['files'][name]['bytes'] or f.sha256(value) != original['files'][name]['sha256']:
            api.fail('SOURCE_CHANGED')
        return value
    journal = pinned(plan['native_source'], limit)
    api.start_output(output)
    files, proofs = {}, []
    def write(name, value):
        if sum(item['bytes'] for item in files.values()) + len(value) > MAX_PACKAGE:
            api.fail('NATIVE_PACKAGE_SIZE_LIMIT')
        f.write_private(output/name, value)
        files[name] = {'bytes': len(value), 'sha256': f.sha256(value)}
    try:
        with tempfile.TemporaryDirectory(prefix='wqn-native-candidate-') as temp:
            temp = Path(temp)
            driver = Driver(domain, temp)
            native_receipt = driver.receipt
            final_journal = None
            for index, (name, ops) in enumerate(sorted(groups.items())):
                session = pinned(name, checkpoint.MAX_PAYLOAD[domain]+16)
                candidate, candidate_journal, proof = driver.run(session, journal, ids, temp/f'job-{index}')
                if not candidate_journal.startswith(journal) or \
                        len(candidate_journal) != len(journal)+len(ids)*f.LAYOUTS[domain].record.size or \
                        proof['matching_proposals'] != len(ops) or \
                        proof['session_id'] != ops[0]['session_id'] or not proof['native_reload_verified'] or \
                        not proof['replay_partition_invariant']:
                    api.fail('NATIVE_PROOF_BINDING_MISMATCH')
                if final_journal is not None and final_journal != candidate_journal:
                    api.fail('NATIVE_JOURNAL_DIFFERS_ACROSS_SESSIONS')
                final_journal = candidate_journal
                candidate_name = f'checkpoint-{index:02d}.v1'
                write(candidate_name, candidate)
                proofs.append({'source_session': name, 'source_session_sha256': f.sha256(session),
                    'candidate_session': candidate_name, 'request_ids': [op['request_id'] for op in ops], 'native': proof})
            write('journal.v1', final_journal)
            write('proof.json', api.encoded({'native_build': native_receipt, 'sessions': proofs,
                'source_journal': plan['native_source'], 'source_journal_sha256': f.sha256(journal)}))
    except NativeError as error:
        # Retain bounded compiler/sanitizer evidence privately. A failed
        # candidate keeps .incomplete and never gains a success manifest.
        write('native-error.json', api.encoded({'reason': str(error),
            'diagnostic': error.diagnostic.decode('utf-8', errors='replace'),
            'repair_authorized': False}))
        f.sync_directory(output)
        api.fail(str(error))
    api.source_manifest(source, source_pin)
    result = {'format': FORMAT, 'source_manifest_sha256': source_pin, 'evidence_manifest_sha256': evidence_pin,
        'plan_sha256': plan_pin, 'domain': domain, 'files': files,
        'scope': 'append ACK markers and checkpoint every proposed session using all native record groups',
        'native_build': native_receipt, 'proposed_operations': len(proposals),
        'blocking_preconditions': [p for p in plan['blocking_preconditions'] if p != 'combined_native_candidate_not_validated'] +
            ['target_firmware_native_build_not_attested'],
        'installation_ready': False, 'repair_authorized': False, 'source_bytes_retained': True}
    pin = api.finish_output(output, 'manifest.json', result)
    return {'status': 'native_candidate_prepared', 'candidate_manifest_sha256': pin,
            'proposed_operations': len(proposals), 'installation_ready': False, 'repair_authorized': False}


def verify(api, source, source_pin, evidence, evidence_pin, plan_dir, plan_pin, candidate, candidate_pin):
    api.check_pin(candidate_pin)
    if candidate.is_symlink() or (candidate/'.incomplete').exists() or (candidate/'.incomplete').is_symlink():
        api.fail('NATIVE_CANDIDATE_INCOMPLETE')
    manifest, raw = api.read_json(candidate/'manifest.json', f.MAX_MANIFEST_BYTES)
    if f.sha256(raw) != candidate_pin:
        api.fail('NATIVE_CANDIDATE_DIGEST_MISMATCH')
    with tempfile.TemporaryDirectory(prefix='wqn-native-verify-') as temp:
        rebuilt = Path(temp)/'candidate'
        generate(api, source, source_pin, evidence, evidence_pin, plan_dir, plan_pin, rebuilt)
        wanted = {p.name for p in rebuilt.iterdir()}
        if {p.name for p in candidate.iterdir()} != wanted:
            api.fail('NATIVE_CANDIDATE_FILE_INVENTORY_MISMATCH')
        for name in wanted:
            expected = bounded(rebuilt/name, MAX_PACKAGE)
            actual = bounded(candidate/name, MAX_PACKAGE)
            if actual != expected:
                api.fail('NATIVE_CANDIDATE_DIFFERS_FROM_RECOMPUTED_BYTES')
    return {'status': 'native_candidate_verified', 'installation_ready': False, 'repair_authorized': False}
