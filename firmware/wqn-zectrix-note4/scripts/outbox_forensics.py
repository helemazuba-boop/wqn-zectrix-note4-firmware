#!/usr/bin/env python3
"""Preserve an offline Word/Note export and inspect journals without repairing them.

This never connects to a device, database or model and never changes the source.
Capture commits a private bundle only after all copies are durable. Incomplete
and uncertain finalization are explicit. Full-length corrupt records are opaque;
neither their identity nor a later ACK may be used to guess away missing data.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import zlib

LEGACY_FORMAT = 'wqn-outbox-forensics-v1'
FORMAT = 'wqn-outbox-forensics-v2'
LEGACY_PROFILE = 'word1000-note1000-v1'
CURRENT_PROFILE = 'word8192-note1000-v1'
STORAGE_PROFILES = {
    LEGACY_PROFILE: {'word': 1000, 'note': 1000, 'analysis_records': 8192},
    CURRENT_PROFILE: {'word': 8192, 'note': 1000, 'analysis_records': 16448},
}
MAX_RECORDS = 16448
MAX_RECORD_DETAILS = 256
MAX_DESCRIPTOR_FILES = 4096
MAX_MANIFEST_BYTES = 8 * 1024 * 1024
CHUNK_BYTES = 65536
REQUEST_ID = re.compile(r'[A-Za-z0-9_-]{16,64}\Z', re.ASCII)
UUID = re.compile(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z', re.ASCII)
DESCRIPTOR_NAME = re.compile(r'(wl|we)([0-9a-fA-F]{24})\.(v1|tmp|bak)\Z', re.ASCII)


@dataclass(frozen=True)
class Layout:
    prefix: str
    magic: int
    record: struct.Struct
    request_offset: int
    mode_max: int
    action_max: int


LAYOUTS = {
    'word': Layout('wout', 0x424F5157, struct.Struct('<IHBBBBHQI65s37s37s33sI'), 24, 6, 5),
    'note': Layout('nout', 0x424F4E51, struct.Struct('<IHBBBBQQI65s37s37s33sI'), 30, 1, 3),
}
ERRORS = {'ok': 0, 'invalid-state': 259, 'invalid-size': 260,
          'invalid-response': 264, 'invalid-crc': 265}


class CaptureCommitUncertain(ValueError):
    pass


def journal_crc(data):
    # Firmware wraps esp_rom_crc32_le(UINT32_MAX, ...) with XOR UINT32_MAX.
    # The ROM API already complements its input/output. This is not the
    # default zlib.crc32(data) convention. SDK C implementation is a test oracle.
    return (zlib.crc32(data, 0xFFFFFFFF) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def source_names(domain, observed_names=()):
    layout = LAYOUTS[domain]
    names = {f'{layout.prefix}.{suffix}': 'journal' for suffix in ('v1', 'rec', 'bak', 'tmp')}
    stems = ('wsq', 'wsr', 'wsd', 'wsv', 'wsi', 'wsh', 'wsm') if domain == 'word' else ('nses',)
    for stem in stems:
        for suffix in ('v1', 'tmp', 'bak'):
            names[f'{stem}.{suffix}'] = 'session'
        if domain == 'word':
            for suffix in ('cur', 'ctp', 'cbk'):
                names[f'{stem}.{suffix}'] = 'cursor'
    # Preserve forensic reject records too; they are never upload candidates.
    for suffix in ('v1', 'tmp', 'bak'):
        names[f'{"wrej" if domain == "word" else "nrej"}.{suffix}'] = 'rejected-journal'
    # Discover flat descriptors and personal cache families. Cache discovery is
    # conditional on observed files so older v1/v2 manifest pins stay valid.
    # Preserve malformed/future names as opaque evidence, never replay proof.
    discovered = set()
    for name in observed_names:
        if domain != 'word' or not isinstance(name, str) or not name.startswith(('wl', 'we')):
            continue
        if '/' in name or '\\' in name or '\0' in name:
            raise ValueError('unsafe word descriptor path')
        discovered.add(name)
        if len(discovered) > MAX_DESCRIPTOR_FILES:
            raise ValueError('descriptor inventory exceeds capture bound; preserve the raw image')
    for name in sorted(discovered):
        match = DESCRIPTOR_NAME.fullmatch(name)
        if name in ('welig.v1', 'welig.tmp', 'welig.bak'):
            for suffix in ('v1', 'tmp', 'bak'):
                names[f'welig.{suffix}'] = 'personal-eligibility-cache'
        elif match:
            for suffix in ('v1', 'tmp', 'bak'):
                names[f'{match[1]}{match[2]}.{suffix}'] = (
                    'local-session-descriptor' if match[1] == 'wl' else 'personal-eligibility-cache')
        else:
            names[name] = 'opaque-word-descriptor' if name.startswith('wl') else 'opaque-word-cache'
    return names


def fixed_text(field):
    head, separator, tail = field.partition(b'\0')
    if not separator or any(tail):
        return None
    try:
        return head.decode('utf-8', errors='strict')
    except UnicodeError:
        return None


def decode_record(raw, domain, offset, include_checkpoint=True):
    layout = LAYOUTS[domain]
    entry = {'offset': offset, 'bytes': len(raw), 'sha256': sha256(raw)}
    if len(raw) != layout.record.size:
        return dict(entry, integrity='partial-tail'), None, 'ok'
    magic, version, kind, action, mode, extra, *fields = layout.record.unpack(raw)
    if magic != layout.magic or version != 1:
        return dict(entry, integrity='unsupported-layout'), None, 'invalid-crc'
    if fields[-1] != journal_crc(raw[:-4]):
        return dict(entry, integrity='crc-mismatch'), None, 'invalid-crc'
    at = layout.request_offset
    request, session, item, occurred = (raw[at:at+65], raw[at+65:at+102],
                                      raw[at+102:at+139], raw[at+139:at+172])
    if request[-1] != 0:
        return dict(entry, integrity='unterminated-request'), None, 'invalid-crc'
    if kind not in (1, 2, 3):
        return dict(entry, integrity='unsupported-kind'), None, 'invalid-response'
    if kind == 1 and (session[-1] != 0 or item[-1] != 0 or occurred[-1] != 0 or
                      mode > layout.mode_max or action > layout.action_max or
                      (domain == 'word' and extra > 1)):
        return dict(entry, integrity='unsupported-observation'), None, 'invalid-response'
    entry.update(integrity='valid', kind={1: 'observation', 2: 'ack', 3: 'suspend'}[kind])
    request_text = fixed_text(request)
    usable = request_text is not None and REQUEST_ID.fullmatch(request_text) is not None
    if usable:
        entry['request_id'] = request_text
    if kind == 1:
        session_text, item_text, occurred_text = map(fixed_text, (session, item, occurred))
        sequence = struct.unpack_from('<Q', raw, 12 if domain == 'word' else 10)[0]
        usable = bool(usable and session_text and UUID.fullmatch(session_text) and
                      item_text and UUID.fullmatch(item_text) and occurred_text and
                      sequence <= 9007199254740991)
        if usable:
            entry.update(session_id=session_text, item_id=item_text, sequence=sequence,
                         action=action, mode=mode, occurred_at=occurred_text)
            if include_checkpoint:
                # Keep local continuation evidence separate from canonical
                # cloud fields. A receipt cannot reconstruct this checkpoint,
                # and exposing it does not authorize resuming a session.
                checkpoint = {'next_position': struct.unpack_from('<I', raw, 20 if domain == 'word' else 26)[0]}
                if domain == 'word':
                    checkpoint['next_phase'] = extra
                else:
                    checkpoint['next_sequence'] = struct.unpack_from('<Q', raw, 18)[0]
                entry['local_checkpoint'] = checkpoint
    if kind == 3:
        entry['suspend_reason'] = struct.unpack_from('<H', raw, 10)[0] if domain == 'word' else extra
    entry['identity_usable_for_lookup'] = bool(usable)
    return entry, (kind, request.split(b'\0', 1)[0], raw), 'ok'


def analyze(path, domain, limit=None, profile=CURRENT_PROFILE):
    """Native prefix behavior plus independent later-block inspection, never salvage."""
    if profile not in STORAGE_PROFILES:
        raise ValueError('unsupported storage profile')
    storage = STORAGE_PROFILES[profile]
    if limit is None:
        limit = storage['analysis_records']
    if not 0 < limit <= MAX_RECORDS:
        raise ValueError('analysis record limit outside supported bounds')
    layout = LAYOUTS[domain]
    entries = []
    pending, acknowledged, suspended = {}, {}, {}
    total = ack_count = suspend_count = orphan_ack = orphan_suspend = prefix = 0
    error = 'ok'
    partial = False
    complete = True
    inspected = 0
    integrity_counts = Counter()
    first_fault = None
    with open_regular(path) as source:
        while True:
            raw = source.read(layout.record.size)
            if not raw:
                break
            if inspected == limit:
                complete = False
                break
            offset = inspected * layout.record.size
            entry, decoded, record_error = decode_record(raw, domain, offset,
                include_checkpoint=profile != LEGACY_PROFILE)
            inspected += 1
            integrity_counts[entry['integrity']] += 1
            if len(entries) < MAX_RECORD_DETAILS:
                entries.append(entry)
            if len(raw) != layout.record.size:
                partial = True
                break
            if error != 'ok':
                continue  # Useful forensic bytes; no authoritative replay past a fault.
            if record_error != 'ok':
                # Native scanner increments after CRC/terminator checks but
                # before dispatching a kind or validating observation enums.
                if record_error == 'invalid-response':
                    total += 1
                error = record_error
                first_fault = offset
                continue
            kind, key, original = decoded
            total += 1
            if kind == 1:
                if key in pending and pending[key] != original:
                    error = 'invalid-state'
                    first_fault = offset
                    continue
                pending.setdefault(key, original)
            elif kind == 2:
                ack_count += 1
                if key in pending:
                    acknowledged.setdefault(key, pending.pop(key))
                elif key not in acknowledged:
                    orphan_ack += 1
            else:
                suspend_count += 1
                if key in pending:
                    suspended.setdefault(key, pending.pop(key))
                elif key not in suspended:
                    orphan_suspend += 1
            prefix += 1
    if complete and error == 'ok' and len(pending) + len(suspended) > storage[domain]:
        error = 'invalid-size'
    return {'record_bytes': layout.record.size, 'analysis_complete': complete,
            'inspected_records': inspected, 'record_details_complete': inspected == len(entries),
            'integrity_counts': dict(integrity_counts), 'first_fault_offset': first_fault,
            'native_error': error if complete else 'not-fully-analyzed',
            'native_error_code': ERRORS[error] if complete else None,
            'validated_prefix_records': prefix,
            'prefix_counts': {'total_records': total, 'pending': len(pending),
                'acknowledged': len(acknowledged), 'suspended': len(suspended),
                'ack_records': ack_count, 'suspend_records': suspend_count,
                'orphan_ack_records': orphan_ack, 'orphan_suspend_records': orphan_suspend},
            'partial_tail': partial, 'records': entries,
            'repair_authorized': False}


def open_regular(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ValueError('input must be a regular file')
        return os.fdopen(descriptor, 'rb')
    except BaseException:
        os.close(descriptor)
        raise


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def copy_preserved(source, target):
    digest, length = hashlib.sha256(), 0
    with open_regular(source) as incoming:
        before = os.fstat(incoming.fileno())
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'wb') as outgoing:
            while block := incoming.read(CHUNK_BYTES):
                outgoing.write(block)
                digest.update(block)
                length += len(block)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        if identity(before) != identity(os.fstat(incoming.fileno())) or \
                identity(before) != identity(os.stat(source, follow_symlinks=False)) or length != before.st_size:
            raise ValueError('source changed during capture; keep the bundle incomplete')
    return {'bytes': length, 'sha256': digest.hexdigest()}


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_private(path, data):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'wb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def capture(source, output, domain, offline, flash_image=None):
    if not offline:
        raise ValueError('capture requires an offline immutable export')
    if source.is_symlink() or not source.is_dir():
        raise ValueError('source must be an offline export directory')
    if output.resolve().is_relative_to(source.resolve()):
        raise ValueError('output must be outside the source export')
    output.mkdir(mode=0o700)  # Exclusive: never overwrite another capture.
    write_private(output/'.incomplete', b'capture has not committed\n')
    directory_before = identity(os.stat(source, follow_symlinks=False))
    known = source_names(domain, (path.name for path in source.iterdir()))
    original_files = {}
    manifest = {'format': FORMAT, 'domain': domain, 'offline_export_declared': True,
                'storage_profile': CURRENT_PROFILE,
                'complete_device_backup': False, 'repair_authorized': False,
                'runtime_candidate_order': [f'{LAYOUTS[domain].prefix}.{suffix}' for suffix in ('v1', 'rec', 'bak')],
                'files': {}, 'missing': [], 'raw_image_supplied_unverified': flash_image is not None}
    for name, category in known.items():
        path = source/name
        if not path.exists() and not path.is_symlink():
            manifest['missing'].append(name)
            continue
        original_files[name] = identity(os.stat(path, follow_symlinks=False))
        entry = copy_preserved(path, output/name)
        entry['category'] = category
        if category in ('journal', 'rejected-journal'):
            entry['analysis'] = analyze(output/name, domain)
        manifest['files'][name] = entry
    if flash_image is not None:
        manifest['files']['raw-image.bin'] = dict(copy_preserved(flash_image, output/'raw-image.bin'), category='raw-image')
    if not manifest['files']:
        raise ValueError('no known exported files or raw image found')
    # A descriptor arriving/disappearing after discovery cannot yield a
    # successful incomplete inventory, even if all individual copies settled.
    if directory_before != identity(os.stat(source, follow_symlinks=False)) or \
            any(before != identity(os.stat(source/name, follow_symlinks=False))
                for name, before in original_files.items()):
        raise ValueError('source inventory changed during capture; keep the bundle incomplete')
    payload = (json.dumps(manifest, ensure_ascii=True, indent=2) + '\n').encode('utf-8')
    if len(payload) > MAX_MANIFEST_BYTES:
        raise ValueError('analysis manifest too large; raw copies remain incomplete')
    write_private(output/'manifest.json', payload)
    sync_directory(output)
    try:
        (output/'.incomplete').unlink()
        sync_directory(output)
    except OSError as error:
        # Unlink or directory sync can take effect before returning an error.
        # Restore the local guard when possible, but do not promise it survives
        # power loss after another I/O error. Source files were never changed.
        try:
            write_private(output/'.incomplete', b'capture finalization uncertain\n')
        except OSError:
            pass
        raise CaptureCommitUncertain('capture finalization uncertain; retain source and recapture into a new directory') from error
    return {'status': 'preserved', 'manifest_sha256': sha256(payload),
            'files': len(manifest['files']), 'repair_authorized': False}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate manifest key')
        result[key] = value
    return result


def reject_nonfinite(_value):
    raise ValueError('non-finite JSON value')


def verify(bundle, expected_manifest=None):
    if bundle.is_symlink() or not bundle.is_dir() or \
            (bundle/'.incomplete').exists() or (bundle/'.incomplete').is_symlink():
        raise ValueError('bundle incomplete or unavailable')
    with open_regular(bundle/'manifest.json') as source:
        payload = source.read(MAX_MANIFEST_BYTES + 1)
    if len(payload) > MAX_MANIFEST_BYTES:
        raise ValueError('manifest too large')
    digest = sha256(payload)
    if expected_manifest is not None and digest != expected_manifest:
        raise ValueError('manifest digest differs from the independently pinned digest')
    manifest = json.loads(payload, object_pairs_hook=unique_object, parse_constant=reject_nonfinite)
    keys = {'format', 'domain', 'offline_export_declared', 'complete_device_backup',
            'repair_authorized', 'runtime_candidate_order', 'files', 'missing', 'raw_image_supplied_unverified'}
    if not isinstance(manifest, dict) or manifest.get('domain') not in LAYOUTS:
        raise ValueError('unsupported bundle format')
    if manifest.get('format') == LEGACY_FORMAT and set(manifest) == keys:
        profile = LEGACY_PROFILE
    elif manifest.get('format') == FORMAT and set(manifest) == keys | {'storage_profile'} and \
            manifest.get('storage_profile') == CURRENT_PROFILE:
        profile = CURRENT_PROFILE
    else:
        raise ValueError('unsupported bundle format/storage profile')
    domain = manifest['domain']
    if manifest['runtime_candidate_order'] != [f'{LAYOUTS[domain].prefix}.{suffix}' for suffix in ('v1', 'rec', 'bak')]:
        raise ValueError('invalid runtime source precedence; temporary files are not candidates')
    if not isinstance(manifest.get('files'), dict) or not manifest['files']:
        raise ValueError('invalid bundle inventory')
    known = source_names(domain, manifest['files'] if profile == CURRENT_PROFILE else ())
    if manifest.get('repair_authorized') is not False or manifest.get('complete_device_backup') is not False or \
            manifest.get('offline_export_declared') is not True:
        raise ValueError('forensic capture cannot authorize repair or claim full backup')
    missing = manifest.get('missing')
    if not isinstance(missing, list) or any(not isinstance(name, str) for name in missing) or \
            sorted(missing) != sorted(set(known) - set(manifest['files'])) or len(missing) != len(set(missing)):
        raise ValueError('invalid missing-file inventory')
    if manifest['raw_image_supplied_unverified'] is not ('raw-image.bin' in manifest['files']):
        raise ValueError('inconsistent raw image inventory')
    if set(path.name for path in bundle.iterdir()) != set(manifest['files']) | {'manifest.json'}:
        raise ValueError('unlisted files in capture')
    for name, expected in manifest['files'].items():
        category = 'raw-image' if name == 'raw-image.bin' else known.get(name)
        if category is None or not isinstance(expected, dict) or expected.get('category') != category:
            raise ValueError('unexpected capture path/category')
        entry_keys = {'bytes', 'sha256', 'category'}
        if category in ('journal', 'rejected-journal'):
            entry_keys.add('analysis')
        if set(expected) != entry_keys:
            raise ValueError('unsupported preserved-file metadata')
        if type(expected.get('bytes')) is not int or expected['bytes'] < 0 or \
                not isinstance(expected.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', expected['sha256']):
            raise ValueError('invalid preserved length/digest')
        actual, size = hashlib.sha256(), 0
        with open_regular(bundle/name) as source:
            while data := source.read(CHUNK_BYTES):
                actual.update(data)
                size += len(data)
        if actual.hexdigest() != expected.get('sha256') or size != expected.get('bytes'):
            raise ValueError('preserved bytes changed')
        if category in ('journal', 'rejected-journal') and analyze(bundle/name, domain, profile=profile) != expected.get('analysis'):
            raise ValueError('journal analysis differs from preserved bytes')
    return {'status': 'verified', 'manifest_sha256': digest,
            'integrity': 'pinned-digest' if expected_manifest else 'self-consistent',
            'files': len(manifest['files']), 'repair_authorized': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    take = commands.add_parser('capture', help='preserve an offline export; no device writes')
    take.add_argument('--domain', choices=tuple(LAYOUTS), required=True)
    take.add_argument('--source-dir', type=Path, required=True)
    take.add_argument('--output', type=Path, required=True)
    take.add_argument('--offline', action='store_true')
    take.add_argument('--flash-image', type=Path)
    check = commands.add_parser('verify', help='verify pinned bytes and re-run analysis')
    check.add_argument('--bundle', type=Path, required=True)
    check.add_argument('--expected-manifest-sha256')
    args = parser.parse_args()
    try:
        result = capture(args.source_dir, args.output, args.domain, args.offline, args.flash_image) \
            if args.command == 'capture' else verify(args.bundle, args.expected_manifest_sha256)
    except (OSError, ValueError, TypeError, KeyError, RecursionError) as error:
        print(json.dumps({'status': 'uncertain' if isinstance(error, CaptureCommitUncertain) else 'failed',
                          'reason': str(error), 'repair_authorized': False}), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
