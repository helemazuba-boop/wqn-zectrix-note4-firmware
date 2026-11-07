#!/usr/bin/env python3
"""Production raw reader, SDK faults, saved-wire receive and independent replay. NOT HIL."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
from unittest.mock import patch

import raw_flash_evidence_receive as receiver
from test_storage_evidence_capture import SHA
from test_binary_outbox_recovery import ERR

PINS = ('11'*16, '22'*6, '33'*32)
SCENARIOS = ('normal', 'encrypted', 'one-byte', 'board-16m', 'republish', 'not-mode', 'off-owner', 'temp-lease',
    'pack-lease', 'no-consent', 'empty-capture', 'empty-mac', 'empty-firmware', 'no-sink',
    'invalid-deadline', 'expired', 'cancel-early', 'size-error', 'physical-error', 'size-mismatch',
    'zero-size', 'too-big', 'read-error', 'recheck-error', 'drift-data', 'size-drift', 'physical-drift',
    'encryption-drift', 'sink-error', 'cancel-mid', 'deadline-mid', 'slow-read', 'cancel-in-read',
    'late-sink-error', 'late-sink-deadline', 'size-error-tail', 'physical-error-tail')


def build(root, directory, mutation):
    headers = {
        'esp_err.h': ERR+'\nconstexpr int ESP_ERR_TIMEOUT=263;\n',
        'esp_flash.h': '#pragma once\n#include <cstdint>\n#include "esp_err.h"\nstruct esp_flash_t {};\nesp_err_t esp_flash_get_size(esp_flash_t*,uint32_t*);\nesp_err_t esp_flash_get_physical_size(esp_flash_t*,uint32_t*);\nesp_err_t esp_flash_read(esp_flash_t*,void*,uint32_t,uint32_t);\n',
        'esp_flash_encrypt.h': '#pragma once\nbool esp_flash_encryption_enabled();\n',
        'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
        'freertos/FreeRTOS.h': '#pragma once\n',
        'freertos/task.h': '#pragma once\nvoid vTaskDelay(unsigned);\n',
        'mbedtls/sha256.h': SHA,
    }
    for name, text in headers.items():
        p = directory/name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    source = (root/'main/runtime/raw_flash_evidence.cpp').read_text()
    mutations = {
        'ignore-consent': ('!request.sensitive_main_flash_confirmed', 'false'),
        'ignore-size': ('size != summary->physical_bytes', 'false'),
        'skip-recheck': ('summary->image_sha256 != summary->recheck_sha256', 'false'),
    }
    if mutation:
        old, new = mutations[mutation]
        if source.count(old) != 1:
            raise RuntimeError('raw collector mutation boundary drift')
        source = source.replace(old, new)
    (directory/'raw.cpp').write_text(source)
    binary = directory/'native'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
        '-fno-pie', '-no-pie', '-I'+str(directory), '-I'+str(root/'main'),
        str(directory/'raw.cpp'), str(root/'main/runtime/storage_evidence_mode.cpp'),
        str(root/'scripts/testdata/raw_flash_evidence.cpp'), '-lcrypto', '-o', str(binary)],
        check=True, timeout=60)
    return binary


def expected(size):
    raw = bytearray(bytes((17*i+29)&255 for i in range(256)) * ((size+255)//256))
    del raw[size:]
    marker = b'SYNTHETIC_MAIN_FLASH_CREDENTIAL_REGION'
    if size >= 4096+len(marker):
        raw[4096:4096+len(marker)] = marker
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('ignore-consent', 'ignore-size', 'skip-recheck'))
    parser.add_argument('--save-example', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    checks = failures = 0
    def check(good, label):
        nonlocal checks, failures
        checks += 1
        failures += not good
        print(('PASS raw-flash: ' if good else 'FAIL raw-flash: ')+label, flush=True)
    with tempfile.TemporaryDirectory(prefix='wqn-raw-evidence-') as temp:
        directory = Path(temp)
        binary = build(root, directory, args.mutation)
        normal = None
        for name in SCENARIOS:
            case = directory/name
            case.mkdir()
            stream = case/'input.bin'
            run = subprocess.run([str(binary), name, str(stream)], capture_output=True, text=True, timeout=60)
            if run.returncode not in (0, 1) or run.stderr:
                raise RuntimeError('raw native infrastructure failure: '+run.stderr[-4096:])
            native = json.loads(run.stdout)
            check(native['passed'], 'native '+name)
            before = hashlib.sha256(stream.read_bytes()).hexdigest()
            receipt = None
            try:
                receipt = receiver.receive(stream, case/'bundle', *PINS, sensitive_confirmed=True)
            except ValueError:
                pass
            complete = name in ('normal', 'encrypted', 'one-byte', 'board-16m', 'republish', 'late-sink-error', 'late-sink-deadline')
            check((receipt is not None and receipt['raw_image_complete']) == complete, 'source result '+name)
            if receipt is not None:
                manifest = json.loads((case/'bundle/manifest.json').read_text())
                check(not any(manifest[k] for k in ('complete_device_backup', 'identity_authenticated',
                    'transport_delivery_acknowledged', 'repair_authorized', 'installation_ready', 'atomic_snapshot_proven')),
                    'no inferred authority '+name)
                raw = (case/'bundle/main-flash.bin').read_bytes()
                size = 16*1024*1024 if name == 'board-16m' else 1 if name == 'one-byte' else 8193
                check(raw == expected(size)[:len(raw)], 'preserve raw bytes '+name)
                verified = receiver.verify_bundle(case/'bundle', receipt['manifest_sha256'], True)
                check(verified['raw_image_complete'] == complete, 'independent replay '+name)
            else:
                check((case/'bundle/.incomplete').exists(), 'retain incomplete '+name)
            check(hashlib.sha256(stream.read_bytes()).hexdigest() == before, 'source immutable '+name)
            if name == 'normal':
                normal = stream
                check(stat.S_IMODE((case/'bundle').stat().st_mode) == 0o700 and
                      all(stat.S_IMODE(p.stat().st_mode) == 0o600 for p in (case/'bundle').iterdir()), 'private bundle permissions')
                if args.save_example is not None:
                    shutil.copytree(case/'bundle', args.save_example)
        def rejected(name, source=None, pins=PINS, consent=True, serial=False):
            output = directory/('rejected-'+name)
            try:
                receiver.receive(source or normal, output, *pins, serial_log=serial, sensitive_confirmed=consent)
            except (ValueError, OSError):
                check(True, 'reject '+name)
            else:
                check(False, 'reject '+name)
        rejected('host-no-consent', consent=False)
        check(not (directory/'rejected-host-no-consent').exists(), 'no output before consent')
        for index in range(3):
            pins = list(PINS); pins[index] = '44'*(len(pins[index])//2)
            rejected('wrong-pin-'+str(index), pins=pins)
        original = normal.read_bytes()
        for name, changed in (('short', original[:-1]), ('extra', original+original[:11]),
            ('bad-magic', b'WQE1'+original[4:]), ('bad-sequence', original[:7]+b'\x01\0\0\0'+original[11:]),
            ('bad-size', original[:5]+b'\xff\xff'+original[7:]),
            ('bad-digest', original[:-10]+bytes([original[-10]^1])+original[-9:])):
            path = directory/(name+'.bin'); path.write_bytes(changed); rejected(name, path)
        serial = directory/'serial.log'
        serial.write_bytes(b'ordinary log\n'+b''.join(b'WQN_EVIDENCE '+base64.b64encode(frame)+b'\n'
            for frame in receiver.wire.frames(normal)))
        receipt = receiver.receive(serial, directory/'serial-received', *PINS, True, True)
        check(receipt['raw_image_complete'], 'saved serial log roundtrip')
        for name in ('image', 'wire', 'manifest-authority'):
            bundle = directory/('forged-'+name); shutil.copytree(directory/'normal/bundle', bundle)
            pin = json.loads((directory/'normal/bundle/manifest.json').read_text())
            if name == 'manifest-authority':
                pin['identity_authenticated'] = True
                (bundle/'manifest.json').write_bytes(receiver.encode(pin))
            else:
                path = bundle/('main-flash.bin' if name == 'image' else 'wire.bin')
                data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
            repin = hashlib.sha256((bundle/'manifest.json').read_bytes()).hexdigest()
            try:
                receiver.verify_bundle(bundle, repin, True)
            except ValueError:
                check(True, 'independent verifier rejects forged '+name)
            else:
                check(False, 'independent verifier rejects forged '+name)
        symlink = directory/'link.bin'; symlink.symlink_to(normal); rejected('symlink-source', symlink)
        fifo = directory/'fifo'; os.mkfifo(fifo); rejected('fifo-source', fifo)
        for name, artifact in (('image-symlink', 'main-flash.bin'), ('wire-symlink', 'wire.bin')):
            bundle = directory/name; shutil.copytree(directory/'normal/bundle', bundle)
            (bundle/artifact).unlink(); (bundle/artifact).symlink_to(directory/'normal/bundle'/artifact)
            pin = hashlib.sha256((bundle/'manifest.json').read_bytes()).hexdigest()
            try:
                receiver.verify_bundle(bundle, pin, True)
            except (ValueError, OSError): check(True, 'reject '+name)
            else: check(False, 'reject '+name)
        forged = directory/'forged-incomplete'; shutil.copytree(directory/'read-error/bundle', forged)
        manifest = json.loads((forged/'manifest.json').read_text()); manifest['raw_image_complete'] = True
        (forged/'manifest.json').write_bytes(receiver.encode(manifest)); (forged/'.incomplete').unlink()
        pin = hashlib.sha256((forged/'manifest.json').read_bytes()).hexdigest()
        try:
            receiver.verify_bundle(forged, pin, True)
        except ValueError: check(True, 'partial source cannot be promoted by repinning a manifest')
        else: check(False, 'partial source cannot be promoted by repinning a manifest')
        changed = directory/'changed.bin'; changed.write_bytes(original)
        feed = receiver.Receiver.feed
        def change_input(instance, frame):
            if instance.sequence == 0:
                stamp = changed.stat(); os.utime(changed, ns=(stamp.st_atime_ns, stamp.st_mtime_ns+1000000))
            return feed(instance, frame)
        with patch.object(receiver.Receiver, 'feed', change_input): rejected('changing-input', changed)
        calls = 0
        real_sync = receiver.wire.sync_dir
        def fail_final(path):
            nonlocal calls
            calls += 1
            if calls == 2: raise OSError('injected final directory sync failure')
            return real_sync(path)
        with patch.object(receiver.wire, 'sync_dir', fail_final):
            rejected('finalization', normal)
        check((directory/'rejected-finalization/.incomplete').exists(), 'uncertain finalization keeps marker')
        pin = hashlib.sha256((directory/'rejected-finalization/manifest.json').read_bytes()).hexdigest()
        try:
            receiver.verify_bundle(directory/'rejected-finalization', pin, True)
        except ValueError: check(True, 'uncertain finalization cannot verify complete')
        else: check(False, 'uncertain finalization cannot verify complete')
    print(f'Raw Flash evidence: {checks} scenarios, {failures} failures, 0 errors (host SDK ports; NOT HIL)', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
