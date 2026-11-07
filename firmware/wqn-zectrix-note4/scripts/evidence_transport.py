"""Bounded WQNT1 session. ACKs follow fsync; final C follows independent replay.

The line IO is injectable. No port is opened by importing this module.
"""
import base64
from collections import OrderedDict
import hashlib
import json
from pathlib import Path
import re
import time

import storage_evidence_receive as logical
import raw_flash_evidence_receive as raw


def private_json(path, value):
    logical.private(path, (json.dumps(value, sort_keys=True, separators=(',', ':'))+'\n').encode())


class Session:
    def __init__(self, output, mac, firmware, kind, sensitive_confirmed=False, clock=time.monotonic):
        if kind not in ('R', 'L') or (kind == 'R' and not sensitive_confirmed):
            raise ValueError('raw main-Flash capture requires explicit sensitive confirmation')
        self.mac, self.firmware = logical.pin(mac, 6).hex(), logical.pin(firmware, 32).hex()
        self.kind, self.sensitive, self.clock = kind, sensitive_confirmed, clock
        self.output = Path(output); self.output.mkdir(mode=0o700)
        logical.private(self.output/'.transport-incomplete', b'transport confirmation pending\n')
        logical.sync_dir(self.output); logical.sync_dir(self.output.parent)
        self.capture = None; self.receiver = None; self.evidence = None
        self.next = 0; self.cache = OrderedDict(); self.pending = []
        self.last_ack = None; self.last_boundary = None
        self.committed = False; self.sealed = False; self.complete = False; self.confirmed = False
        self.manifest = None; self.known_offers = set(); self.received_bytes = 0
        self.discarded_frames = 0
        self.last_activity = self.clock()

    def _ack(self, final):
        self.receiver.checkpoint()  # Bytes and directory entries precede ACK.
        digest = hashlib.sha256(b''.join(self.pending)).hexdigest()
        sequence = self.next-1
        if final:
            receipt = self.receiver.commit()
            self.sealed = True
            self.manifest = receipt['manifest_sha256']
            if self.kind == 'R':
                verified = raw.verify_bundle(self.evidence, self.manifest, True)
                self.complete = verified['raw_image_complete']
            else:
                logical.verify_bundle(self.evidence, self.manifest)
                self.complete = True
            private_json(self.output/('delivery-'+self.capture+'.json'), dict(
                protocol='WQNT1', capture_id=self.capture, kind=self.kind, mac=self.mac,
                firmware_elf_sha256=self.firmware, last_sequence=sequence, window_sha256=digest,
                manifest_sha256=self.manifest, evidence=self.evidence.name,
                host_commit_verified=True, device_received_commit_ack=False,
                source_complete=self.complete, identity_authenticated=False, repair_authorized=False))
            logical.sync_dir(self.output)
            self.committed = True
        self.last_ack = (f'WQNT1 {"C" if final else "A"} {self.capture} {sequence:08x} {digest}'
                         + (f' {self.manifest}' if final else '')+'\n').encode()
        self.last_boundary = sequence
        self.pending = []
        return self.last_ack

    def _discard(self):
        self.discarded_frames += 1
        if self.discarded_frames > 256:
            raise ValueError('corrupt/missing frame retry budget exhausted')
        return None

    def line(self, line):
        if len(line) > 1024:
            raise ValueError('transport line budget exceeded')
        self.received_bytes += len(line)
        if self.received_bytes > 512*1024*1024:
            raise ValueError('transport input budget exceeded')
        at = line.find(b'WQNT1 ')
        if at < 0:
            return None
        try:
            fields = line[at:].strip().decode('ascii').split(' ')
        except UnicodeDecodeError:
            return self._discard()
        if fields[:2] == ['WQNT1', 'O']:
            if len(fields) != 6 or fields[3:6] != [self.mac, self.firmware, self.kind]:
                raise ValueError('offered device/firmware/kind does not match operator pins')
            logical.pin(fields[2], 16)
            capture = fields[2]
            if capture == self.capture:
                return f'WQNT1 R {capture}\n'.encode() if self.next == 0 else None
            if self.next != 0 or self.committed:
                raise ValueError('device replaced an active evidence capture')
            if len(self.known_offers) >= 4 or capture in self.known_offers:
                raise ValueError('too many or replayed capture offers')
            if self.receiver is not None:
                self.receiver.close()  # Preserve the old empty attempt, never overwrite it.
            self.known_offers.add(capture); self.capture = capture
            self.evidence = self.output/('evidence-'+capture)
            self.receiver = raw.Receiver(self.evidence, capture, self.mac, self.firmware, True) if self.kind == 'R' else \
                logical.Receiver(self.evidence, capture, self.mac, self.firmware)
            self.receiver.checkpoint()
            self.last_activity = self.clock()
            return f'WQNT1 R {capture}\n'.encode()
        if fields[:2] == ['WQNT1', 'D']:
            if self.capture is None:
                return None  # An old queued prefix cannot start a new session.
            if len(fields) < 3 or fields[2] != self.capture:
                raise ValueError('data belongs to another capture')
            if len(fields) != 7:
                return self._discard()
            if not re.fullmatch('[0-9a-f]{8}', fields[3]) or not re.fullmatch('[0-9a-f]{4}', fields[4]) or \
                    not re.fullmatch('[0-9a-f]{64}', fields[5]):
                return self._discard()
            sequence, size = int(fields[3], 16), int(fields[4], 16)
            if size < 11 or size > 523:
                return self._discard()
            try:
                frame = base64.b64decode(fields[6], validate=True)
            except ValueError:
                return self._discard()
            if len(frame) != size or hashlib.sha256(frame).hexdigest() != fields[5]:
                return self._discard()
            if sequence < self.next:
                if self.cache.get(sequence) != fields[5]:
                    raise ValueError('replayed frame differs from persisted bytes')
                return self.last_ack if sequence == self.last_boundary else None
            if self.committed:
                raise ValueError('data after committed finish')
            if sequence != self.next:
                return self._discard()  # Sender retains and retries the entire unacknowledged window.
            if frame[:4] != (b'WQR1' if self.kind == 'R' else b'WQE1') or int.from_bytes(frame[7:11], 'little') != sequence:
                raise ValueError('inner and outer frame identity disagree')
            self.receiver.feed(frame)
            self.cache[sequence] = fields[5]
            while len(self.cache) > 16: self.cache.popitem(last=False)
            self.pending.append(frame); self.next += 1; self.last_activity = self.clock()
            if len(self.pending) == 16 or frame[4] == 5:
                return self._ack(frame[4] == 5)
            return None
        if fields[:2] == ['WQNT1', 'E'] and len(fields) == 4 and fields[2] == self.capture:
            raise ValueError('device stopped capture with source/transport error '+fields[3])
        if fields[:2] == ['WQNT1', 'F']:
            if len(fields) != 4 or not self.committed or fields[2:] != [self.capture, self.manifest]:
                raise ValueError('finish does not bind the committed manifest')
            private_json(self.output/('delivery-confirmed-'+self.capture+'.json'), dict(
                protocol='WQNT1', capture_id=self.capture, manifest_sha256=self.manifest,
                device_received_commit_ack=True, source_complete=self.complete,
                identity_authenticated=False, repair_authorized=False))
            logical.sync_dir(self.output)
            try:
                (self.output/'.transport-incomplete').unlink(); logical.sync_dir(self.output)
            except OSError:
                if not (self.output/'.transport-incomplete').exists():
                    logical.private(self.output/'.transport-incomplete', b'transport finalization uncertain\n')
                raise
            self.confirmed = True
        return None

    def run(self, io, timeout=1800, idle_timeout=180):
        started = self.clock()
        try:
            while not self.confirmed:
                now = self.clock()
                if now-started >= timeout or now-self.last_activity >= idle_timeout:
                    if self.committed: break
                    raise TimeoutError('evidence transport deadline exceeded')
                line = io.read_line(0.05)
                if line is None: continue
                if line == b'':
                    if self.committed: break
                    raise OSError('evidence transport closed before commit')
                answer = self.line(line)
                if answer is not None: io.write(answer)
            return dict(status=('evidence_saved' if self.confirmed else 'evidence_saved_device_ack_unknown'),
                evidence=str(self.evidence), manifest_sha256=self.manifest, source_complete=self.complete,
                device_received_commit_ack=self.confirmed, identity_authenticated=False,
                repair_authorized=False, installation_ready=False)
        except BaseException as error:
            if self.receiver is not None and not self.sealed:
                if self.kind == 'R': self.receiver.partial(str(error))
                else: self.receiver.preserve_partial(str(error))
            raise
        finally:
            if self.receiver is not None: self.receiver.close()
