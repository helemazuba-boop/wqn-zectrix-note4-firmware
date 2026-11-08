#!/usr/bin/env python3
"""Collect device-authenticated receipts for pinned offline exports; prepare review plans.

No device, database, source bundle or upload queue is modified. Collection uses
only the dedicated read RPC endpoint. Plans remain non-authorizing until the
separate maintenance identity, checkpoint and publication workflow exists.
"""
import argparse
import base64
from collections import Counter, defaultdict
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

import outbox_forensics as f
import outbox_checkpoint as checkpoint

FORMAT = 'wqn-outbox-reconciliation-evidence-v1'
PLAN_FORMAT = 'wqn-outbox-repair-review-v2'
PLAN_NVS_FORMAT = 'wqn-outbox-repair-review-v3'
CONTRACT = 'outbox-reconciliation-v1'
SCHEMA_SHA = '8d8f8f8ed86f94eb8cca0999be0a0e97b2ea727f72c496f61e6a5bf3bbe83d69'
ENDPOINT_PATH = '/api/esp32/v3/study/observations/receipts'
MAX_RESPONSE = 65536
MAX_BATCHES = 2048
MAX_JSON = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MODES = {'word': ('sequential', 'random', 'dictionary', 'review', 'intake', 'shuffle', 'mistakes'),
         'note': ('sequential', 'recent')}
ACTIONS = {'word': ('shown', 'revealed', 'known', 'unknown', 'skipped', 'looked_up'),
           'note': ('opened', 'read_completed', 'skipped', 'session_paused')}
SHA = re.compile(r'[0-9a-f]{64}\Z')
FILE = re.compile(r'(?:inventory|assessment|(?:request|response)-\d{6})\.json\Z')
TIMESTAMP = re.compile(r'\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}(?:\.(\d+))?(?:[Zz]|[+-]\d{2}:\d{2})\Z')


class EvidenceError(ValueError):
    pass


def fail(code):
    raise EvidenceError(code)


def encoded(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':'))+'\n').encode()


def parsed(raw):
    try:
        return json.loads(raw, object_pairs_hook=f.unique_object, parse_constant=f.reject_nonfinite)
    except (ValueError, UnicodeError, RecursionError):
        fail('INVALID_JSON_EVIDENCE')


def read_json(path, limit=MAX_JSON):
    with f.open_regular(path) as stream:
        raw = stream.read(limit+1)
    if len(raw) > limit:
        fail('EVIDENCE_SIZE_LIMIT')
    return parsed(raw), raw


def check_pin(value):
    if not isinstance(value, str) or not SHA.fullmatch(value):
        fail('INDEPENDENT_SHA256_REQUIRED')


def source_manifest(source, pin):
    check_pin(pin)
    try:
        f.verify(source, pin)
        manifest, _ = read_json(source/'manifest.json', f.MAX_MANIFEST_BYTES)
        return manifest
    except (ValueError, OSError, TypeError, KeyError):
        fail('SOURCE_BUNDLE_INVALID')


def protocol_validator():
    # Match the existing pinned gate environment; a missing optional format
    # checker must never silently weaken validation of canonical timestamps.
    try:
        from jsonschema import Draft202012Validator, FormatChecker
        import rfc3339_validator  # noqa: F401
    except ImportError:
        fail('ACTIVATE_PINNED_SCHEMA_ENVIRONMENT')
    root = Path(__file__).resolve().parents[1]/'contracts'/CONTRACT
    manifest, _ = read_json(root/'manifest.json')
    schema, raw = read_json(root/(CONTRACT+'.schema.json'))
    if f.sha256(raw) != SCHEMA_SHA or manifest.get('schema_sha256') != SCHEMA_SHA:
        fail('CONTRACT_SCHEMA_CHANGED')
    return Draft202012Validator(schema, format_checker=FormatChecker())


def endpoint_url(endpoint, allow_http=False):
    if not isinstance(endpoint, str) or len(endpoint) > 2048:
        fail('INVALID_RECEIPT_ENDPOINT')
    value = urllib.parse.urlsplit(endpoint)
    if value.username or value.password or value.query or value.fragment or value.path != ENDPOINT_PATH:
        fail('INVALID_RECEIPT_ENDPOINT')
    if value.scheme == 'https' and value.hostname:
        return endpoint, 'device_bearer_https'
    if allow_http and value.scheme == 'http' and value.hostname in ('127.0.0.1', '::1'):
        return endpoint, 'loopback_test_http'
    fail('HTTPS_REQUIRED')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def transport_worker():
    """Private stdin carries credentials; stdout never echoes credentials/errors."""
    status = None
    try:
        raw = sys.stdin.buffer.read(32769)
        if len(raw) > 32768:
            fail('WORKER_INPUT_LIMIT')
        query = parsed(raw)
        token = query['token']
        if not isinstance(token, str) or not SHA.fullmatch(token):
            fail('INVALID_DEVICE_CREDENTIAL')
        endpoint, mode = endpoint_url(query['endpoint'], query['allow_http'])
        body = query['body'].encode()
        if len(body) > 16384:
            fail('REQUEST_SIZE_LIMIT')
        handlers = [NoRedirect()]
        if mode == 'loopback_test_http':
            handlers.append(urllib.request.ProxyHandler({}))
        opener = urllib.request.build_opener(*handlers)
        request = urllib.request.Request(endpoint, data=body, method='POST', headers={
            'Authorization': 'Bearer '+token, 'Content-Type': 'application/json',
            'X-WQN-Protocol': '3', 'X-WQN-Request-Id': parsed(body)['request_id'],
            'User-Agent': 'WQN-OutboxEvidence/1',
        })
        try:
            response = opener.open(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status = response.code
            payload = bytearray()
            while True:
                block = response.read1(min(8192, MAX_RESPONSE+1-len(payload)))
                if not block:
                    break
                payload.extend(block)
                if len(payload) > MAX_RESPONSE:
                    fail('RESPONSE_SIZE_LIMIT')
            decoded = parsed(bytes(payload))
            if token in bytes(payload).decode('utf-8').lower() or token in encoded(decoded).decode().lower():
                fail('CREDENTIAL_REFLECTION')
            result = {'http_status': response.code, 'raw': base64.b64encode(payload).decode(), 'error': None}
    except EvidenceError as error:
        result = {'http_status': status, 'raw': None, 'error': str(error)}
    except Exception:
        result = {'http_status': None, 'raw': None, 'error': 'TRANSPORT_FAILURE'}
    sys.stdout.buffer.write(encoded(result))


def fetch(endpoint, token, body, allow_http, timeout):
    # Killing the dedicated child bounds DNS, TLS, redirects and slow body
    # reads together. No unjoined thread can continue using the credential.
    worker = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--transport-worker'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        output, _ = worker.communicate(encoded({'endpoint': endpoint, 'token': token,
            'body': body.decode(), 'allow_http': allow_http}), timeout=timeout)
    except subprocess.TimeoutExpired:
        worker.kill()
        worker.communicate()
        return {'http_status': None, 'raw': None, 'error': 'TRANSPORT_DEADLINE'}
    except BaseException:
        # Cancellation must stop the credential-using child as well.
        worker.kill()
        worker.communicate()
        raise
    if worker.returncode != 0 or len(output) > 100000:
        return {'http_status': None, 'raw': None, 'error': 'TRANSPORT_WORKER_FAILED'}
    return parsed(output)


def inventory(source, manifest):
    domain = manifest['domain']
    profile = manifest.get('storage_profile', f.LEGACY_PROFILE)
    rows = []
    for name, info in sorted(manifest['files'].items()):
        if info['category'] not in ('journal', 'rejected-journal'):
            continue
        size = f.LAYOUTS[domain].record.size
        with f.open_regular(source/name) as stream:
            for index in range(f.STORAGE_PROFILES[profile]['analysis_records']):
                raw = stream.read(size)
                if not raw:
                    break
                entry, _, _ = f.decode_record(raw, domain, index*size)
                entry['file'] = name
                rows.append(entry)
                if len(raw) != size:
                    break
            at = stream.tell()
            if at < info['bytes']:
                rows.append({'file': name, 'offset': at, 'bytes': info['bytes']-at,
                    'integrity': 'not-analyzed-range'})
    return rows


def logical_key(row):
    return row['session_id'].lower(), row['request_id']


def query_groups(rows):
    groups = defaultdict(set)
    for row in rows:
        if row.get('kind') == 'observation' and row.get('identity_usable_for_lookup'):
            groups[row['session_id'].lower()].add(row['request_id'])
    return [(sid, sorted(ids)[at:at+64]) for sid, ids in sorted(groups.items()) for at in range(0, len(ids), 64)]


def checked_reply(request, reply, status, actor, validator):
    if status != 200:
        fail('HTTP_'+str(status))
    if not validator.is_valid(reply) or reply.get('ok') is not True:
        fail('INVALID_REMOTE_SCHEMA')
    data = reply['data']
    if reply['request_id'] != request['request_id'] or {k: v.lower() for k, v in data['actor'].items()} != actor or \
            data['session']['session_id'].lower() != request['session_id'].lower() or \
            data['session']['domain'] != request['domain'] or len(data['receipts']) != len(request['request_ids']):
        fail('REMOTE_BINDING_MISMATCH')
    for expected, item in zip(request['request_ids'], data['receipts']):
        if item['request_id'] != expected:
            fail('REMOTE_REQUEST_ORDER_MISMATCH')
        if item['state'] == 'found':
            value = item['observation']
            if value['request_id'] != expected or value['session_id'].lower() != request['session_id'].lower() or \
                    value['mode'] != data['session']['mode'] or value['action'] not in ACTIONS[request['domain']]:
                fail('REMOTE_FACT_BINDING_MISMATCH')
    return data


def instant(value):
    match = TIMESTAMP.fullmatch(value)
    if not match or (match[1] and len(match[1].rstrip('0')) > 6):
        fail('TIMESTAMP_PRECISION_UNRESOLVED')
    try:
        return dt.datetime.fromisoformat(value.upper().replace('Z', '+00:00')).astimezone(dt.timezone.utc)
    except (ValueError, OverflowError):
        fail('TIMESTAMP_UNRESOLVED')


def compare_fact(row, observation, domain):
    differences = [key for key in ('request_id', 'sequence') if row[key] != observation[key]]
    differences += [key for key in ('session_id', 'item_id') if row[key].lower() != observation[key].lower()]
    if MODES[domain][row['mode']] != observation['mode']:
        differences.append('mode')
    if ACTIONS[domain][row['action']] != observation['action']:
        differences.append('action')
    try:
        if instant(row['occurred_at']) != instant(observation['occurred_at']):
            differences.append('occurred_at')
    except EvidenceError:
        return {'state': 'unresolved_timestamp', 'differences': ['occurred_at']}
    if differences:
        return {'state': 'payload_mismatch', 'differences': differences}
    return {'state': 'matched', 'observation_id': observation['observation_id']}


def assess(rows, requests, results, domain, actor, validator, root):
    lookup, outcomes = {}, []
    for request, batch in zip(requests, results):
        try:
            if batch['transport_error']:
                fail(batch['transport_error'])
            reply, _ = read_json(root/batch['response_file'], MAX_RESPONSE)
            data = checked_reply(request, reply, batch['http_status'], actor, validator)
            for entry in data['receipts']:
                lookup[(request['session_id'].lower(), entry['request_id'])] = entry
            outcomes.append({'state': 'verified', 'read_at': data['read_at']})
        except EvidenceError as error:
            outcomes.append({'state': 'unavailable', 'reason': str(error)})
    assessed = []
    for row in rows:
        result = {'file': row['file'], 'offset': row['offset'], 'state': 'opaque'}
        if row.get('kind') in ('ack', 'suspend'):
            result['state'] = 'local_marker'
        if row.get('kind') == 'observation' and row.get('identity_usable_for_lookup'):
            remote = lookup.get(logical_key(row))
            result['state'] = remote['state'] if remote else 'unavailable'
            if remote and remote['state'] == 'found':
                result.update(compare_fact(row, remote['observation'], domain))
        assessed.append(result)
    return {'records': assessed, 'batches': outcomes,
            'authenticated_batches': sum(batch['state'] == 'verified' for batch in outcomes),
            'query_complete': all(batch['state'] == 'verified' for batch in outcomes)}


def start_output(output):
    output.mkdir(mode=0o700)
    f.write_private(output/'.incomplete', b'evidence not committed\n')


def finish_output(output, filename, value):
    raw = encoded(value)
    f.write_private(output/filename, raw)
    f.sync_directory(output)
    try:
        (output/'.incomplete').unlink()
        f.sync_directory(output)
    except OSError as error:
        try:
            f.write_private(output/'.incomplete', b'finalization uncertain\n')
        except OSError:
            pass
        raise f.CaptureCommitUncertain('EVIDENCE_FINALIZATION_UNCERTAIN') from error
    return f.sha256(raw)


def collect(source, source_pin, output, endpoint, token_file, actor, allow_http=False,
            timeout=300, batch_timeout=10, transport=None):
    if not all(isinstance(actor.get(k), str) and f.UUID.fullmatch(actor[k]) for k in ('user_id', 'device_id')) or \
            set(actor) != {'user_id', 'device_id'}:
        fail('EXPECTED_ACTOR_REQUIRED')
    actor = {k: v.lower() for k, v in actor.items()}
    endpoint, mode = endpoint_url(endpoint, allow_http)
    if not all(math.isfinite(n) and 0 < n <= 3600 for n in (timeout, batch_timeout)):
        fail('INVALID_DEADLINE')
    manifest = source_manifest(source, source_pin)
    if output.resolve().is_relative_to(source.resolve()):
        fail('OUTPUT_INSIDE_SOURCE')
    validator = protocol_validator()
    rows = inventory(source, manifest)
    groups = query_groups(rows)
    if len(groups) > MAX_BATCHES:
        fail('QUERY_BATCH_LIMIT')
    with f.open_regular(token_file) as stream:
        raw_token = stream.read(257)
    if len(raw_token) > 256:
        fail('INVALID_DEVICE_CREDENTIAL')
    token = raw_token.strip().decode('ascii')
    if not SHA.fullmatch(token):
        fail('INVALID_DEVICE_CREDENTIAL')
    start_output(output)
    files, total = {}, 0
    def write(name, raw):
        nonlocal total
        if token.encode() in raw.lower():
            fail('CREDENTIAL_REFLECTION')
        total += len(raw)
        if total > MAX_TOTAL_BYTES:
            fail('EVIDENCE_SIZE_LIMIT')
        f.write_private(output/name, raw)
        files[name] = {'bytes': len(raw), 'sha256': f.sha256(raw)}
    write('inventory.json', encoded(rows))
    requests, batches = [], []
    boot, stopped = 'evidence_'+uuid.uuid4().hex, False
    deadline = time.monotonic()+timeout
    for index, (sid, ids) in enumerate(groups):
        body = {'request_id': 'rcpt_'+uuid.uuid4().hex, 'boot_id': boot,
                'firmware_version': 'outbox-evidence-v1', 'capabilities': ['outbox.receipts.v1'],
                'contract': CONTRACT, 'domain': manifest['domain'], 'session_id': sid, 'request_ids': ids}
        if not validator.is_valid(body):
            fail('INVALID_QUERY_SCHEMA')
        request_name, response_name = f'request-{index:06d}.json', f'response-{index:06d}.json'
        request_raw = encoded(body)
        write(request_name, request_raw)
        remaining = deadline-time.monotonic()
        if stopped or remaining <= 0:
            result = {'http_status': None, 'raw': None, 'error': 'NOT_QUERIED_AFTER_STOP'}
        else:
            result = (transport or fetch)(endpoint, token, request_raw, allow_http, min(remaining, batch_timeout))
        batch = {'request_file': request_name, 'response_file': None,
                 'http_status': result['http_status'], 'transport_error': result['error']}
        if result['raw'] is not None:
            raw = base64.b64decode(result['raw'], validate=True)
            if len(raw) > MAX_RESPONSE:
                fail('RESPONSE_SIZE_LIMIT')
            # Also inspect decoded strings so escaped credentials cannot be
            # written into a private artifact by a reflecting/malicious server.
            if token.encode() in encoded(parsed(raw)).lower():
                fail('CREDENTIAL_REFLECTION')
            write(response_name, raw)
            batch['response_file'] = response_name
        requests.append(body)
        batches.append(batch)
        outcome = assess([], [body], [batch], manifest['domain'], actor, validator, output)
        stopped = stopped or not outcome['query_complete']
    assessment = assess(rows, requests, batches, manifest['domain'], actor, validator, output)
    write('assessment.json', encoded(assessment))
    # Neither network time nor output publication weakens the original pin.
    source_manifest(source, source_pin)
    if inventory(source, manifest) != rows:
        fail('SOURCE_CHANGED')
    evidence = {'format': FORMAT, 'source_manifest_sha256': source_pin,
        'source_domain': manifest['domain'], 'expected_actor': actor, 'endpoint': endpoint,
        'authentication': mode, 'source_actor_binding': 'operator_asserted_unverified',
        'contract_schema_sha256': SCHEMA_SHA, 'files': files, 'batches': batches,
        'query_complete': assessment['query_complete'], 'repair_authorized': False}
    pin = finish_output(output, 'manifest.json', evidence)
    return {'status': 'evidence_preserved', 'manifest_sha256': pin,
        'query_complete': assessment['query_complete'], 'batches': len(batches),
        'authenticated_batches': assessment['authenticated_batches'], 'repair_authorized': False}


def verify(source, source_pin, evidence, evidence_pin):
    original = source_manifest(source, source_pin)
    check_pin(evidence_pin)
    if evidence.is_symlink() or not evidence.is_dir() or \
            (evidence/'.incomplete').exists() or (evidence/'.incomplete').is_symlink():
        fail('EVIDENCE_INCOMPLETE')
    manifest, raw = read_json(evidence/'manifest.json', f.MAX_MANIFEST_BYTES)
    if f.sha256(raw) != evidence_pin:
        fail('EVIDENCE_DIGEST_MISMATCH')
    keys = {'format', 'source_manifest_sha256', 'source_domain', 'expected_actor', 'endpoint',
            'authentication', 'source_actor_binding', 'contract_schema_sha256', 'files', 'batches',
            'query_complete', 'repair_authorized'}
    if set(manifest) != keys or manifest['format'] != FORMAT or manifest['repair_authorized'] is not False or \
            manifest['source_manifest_sha256'] != source_pin or manifest['source_domain'] != original['domain'] or \
            manifest['source_actor_binding'] != 'operator_asserted_unverified' or manifest['contract_schema_sha256'] != SCHEMA_SHA:
        fail('INVALID_EVIDENCE_MANIFEST')
    _, mode = endpoint_url(manifest['endpoint'], manifest['authentication'] == 'loopback_test_http')
    actor = manifest['expected_actor']
    if mode != manifest['authentication'] or set(actor) != {'user_id', 'device_id'} or \
            any(not isinstance(v, str) or not f.UUID.fullmatch(v) or v != v.lower() for v in actor.values()):
        fail('INVALID_ACTOR_BINDING')
    if not isinstance(manifest['files'], dict) or not isinstance(manifest['batches'], list) or len(manifest['batches']) > MAX_BATCHES:
        fail('INVALID_EVIDENCE_INVENTORY')
    if set(p.name for p in evidence.iterdir()) != set(manifest['files']) | {'manifest.json'}:
        fail('UNLISTED_EVIDENCE_FILES')
    total = 0
    for name, expected in manifest['files'].items():
        if not FILE.fullmatch(name) or not isinstance(expected, dict) or set(expected) != {'bytes', 'sha256'} or \
                type(expected['bytes']) is not int or expected['bytes'] < 0 or \
                not isinstance(expected['sha256'], str) or not SHA.fullmatch(expected['sha256']):
            fail('UNSAFE_EVIDENCE_FILE')
        _, payload = read_json(evidence/name)
        total += len(payload)
        if total > MAX_TOTAL_BYTES or len(payload) != expected['bytes'] or f.sha256(payload) != expected['sha256']:
            fail('EVIDENCE_FILE_CHANGED')
    rows = inventory(source, original)
    saved, _ = read_json(evidence/'inventory.json')
    if saved != rows:
        fail('SOURCE_INVENTORY_MISMATCH')
    requests, wanted = [], {'inventory.json', 'assessment.json'}
    validator, groups = protocol_validator(), query_groups(rows)
    if len(groups) != len(manifest['batches']):
        fail('QUERY_COVERAGE_MISMATCH')
    for index, (batch, (sid, ids)) in enumerate(zip(manifest['batches'], groups)):
        if set(batch) != {'request_file', 'response_file', 'http_status', 'transport_error'} or \
                batch['request_file'] != f'request-{index:06d}.json' or \
                batch['response_file'] not in (None, f'response-{index:06d}.json') or \
                (batch['transport_error'] is not None and not re.fullmatch('[A-Z_]+', batch['transport_error'])) or \
                (batch['http_status'] is not None and (type(batch['http_status']) is not int or not 100 <= batch['http_status'] <= 599)) or \
                (batch['response_file'] is None and batch['transport_error'] is None):
            fail('INVALID_BATCH_EVIDENCE')
        wanted.add(batch['request_file'])
        request, _ = read_json(evidence/batch['request_file'], 16384)
        if not validator.is_valid(request) or request['session_id'] != sid or request['request_ids'] != ids or \
                request['domain'] != original['domain']:
            fail('QUERY_BINDING_MISMATCH')
        requests.append(request)
        if batch['response_file']:
            wanted.add(batch['response_file'])
    if set(manifest['files']) != wanted:
        fail('INVALID_BATCH_FILE_INVENTORY')
    assessment = assess(rows, requests, manifest['batches'], original['domain'], actor, validator, evidence)
    saved, _ = read_json(evidence/'assessment.json')
    if saved != assessment or manifest['query_complete'] is not assessment['query_complete']:
        fail('ASSESSMENT_DIFFERS_FROM_RAW_EVIDENCE')
    return original, rows, assessment, manifest


def review_plan(source, source_pin, evidence, evidence_pin, *, nvs_inputs=None,
                nvs_inputs_pin=None, sensitive_confirmed=False, output=None):
    original, rows, assessment, manifest = verify(source, source_pin, evidence, evidence_pin)
    nvs = None
    if nvs_inputs is not None or nvs_inputs_pin is not None or sensitive_confirmed:
        if nvs_inputs is None or nvs_inputs_pin is None:
            fail('NVS_INPUTS_PAIR_REQUIRED')
        import outbox_nvs_context
        try:
            nvs = outbox_nvs_context.VerifiedContext(nvs_inputs, nvs_inputs_pin, source_pin,
                original['domain'], sensitive_confirmed, output)
        except outbox_nvs_context.ContextError as error:
            fail(str(error))
    selected, blockers = None, ['device_source_identity_not_attested', 'maintenance_revalidation_required',
        'checkpoint_runtime_context_not_attested', 'combined_native_candidate_not_validated',
        'controlled_publication_not_implemented',
        'explicit_repair_authorization_required']
    for name in original['runtime_candidate_order']:
        if name not in original['files']:
            continue
        report = original['files'][name]['analysis']
        if not report['analysis_complete']:
            blockers.append('runtime_source_not_fully_analyzed')
            break
        if report['native_error_code'] == 0:
            selected = name
            if report['partial_tail']:
                blockers.append('runtime_source_has_partial_tail')
            break
    if selected is None:
        blockers.append('no_safe_native_source')
    if any(row['file'] == selected and not row.get('identity_usable_for_lookup') for row in rows):
        blockers.append('runtime_source_has_unusable_record_identity')
    local, variants = defaultdict(set), defaultdict(set)
    observed = {}
    for row in rows:
        if row.get('kind') == 'observation' and row.get('identity_usable_for_lookup'):
            variants[logical_key(row)].add(row['sha256'])
        if row['file'] != selected or not row.get('identity_usable_for_lookup'):
            continue
        key = row['request_id']
        if row.get('kind') == 'observation':
            local[key].add('pending')
            observed.setdefault(key, row)
        elif row.get('kind') in ('ack', 'suspend') and 'pending' in local[key]:
            local[key].discard('pending')
            local[key].add('suspended' if row['kind'] == 'suspend' else 'acknowledged')
    decisions, operations, checkpoint_checks = [], [], []
    material = checkpoint.Material(source, original, nvs)
    for row, item in zip(rows, assessment['records']):
        action = 'retain_raw_evidence'
        if row.get('kind') == 'observation' and row.get('identity_usable_for_lookup'):
            state = local[row['request_id']] if row['file'] == selected else set()
            if row['file'] != selected:
                action = 'preserve_other_source_copy'
            elif len(variants[logical_key(row)]) > 1:
                action = 'hold_conflicting_local_variants'
            elif 'suspended' in state:
                action = 'preserve_suspended'
            elif state == {'acknowledged'}:
                action = 'preserve_acknowledged'
            elif state == {'pending'} and item['state'] == 'matched' and \
                    'runtime_source_has_partial_tail' not in blockers and \
                    'runtime_source_has_unusable_record_identity' not in blockers:
                check = material.validate(row)
                checkpoint_checks.append({'file': row['file'], 'offset': row['offset'], **check})
                if check['state'] == 'held':
                    action = 'hold_local_checkpoint_' + check['reason']
                else:
                    action = 'propose_ack_after_checkpoint'
                if check['state'] != 'held' and observed.get(row['request_id']) == row:
                    operations.append({'operation': 'acknowledge_after_checkpoint', 'request_id': row['request_id'],
                        'session_id': row['session_id'], 'sequence': row['sequence'],
                        'source': {k: row[k] for k in ('file', 'offset', 'sha256')},
                        'canonical_observation_id': item['observation_id'],
                        'expected_local_checkpoint': row['local_checkpoint'],
                        'checkpoint_candidate': check['candidate'],
                        'checkpoint_session_source': check['session_source'],
                        'requires': ['maintenance_identity_check', 'fresh_receipt_revalidation',
                                     'native_candidate_validation', 'durable_checkpoint', 'explicit_repair_authorization',
                                     *check['unverified_context']]})
                    if nvs is not None:
                        operations[-1]['nvs_session_context'] = check['nvs_session_context']
            else:
                action = 'hold_pending_'+item['state']
        decisions.append({'file': row['file'], 'offset': row['offset'], 'action': action})
    if manifest['authentication'] != 'device_bearer_https':
        blockers.append('loopback_test_evidence_only')
    if not assessment['query_complete']:
        blockers.append('receipt_query_incomplete')
    if not assessment['authenticated_batches']:
        blockers.append('no_authenticated_receipts')
    for check in checkpoint_checks:
        for reason in check['unverified_context']:
            if reason not in blockers:
                blockers.append(reason)
    # Material is read again after the initial receipt verification; retain
    # the independent source pin across these extra file reads as well.
    if nvs is not None:
        try:
            nvs.reverify()
        except outbox_nvs_context.ContextError as error:
            fail(str(error))
    source_manifest(source, source_pin)
    plan = {'format': PLAN_FORMAT, 'source_manifest_sha256': source_pin,
        'evidence_manifest_sha256': evidence_pin, 'expected_actor': manifest['expected_actor'],
        'native_source': selected, 'proposed_operations': operations, 'decisions': decisions,
        'local_checkpoint_checks': checkpoint_checks,
        'checkpoint_material': {name: {k: entry[k] for k in ('category', 'bytes', 'sha256')}
            for name, entry in original['files'].items() if entry['category'] in
            ('session', 'cursor', 'local-session-descriptor', 'opaque-word-descriptor')},
        'blocking_preconditions': blockers, 'installation_ready': False, 'repair_authorized': False,
        'source_bytes_retained': True}
    if nvs is not None:
        plan['format'] = PLAN_NVS_FORMAT
        plan['nvs_context'] = nvs.summary()
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('collect', 'verify', 'plan', 'verify-plan', 'candidate', 'verify-candidate'):
        item = sub.add_parser(command)
        item.add_argument('--source-bundle', type=Path, required=True)
        item.add_argument('--source-manifest-sha256', required=True)
        if command != 'collect':
            item.add_argument('--evidence', type=Path, required=True)
            item.add_argument('--evidence-manifest-sha256', required=True)
        if command in ('collect', 'plan', 'candidate'):
            item.add_argument('--output', type=Path, required=True)
        if command == 'collect':
            item.add_argument('--endpoint', required=True)
            item.add_argument('--device-token-file', type=Path, required=True)
            item.add_argument('--expected-user-id', required=True)
            item.add_argument('--expected-device-id', required=True)
            item.add_argument('--allow-insecure-loopback-test', action='store_true')
            item.add_argument('--timeout-seconds', type=float, default=300)
        if command in ('verify-plan', 'candidate', 'verify-candidate'):
            item.add_argument('--plan', type=Path, required=True)
            item.add_argument('--plan-sha256', required=True)
        if command == 'verify-candidate':
            item.add_argument('--candidate', type=Path, required=True)
            item.add_argument('--candidate-manifest-sha256', required=True)
        if command in ('plan', 'verify-plan', 'candidate', 'verify-candidate'):
            item.add_argument('--nvs-context-inputs', type=Path)
            item.add_argument('--nvs-context-inputs-sha256')
            item.add_argument('--sensitive-main-flash', action='store_true')
    args = parser.parse_args()
    try:
        nvs_options = dict(nvs_inputs=getattr(args, 'nvs_context_inputs', None),
            nvs_inputs_pin=getattr(args, 'nvs_context_inputs_sha256', None),
            sensitive_confirmed=getattr(args, 'sensitive_main_flash', False))
        if args.command == 'collect':
            result = collect(args.source_bundle, args.source_manifest_sha256, args.output, args.endpoint,
                args.device_token_file, {'user_id': args.expected_user_id, 'device_id': args.expected_device_id},
                args.allow_insecure_loopback_test, args.timeout_seconds)
        elif args.command == 'verify':
            _, _, assessment, _ = verify(args.source_bundle, args.source_manifest_sha256,
                args.evidence, args.evidence_manifest_sha256)
            result = {'status': 'verified', 'query_complete': assessment['query_complete'], 'repair_authorized': False}
        elif args.command in ('candidate', 'verify-candidate'):
            import outbox_candidate as native_candidate
            common = (sys.modules[__name__], args.source_bundle, args.source_manifest_sha256,
                args.evidence, args.evidence_manifest_sha256, args.plan, args.plan_sha256)
            if args.command == 'candidate':
                result = native_candidate.generate(*common, args.output, **nvs_options)
            else:
                result = native_candidate.verify(*common, args.candidate, args.candidate_manifest_sha256, **nvs_options)
        else:
            plan = review_plan(args.source_bundle, args.source_manifest_sha256, args.evidence, args.evidence_manifest_sha256,
                **nvs_options, output=args.output if args.command == 'plan' else None)
            if args.command == 'plan':
                if args.output.resolve().is_relative_to(args.source_bundle.resolve()) or \
                        args.output.resolve().is_relative_to(args.evidence.resolve()):
                    fail('OUTPUT_INSIDE_EVIDENCE')
                start_output(args.output)
                pin = finish_output(args.output, 'plan.json', plan)
                result = {'status': 'review_plan_prepared', 'plan_sha256': pin,
                    'proposed_operations': len(plan['proposed_operations']), 'repair_authorized': False}
            else:
                check_pin(args.plan_sha256)
                if args.plan.is_symlink() or set(p.name for p in args.plan.iterdir()) != {'plan.json'}:
                    fail('PLAN_INCOMPLETE')
                saved, raw = read_json(args.plan/'plan.json')
                if f.sha256(raw) != args.plan_sha256 or saved != plan:
                    fail('PLAN_DIFFERS_FROM_EVIDENCE')
                result = {'status': 'review_plan_verified', 'installation_ready': False, 'repair_authorized': False}
    except (OSError, ValueError, KeyError, TypeError, RecursionError, UnicodeError) as error:
        print(json.dumps({'status': 'uncertain' if isinstance(error, f.CaptureCommitUncertain) else 'failed',
            'reason': str(error) if isinstance(error, EvidenceError) else 'LOCAL_IO_OR_VALIDATION_FAILURE',
            'repair_authorized': False}), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 2 if result.get('query_complete') is False else 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--transport-worker']:
        transport_worker()
    else:
        raise SystemExit(main())
