#!/usr/bin/env python3
"""Receive one operator-pinned logical evidence stream; never contacts a device.

Input is concatenated binary WQE1 frames or saved WQN_EVIDENCE base64 log lines.
The format is an integrity envelope, not authentication or repair permission.
Incomplete/source-error input retains private objects and its incomplete marker.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import tempfile

import outbox_forensics as forensic

HEADER = struct.Struct('<4sBHI')
ENTITY = struct.Struct('<HBBBiQB')
FINISH = struct.Struct('<HQi32sBB')
FIXED = set(forensic.source_names('word')) | set(forensic.source_names('note'))
KEYS = {'cur_seq', 'cur_rnd', 'cur_dic', 'cur_rev', 'cur_int', 'cur_shf',
        'cur_mis', 'word_deck_gen', 'word_deck_chg', 'word_deck', 'word_seq_idx'}
SCALAR_BYTES = {1: 1, 17: 1, 2: 2, 18: 2, 4: 4, 20: 4, 8: 8, 24: 8}
MAX_FILE = 16 * 1024 * 1024
MAX_TOTAL = 32 * 1024 * 1024
MAX_NVS = 65536
MAX_ENTITIES = 512
MAX_INPUT = 160 * 1024 * 1024
ANSI_COLOR = re.compile(rb'\x1b\[[0-9;]*m')


def pin(text, count):
    if not isinstance(text, str) or not re.fullmatch('[0-9a-fA-F]{' + str(count * 2) + '}', text):
        raise ValueError('invalid operator identity pin')
    value = bytes.fromhex(text)
    if not any(value):
        raise ValueError('empty operator identity pin')
    return value


def selected(name):
    return (os.fsdecode(name) in FIXED or name.startswith((b'wl', b'we', b'wp_', b'np_')))


def private(path, data):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'wb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def sync_dir(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def frames(source, serial_log=False):
    total = 0
    descriptor = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > MAX_INPUT:
            raise ValueError('evidence input must be a bounded saved regular file')
        if serial_log:
            while True:
                line = stream.readline(4097)
                if not line:
                    break
                total += len(line)
                if len(line) > 4096 or total > MAX_INPUT:
                    raise ValueError('serial input budget exceeded')
                line = ANSI_COLOR.sub(b'', line)
                if b'WQN_EVIDENCE ' not in line:
                    continue
                data = line.split(b'WQN_EVIDENCE ', 1)[1].strip()
                try:
                    raw = base64.b64decode(data, validate=True)
                except ValueError as error:
                    raise ValueError('invalid evidence base64') from error
                if len(raw) < HEADER.size:
                    raise ValueError('short evidence frame')
                yield raw
        else:
            while True:
                header = stream.read(HEADER.size)
                if not header:
                    break
                if len(header) != HEADER.size:
                    raise ValueError('short evidence header')
                _, _, size, _ = HEADER.unpack(header)
                if size > 512:
                    raise ValueError('oversize evidence frame')
                data = stream.read(size)
                if len(data) != size:
                    raise ValueError('short evidence payload')
                total += HEADER.size + size
                if total > MAX_INPUT:
                    raise ValueError('binary input budget exceeded')
                yield header + data
        after = os.fstat(stream.fileno())
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
                after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise ValueError('evidence input changed during receive')


class Receiver:
    def __init__(self, output, capture_id, mac, firmware):
        self.output = Path(output)
        self.expected = pin(capture_id, 16), pin(mac, 6), pin(firmware, 32)
        self.output.mkdir(mode=0o700)
        private(self.output / '.incomplete', b'evidence receive not committed\n')
        for name in ('objects', 'files', 'nvs'):
            (self.output / name).mkdir(mode=0o700)
        self.sequence = 0
        self.digest = hashlib.sha256()
        self.entities = []
        self.active = None
        self.total = 0
        self.ended = False
        self.identity = None
        self.finish_digest = None
        self.names = set()
        self.wire_hash = hashlib.sha256()
        self.wire = os.fdopen(os.open(self.output / 'wire.bin',
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), 'wb')

    def feed(self, raw):
        if self.ended or len(raw) < HEADER.size:
            raise ValueError('unexpected evidence after finish or short frame')
        magic, kind, size, sequence = HEADER.unpack_from(raw)
        if magic != b'WQE1' or size > 512 or len(raw) != HEADER.size + size or sequence != self.sequence:
            raise ValueError('evidence frame/version/sequence mismatch')
        body = raw[HEADER.size:]
        if self.sequence == 0 and kind != 1:
            raise ValueError('evidence identity must come first')
        self.wire.write(raw)
        self.wire_hash.update(raw)
        if kind != 5:
            self.digest.update(raw)
        self.sequence += 1
        if kind == 1:
            if self.identity is not None or len(body) != 72:
                raise ValueError('invalid or repeated evidence identity')
            if (body[:16], body[16:22], body[22:54]) != self.expected:
                raise ValueError('operator identity pin mismatch')
            boot_error, observed, cached, ready, namespace_error, namespace_found = struct.unpack_from('<iIIBiB', body, 54)
            if ready not in (0, 1) or namespace_found not in (0, 1):
                raise ValueError('invalid context flags')
            self.identity = dict(capture_id=body[:16].hex(), mac=body[16:22].hex(),
                firmware_elf_sha256=body[22:54].hex(), boot_error=boot_error,
                observed_schema_generation=observed, cached_scope_generation=cached,
                scope_ready=bool(ready), namespace_error=namespace_error,
                namespace_found=bool(namespace_found))
        elif kind == 2:
            if self.active is not None or len(body) < ENTITY.size or len(self.entities) >= MAX_ENTITIES:
                raise ValueError('invalid entity boundary')
            number, domain, presence, value_type, error, declared, name_len = ENTITY.unpack_from(body)
            name = body[ENTITY.size:]
            if number != len(self.entities) + 1 or len(name) != name_len or not 0 < name_len < 64:
                raise ValueError('entity identity or name mismatch')
            if domain not in (0, 1) or presence not in (0, 1, 2) or b'/' in name or b'\\' in name or b'\0' in name:
                raise ValueError('unsafe evidence name or domain')
            if (domain, name) in self.names:
                raise ValueError('duplicate evidence name')
            self.names.add((domain, name))
            if domain == 0:
                if value_type != 0 or not selected(name):
                    raise ValueError('file outside evidence allowlist')
                maximum = MAX_FILE
            else:
                if name not in {key.encode() for key in KEYS}:
                    raise ValueError('NVS key outside evidence allowlist')
                maximum = MAX_NVS
                if error == 0 and presence == 1:
                    if value_type in SCALAR_BYTES:
                        if declared != SCALAR_BYTES[value_type]:
                            raise ValueError('invalid scalar width')
                    elif value_type not in (0x21, 0x42):
                        raise ValueError('unsupported complete NVS type')
            if error == 0 and (declared > maximum or presence == 2 or
                              (presence == 0 and (declared != 0 or value_type != 0))):
                raise ValueError('invalid complete entity size or absence')
            item = dict(id=number, domain=domain, name_hex=name.hex(), presence=presence,
                        nvs_type=value_type, error=error, declared_bytes=declared, captured_bytes=0)
            item['hash'] = hashlib.sha256()
            item['path'] = self.output / 'objects' / f'{number:04d}.bin'
            item['handle'] = open(os.open(item['path'], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), 'wb')
            self.entities.append(item)
            self.active = item
        elif kind == 3:
            if self.active is None or len(body) <= 10 or len(body) > 394:
                raise ValueError('data without entity or invalid chunk')
            number, offset = struct.unpack_from('<HQ', body)
            item = self.active
            data = body[10:]
            if number != item['id'] or offset != item['captured_bytes'] or item['error'] != 0 or item['presence'] != 1:
                raise ValueError('data identity/offset mismatch')
            if offset + len(data) > item['declared_bytes'] or self.total + len(data) > MAX_TOTAL:
                raise ValueError('evidence byte budget exceeded')
            item['handle'].write(data)
            item['hash'].update(data)
            item['captured_bytes'] += len(data)
            self.total += len(data)
        elif kind == 4:
            if self.active is None or len(body) != 46:
                raise ValueError('invalid entity end')
            number, captured, digest, error = struct.unpack('<HQ32si', body)
            item = self.active
            if number != item['id'] or captured != item['captured_bytes'] or digest != item['hash'].digest():
                raise ValueError('entity byte digest mismatch')
            if item['error'] != 0 and error != item['error']:
                raise ValueError('entity error changed')
            item['error'] = error
            if error == 0 and captured != item['declared_bytes']:
                raise ValueError('incomplete entity declared complete')
            item['sha256'] = digest.hex()
            item['handle'].flush()
            os.fsync(item['handle'].fileno())
            item['handle'].close()
            del item['handle'], item['hash']
            self.active = None
        elif kind == 5:
            if self.active is not None or len(body) != FINISH.size:
                raise ValueError('invalid evidence finish')
            count, total, error, digest, unchanged, complete = FINISH.unpack(body)
            if count != len(self.entities) or total != self.total:
                raise ValueError('evidence totals mismatch')
            if digest != self.digest.digest():
                raise ValueError('evidence transcript digest mismatch')
            expected = {(0, os.fsencode(name)) for name in FIXED} | {(1, key.encode()) for key in KEYS}
            if not expected <= self.names:
                raise ValueError('evidence inventory missing required entries')
            nvs = [item for item in self.entities if item['domain'] == 1]
            if (error != 0 or unchanged != 1 or complete != 1 or any(item['error'] for item in self.entities)
                    or self.identity['namespace_error'] != 0 or
                    (not self.identity['namespace_found'] and any(item['presence'] != 0 for item in nvs))):
                raise ValueError('source reports incomplete or changing evidence')
            self.finish_digest = digest.hex()
            self.ended = True
        else:
            raise ValueError('unknown evidence frame kind')

    def commit(self):
        if not self.ended:
            raise ValueError('missing complete evidence finish')
        self.wire.flush()
        os.fsync(self.wire.fileno())
        self.wire.close()
        self.wire = None
        rows = []
        for item in self.entities:
            row = {key: value for key, value in item.items() if key != 'path'}
            if item['presence'] == 1:
                name = bytes.fromhex(item['name_hex'])
                if item['domain'] == 0:
                    target = os.fsencode(self.output / 'files') + b'/' + name
                else:
                    target = os.fsencode(self.output / 'nvs') + b'/' + name + f'.{item["nvs_type"]:02x}.bin'.encode()
                os.rename(item['path'], target)
                item['path'] = Path(os.fsdecode(target))
            else:
                item['path'].unlink()
            rows.append(row)
        manifest = dict(format='wqn-logical-storage-evidence-v1', capture_complete=True,
            complete_device_backup=False, identity_authenticated=False,
            identity_binding='operator-pinned capture ID, MAC and firmware ELF hash; not remote attestation',
            repair_authorized=False, installation_ready=False, identity=self.identity,
            transcript_sha256=self.finish_digest, frames=self.sequence,
            wire_sha256=self.wire_hash.hexdigest(), entities=rows, data_bytes=self.total)
        encoded = (json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n').encode()
        private(self.output / 'manifest.json', encoded)
        for name in ('files', 'nvs', 'objects'):
            sync_dir(self.output / name)
        sync_dir(self.output)
        try:
            (self.output / '.incomplete').unlink()
            sync_dir(self.output)
        except OSError:
            if not (self.output / '.incomplete').exists():
                private(self.output / '.incomplete', b'evidence finalization uncertain\n')
            raise
        return dict(status='logical_evidence_received', manifest_sha256=hashlib.sha256(encoded).hexdigest(),
                    identity_authenticated=False, repair_authorized=False, installation_ready=False)

    def close(self):
        if self.wire is not None:
            self.wire.close()
            self.wire = None
        for item in self.entities:
            handle = item.pop('handle', None)
            if handle is not None:
                handle.close()

    def preserve_partial(self, error):
        rows = []
        for item in self.entities:
            row = {key: value for key, value in item.items() if key not in ('path', 'handle', 'hash')}
            if 'hash' in item:
                row['partial_sha256'] = item['hash'].hexdigest()
            row['object_path_hex'] = os.fsencode(item['path'].relative_to(self.output)).hex()
            rows.append(row)
        try:
            private(self.output / 'partial.json', (json.dumps(dict(format='wqn-partial-logical-evidence-v1',
                capture_complete=False, identity_authenticated=False, repair_authorized=False,
                reason=str(error), identity=self.identity, received_frames=self.sequence,
                data_bytes=self.total, entities=rows), sort_keys=True) + '\n').encode())
        except OSError:
            pass  # Keep the incomplete marker and objects if diagnostic write also fails.


def receive(source, output, capture_id, mac, firmware, serial_log=False):
    receiver = Receiver(output, capture_id, mac, firmware)
    try:
        for frame in frames(source, serial_log):
            receiver.feed(frame)
        return receiver.commit()
    except (ValueError, OSError) as error:
        receiver.preserve_partial(error)
        raise
    finally:
        receiver.close()


def read_bound(path, maximum):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > maximum:
            raise ValueError('invalid evidence artifact size or type')
        data = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
        if len(data) > maximum or (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
                after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise ValueError('artifact changed during verification')
        return data


def verify_bundle(bundle, expected_manifest_sha256):
    bundle = Path(bundle)
    pin(expected_manifest_sha256, 32)
    if bundle.is_symlink() or not bundle.is_dir() or {p.name for p in bundle.iterdir()} != {
            'manifest.json', 'wire.bin', 'files', 'nvs', 'objects'}:
        raise ValueError('incomplete evidence or unexpected artifact inventory')
    raw = read_bound(bundle / 'manifest.json', 1024 * 1024)
    if hashlib.sha256(raw).hexdigest() != expected_manifest_sha256.lower():
        raise ValueError('evidence manifest pin mismatch')
    manifest = json.loads(raw)
    identity = manifest['identity']
    # Replaying the retained wire independently recomputes every digest, context,
    # inventory and authorization flag, rather than trusting manifest claims.
    with tempfile.TemporaryDirectory(prefix='wqn-evidence-verify-') as temp:
        rebuilt = Path(temp) / 'rebuilt'
        receive(bundle / 'wire.bin', rebuilt, identity['capture_id'], identity['mac'], identity['firmware_elf_sha256'])
        if read_bound(rebuilt / 'manifest.json', 1024 * 1024) != raw:
            raise ValueError('evidence manifest differs from independent replay')
        for name in ('files', 'nvs', 'objects'):
            actual, expected = bundle / name, rebuilt / name
            if actual.is_symlink() or not actual.is_dir():
                raise ValueError('invalid evidence directory')
            entries = {os.fsencode(p.name) for p in actual.iterdir()}
            if entries != {os.fsencode(p.name) for p in expected.iterdir()}:
                raise ValueError('evidence object inventory mismatch')
            for item in entries:
                a, b = os.fsencode(actual) + b'/' + item, os.fsencode(expected) + b'/' + item
                maximum = MAX_NVS if name == 'nvs' else MAX_FILE
                if read_bound(a, maximum) != read_bound(b, maximum):
                    raise ValueError('evidence bytes differ from independent replay')
        wire = read_bound(bundle / 'wire.bin', MAX_INPUT)
        if hashlib.sha256(wire).hexdigest() != manifest['wire_sha256']:
            raise ValueError('evidence wire changed during verification')
    if (bundle / '.incomplete').exists():
        raise ValueError('evidence finalization uncertain')
    if read_bound(bundle / 'manifest.json', 1024 * 1024) != raw:
        raise ValueError('evidence manifest changed during verification')
    return dict(status='logical_evidence_verified', manifest_sha256=expected_manifest_sha256.lower(),
                identity_authenticated=False, repair_authorized=False, installation_ready=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    take = commands.add_parser('receive')
    take.add_argument('--input', required=True, type=Path)
    take.add_argument('--output', required=True, type=Path)
    take.add_argument('--capture-id', required=True)
    take.add_argument('--mac', required=True, help='12 hexadecimal digits')
    take.add_argument('--firmware-elf-sha256', required=True)
    take.add_argument('--serial-log', action='store_true')
    verify = commands.add_parser('verify')
    verify.add_argument('--bundle', required=True, type=Path)
    verify.add_argument('--expected-manifest-sha256', required=True)
    args = parser.parse_args()
    try:
        result = receive(args.input, args.output, args.capture_id, args.mac, args.firmware_elf_sha256, args.serial_log) \
            if args.command == 'receive' else verify_bundle(args.bundle, args.expected_manifest_sha256)
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps(dict(status='incomplete', reason=str(error), repair_authorized=False)), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
