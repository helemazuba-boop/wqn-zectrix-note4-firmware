#!/usr/bin/env python3
"""Receive/verify saved WQR1 main-Flash evidence. No device connection or repair.

Raw images may contain credentials. Size-matched, rechecked main Flash is still
not an eFuse/key backup, an atomic pre-boot snapshot or authenticated identity.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile

import storage_evidence_receive as wire

BEGIN = struct.Struct('<16s6s32siIIIBBB')
FINISH = struct.Struct('<IiII32s32s32sB')
MAX_IMAGE = 64 * 1024 * 1024
MARKER = b'raw main-flash evidence not finalized\n'


def encode(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()


class Receiver:
    def __init__(self, output, capture_id, mac, firmware, sensitive_confirmed):
        if not sensitive_confirmed:
            raise ValueError('explicit sensitive main-Flash confirmation required')
        self.expected = wire.pin(capture_id, 16), wire.pin(mac, 6), wire.pin(firmware, 32)
        self.output = Path(output)
        self.output.mkdir(mode=0o700)
        wire.private(self.output/'.incomplete', MARKER)
        self.handles = []
        try:
            for name in ('wire.bin', 'main-flash.bin'):
                self.handles.append(os.fdopen(os.open(self.output/name,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), 'wb'))
        except OSError:
            self.close()
            raise
        self.sequence = self.total = 0
        self.identity = None
        self.size = self.physical = 0
        self.encrypted = False
        self.ended = self.complete = False
        self.transcript = hashlib.sha256()
        self.wire_hash = hashlib.sha256()
        self.image_hash = hashlib.sha256()
        self.finish = None

    def feed(self, raw):
        if self.ended or len(raw) < wire.HEADER.size:
            raise ValueError('unexpected raw frame after finish or short header')
        magic, kind, size, sequence = wire.HEADER.unpack_from(raw)
        if magic != b'WQR1' or size > 512 or len(raw) != wire.HEADER.size+size or sequence != self.sequence:
            raise ValueError('raw frame/version/size/sequence mismatch')
        if self.sequence == 0 and kind != 1:
            raise ValueError('raw identity must come first')
        self.handles[0].write(raw)
        self.wire_hash.update(raw)
        if kind != 5:
            self.transcript.update(raw)
        self.sequence += 1
        body = raw[wire.HEADER.size:]
        if kind == 1:
            if self.identity is not None or len(body) != BEGIN.size:
                raise ValueError('invalid raw identity frame')
            capture, mac, firmware, error, generation, configured, physical, encrypted, confirmed, semantics = BEGIN.unpack(body)
            if (capture, mac, firmware) != self.expected:
                raise ValueError('raw identity does not match operator pins')
            if configured == 0 or configured > MAX_IMAGE or physical != configured:
                raise ValueError('raw image does not cover the reported physical main Flash')
            if encrypted not in (0, 1) or confirmed != 1 or semantics != 1:
                raise ValueError('invalid raw-byte semantics or confirmation claim')
            self.identity = dict(capture_id=capture.hex(), mac=mac.hex(), firmware_elf_sha256=firmware.hex(),
                                 boot_error=error, observed_generation=generation)
            self.size, self.physical, self.encrypted = configured, physical, bool(encrypted)
        elif kind == 3:
            if self.identity is None or not 4 < len(body) <= 388:
                raise ValueError('invalid raw data frame')
            offset, = struct.unpack_from('<I', body)
            data = body[4:]
            if offset != self.total or len(data) > self.size-self.total:
                raise ValueError('noncontiguous or excessive raw image data')
            self.handles[1].write(data)
            self.image_hash.update(data)
            self.total += len(data)
        elif kind == 5:
            if self.identity is None or len(body) != FINISH.size:
                raise ValueError('invalid raw finish frame')
            captured, error, failed, rechecked, image, reread, digest, consistent = FINISH.unpack(body)
            if captured != self.total or rechecked > self.size or consistent not in (0, 1):
                raise ValueError('invalid raw source totals')
            if digest != self.transcript.digest() or image != self.image_hash.digest():
                raise ValueError('raw transcript or image digest mismatch')
            if captured < self.size and rechecked != 0:
                raise ValueError('partial first pass cannot have a second pass')
            if rechecked == 0 and reread != hashlib.sha256().digest():
                raise ValueError('invalid empty recheck digest')
            if failed != 0xffffffff and (failed >= self.size or failed != (captured if captured < self.size else rechecked)):
                raise ValueError('invalid raw failure offset')
            complete = error == 0 and consistent == 1
            if complete and (captured != self.size or rechecked != self.size or image != reread or failed != 0xffffffff):
                raise ValueError('unverified raw image declared complete')
            if (error == 0) != bool(consistent):
                raise ValueError('inconsistent raw source result')
            self.complete = complete
            self.finish = dict(source_error=error, failed_offset=None if failed == 0xffffffff else failed,
                rechecked_bytes=rechecked, source_recheck_reported=bool(consistent),
                recheck_sha256=reread.hex(), transcript_sha256=digest.hex())
            self.ended = True
        else:
            raise ValueError('unknown raw frame kind')

    def commit(self):
        if not self.ended:
            raise ValueError('missing raw source finish')
        for handle in self.handles:
            handle.flush()
            os.fsync(handle.fileno())
        self.close()
        manifest = dict(format='wqn-raw-main-flash-evidence-v1', raw_image_complete=self.complete,
            scope='size-matched SDK main Flash; raw ciphertext where encrypted',
            configured_bytes=self.size, physical_bytes=self.physical, data_bytes=self.total,
            flash_encryption_enabled=self.encrypted, sensitive_main_flash_requested=True,
            identity=self.identity, frames=self.sequence, image_sha256=self.image_hash.hexdigest(),
            wire_sha256=self.wire_hash.hexdigest(), **self.finish,
            complete_device_backup=False, pre_boot_snapshot=False, atomic_snapshot_proven=False,
            identity_authenticated=False, transport_delivery_acknowledged=False,
            installation_ready=False, repair_authorized=False)
        encoded = encode(manifest)
        wire.private(self.output/'manifest.json', encoded)
        wire.sync_dir(self.output)
        if self.complete:
            try:
                (self.output/'.incomplete').unlink()
                wire.sync_dir(self.output)
            except OSError:
                if not (self.output/'.incomplete').exists():
                    wire.private(self.output/'.incomplete', MARKER)
                raise
        return dict(status='raw_flash_image_received' if self.complete else 'raw_flash_image_incomplete',
            raw_image_complete=self.complete, manifest_sha256=hashlib.sha256(encoded).hexdigest(),
            identity_authenticated=False, repair_authorized=False, installation_ready=False)

    def close(self):
        for handle in self.handles:
            handle.close()
        self.handles = []

    def checkpoint(self):
        for handle in self.handles:
            handle.flush()
            os.fsync(handle.fileno())
        wire.sync_dir(self.output)
        wire.sync_dir(self.output.parent)

    def partial(self, error):
        try:
            wire.private(self.output/'partial.json', encode(dict(
                format='wqn-partial-raw-main-flash-evidence-v1', raw_image_complete=False,
                identity=self.identity, received_frames=self.sequence, data_bytes=self.total,
                prefix_sha256=self.image_hash.hexdigest(), reason=str(error),
                identity_authenticated=False, repair_authorized=False)))
        except OSError:
            pass


def receive(source, output, capture_id, mac, firmware, serial_log=False, sensitive_confirmed=False):
    receiver = Receiver(output, capture_id, mac, firmware, sensitive_confirmed)
    try:
        for frame in wire.frames(source, serial_log):
            receiver.feed(frame)
        return receiver.commit()
    except (ValueError, OSError) as error:
        receiver.partial(error)
        raise
    finally:
        receiver.close()


def same_bytes(a, b, maximum):
    handles = []
    try:
        for path in (a, b):
            handle = os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb')
            handles.append(handle)
        before = [os.fstat(h.fileno()) for h in handles]
        if any(not stat.S_ISREG(s.st_mode) or s.st_size > maximum for s in before):
            raise ValueError('invalid raw image artifact type or size')
        if before[0].st_size != before[1].st_size:
            raise ValueError('raw image size differs from independent replay')
        remaining = before[0].st_size
        while remaining:
            count = min(65536, remaining)
            left, right = (h.read(count) for h in handles)
            if len(left) != count or left != right:
                raise ValueError('raw image bytes differ from independent replay')
            remaining -= count
        for original, handle in zip(before, handles):
            after = os.fstat(handle.fileno())
            if (original.st_dev, original.st_ino, original.st_size, original.st_mtime_ns) != (
                    after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
                raise ValueError('raw image changed during verification')
    finally:
        for handle in handles:
            handle.close()


def verify_bundle(bundle, expected_manifest_sha256, sensitive_confirmed=False):
    if not sensitive_confirmed:
        raise ValueError('explicit sensitive main-Flash confirmation required')
    bundle = Path(bundle)
    wire.pin(expected_manifest_sha256, 32)
    if bundle.is_symlink() or not bundle.is_dir():
        raise ValueError('invalid raw evidence directory')
    raw = wire.read_bound(bundle/'manifest.json', 65536)
    if hashlib.sha256(raw).hexdigest() != expected_manifest_sha256.lower():
        raise ValueError('raw manifest pin mismatch')
    manifest = json.loads(raw)
    names = {'manifest.json', 'wire.bin', 'main-flash.bin'}
    if not manifest['raw_image_complete']:
        names.add('.incomplete')
        if wire.read_bound(bundle/'.incomplete', 256) != MARKER:
            raise ValueError('invalid incomplete raw evidence marker')
    if {p.name for p in bundle.iterdir()} != names:
        raise ValueError('unexpected or uncertain raw evidence inventory')
    identity = manifest['identity']
    with tempfile.TemporaryDirectory(prefix='wqn-raw-verify-') as temp:
        rebuilt = Path(temp)/'rebuilt'
        receive(bundle/'wire.bin', rebuilt, identity['capture_id'], identity['mac'],
                identity['firmware_elf_sha256'], sensitive_confirmed=True)
        if wire.read_bound(rebuilt/'manifest.json', 65536) != raw:
            raise ValueError('raw manifest differs from independent wire replay')
        same_bytes(bundle/'main-flash.bin', rebuilt/'main-flash.bin', MAX_IMAGE)
        same_bytes(bundle/'wire.bin', rebuilt/'wire.bin', wire.MAX_INPUT)
    if wire.read_bound(bundle/'manifest.json', 65536) != raw:
        raise ValueError('raw manifest changed during verification')
    return dict(status='raw_flash_image_verified' if manifest['raw_image_complete'] else 'raw_flash_incomplete_verified',
        raw_image_complete=manifest['raw_image_complete'], manifest_sha256=expected_manifest_sha256.lower(),
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
    for command in (take, verify):
        command.add_argument('--sensitive-main-flash', action='store_true',
            help='confirm handling of potentially credential-bearing main-Flash bytes')
    args = parser.parse_args()
    try:
        result = receive(args.input, args.output, args.capture_id, args.mac, args.firmware_elf_sha256,
                         args.serial_log, args.sensitive_main_flash) if args.command == 'receive' else \
                 verify_bundle(args.bundle, args.expected_manifest_sha256, args.sensitive_main_flash)
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps(dict(status='incomplete', reason=str(error), repair_authorized=False)), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0 if result['raw_image_complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
