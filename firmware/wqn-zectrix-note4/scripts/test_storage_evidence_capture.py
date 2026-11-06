#!/usr/bin/env python3
"""Native read-only collector + pinned receiver fault scenarios; NOT device HIL."""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

import outbox_forensics as forensic
import storage_evidence_receive as receiver
from test_binary_outbox_recovery import ERR

PINS = ('11' * 16, '22' * 6, '33' * 32)
NVS = r'''#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
using nvs_handle_t=unsigned;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102,ESP_ERR_NVS_TYPE_MISMATCH=0x1103;
enum nvs_type_t {NVS_TYPE_U8=1,NVS_TYPE_I8=17,NVS_TYPE_U16=2,NVS_TYPE_I16=18,
NVS_TYPE_U32=4,NVS_TYPE_I32=20,NVS_TYPE_U64=8,NVS_TYPE_I64=24,NVS_TYPE_STR=33,NVS_TYPE_BLOB=66,NVS_TYPE_ANY=255};
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_find_key(nvs_handle_t,const char*,nvs_type_t*);
#define DECL(name,type) esp_err_t nvs_get_##name(nvs_handle_t,const char*,type*);
DECL(u8,uint8_t) DECL(i8,int8_t) DECL(u16,uint16_t) DECL(i16,int16_t)
DECL(u32,uint32_t) DECL(i32,int32_t) DECL(u64,uint64_t) DECL(i64,int64_t)
esp_err_t nvs_get_str(nvs_handle_t,const char*,char*,size_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
'''
SHA = r'''#pragma once
#include <openssl/evp.h>
void Destination(const void*);
struct mbedtls_sha256_context {EVP_MD_CTX* p=nullptr;};
inline void mbedtls_sha256_init(mbedtls_sha256_context* c){c->p=EVP_MD_CTX_new();}
inline void mbedtls_sha256_free(mbedtls_sha256_context* c){EVP_MD_CTX_free(c->p);}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c,int){return c->p&&EVP_DigestInit_ex(c->p,EVP_sha256(),nullptr)==1?0:-1;}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c,const unsigned char* p,size_t n){return EVP_DigestUpdate(c->p,p,n)==1?0:-1;}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c,unsigned char* p){Destination(p);unsigned n=0;return EVP_DigestFinal_ex(c->p,p,&n)==1&&n==32?0:-1;}
'''


def build(root, directory, mutation):
    headers = {
        'esp_err.h': ERR + '\nconstexpr int ESP_ERR_TIMEOUT=263, ESP_ERR_NOT_SUPPORTED=262;\n',
        'nvs.h': NVS,
        'esp_heap_caps.h': '#pragma once\n#include <cstddef>\nconstexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_SPIRAM=2,MALLOC_CAP_8BIT=4;\nvoid* heap_caps_malloc(size_t,unsigned);void* heap_caps_calloc(size_t,size_t,unsigned);void heap_caps_free(void*);\n',
        'esp_spiffs.h': '#pragma once\nbool esp_spiffs_mounted(const char*);\n',
        'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
        'config.h': '#pragma once\n#define WQN_NVS_NAMESPACE "evidence_test"\n',
        'freertos/FreeRTOS.h': '#pragma once\n',
        'freertos/task.h': '#pragma once\nvoid vTaskDelay(unsigned);\n',
        'mbedtls/sha256.h': SHA,
    }
    for name, text in headers.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    source = (root / 'main/runtime/storage_evidence.cpp').read_text()
    changes = {
        'ignore-mode': ('g_mode.load(std::memory_order_acquire) != Mode::kBootRecovery', 'false'),
        'skip-recheck': ('if (!unchanged) remember(ESP_ERR_INVALID_STATE);', 'unchanged = true; if (!unchanged) remember(ESP_ERR_INVALID_STATE);'),
        'write-open': ('NVS_READONLY', 'NVS_READWRITE'),
    }
    if mutation in changes:
        old, new = changes[mutation]
        expected = 2 if mutation == 'write-open' else 1
        if source.count(old) != expected:
            raise RuntimeError('collector mutation boundary drift')
        source = source.replace(old, new)
    (directory / 'collector.cpp').write_text(source)
    binary = directory / 'native'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
        '-fno-pie', '-no-pie', '-I' + str(directory), '-I' + str(root / 'main'),
        str(directory / 'collector.cpp'), str(root / 'scripts/testdata/storage_evidence.cpp'),
        '-lcrypto', '-Wl,--wrap=open', '-Wl,--wrap=close', '-Wl,--wrap=read',
        '-Wl,--wrap=lstat', '-Wl,--wrap=opendir', '-o', str(binary)], check=True, timeout=60)
    return binary


def native_case(binary, directory, name, save):
    path = directory / name
    path.mkdir()
    storage = path / 'storage'
    storage.mkdir()
    initial = {'wout.v1': b'\xaa' * 200, 'nout.v1': b'\xbb' * 206,
        'wsq.cur': b'\x01' * 52, 'wl-future.bad': b'opaque\x00\xff',
        'wp_manifest.json': b'corrupt manifest bytes', 'wp_test.wqwp': b'PACK\x00\xff',
        'np_manifest.json': b'', 'credential.txt': b'DO_NOT_EXPORT_CREDENTIAL'}
    for filename, data in initial.items():
        (storage / filename).write_bytes(data)
    if name == 'bigfile':
        with open(storage / 'wout.v1', 'wb') as output:
            output.truncate(receiver.MAX_FILE + 1)
    if name == 'many':
        for i in range(513):
            (storage / f'wl-{i}.future').write_bytes(b'x')
    if name in ('symlink', 'fifo'):
        (storage / 'wout.v1').unlink()
        if name == 'symlink':
            (path / 'private').write_bytes(b'DO_NOT_EXPORT_CREDENTIAL')
            (storage / 'wout.v1').symlink_to(path / 'private')
        else:
            os.mkfifo(storage / 'wout.v1')
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in storage.iterdir()
              if p.is_file() and not p.is_symlink()}
    stream = path / 'stream.bin'
    run = subprocess.run([str(binary), str(storage), name, str(stream)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    if run.returncode not in (0, 1) or run.stderr:
        raise RuntimeError('native collector infrastructure failure: ' + run.stderr[-4096:])
    report = json.loads(run.stdout)
    expected_complete = name in ('normal', 'namespace-missing', 'key-missing', 'nvs-empty', 'late-sink-error')
    received = False
    receipt = None
    try:
        receipt = receiver.receive(stream, path / 'received', *PINS)
        received = True
    except ValueError:
        pass
    good = report['passed'] and received == expected_complete
    if not name.startswith('drift-'):
        good = good and hashes == {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in storage.iterdir() if p.is_file() and not p.is_symlink()}
    good = good and b'DO_NOT_EXPORT_CREDENTIAL' not in stream.read_bytes()
    if received:
        manifest = json.loads((path / 'received/manifest.json').read_text())
        good = good and not manifest['repair_authorized'] and not manifest['identity_authenticated']
        good = good and not (path / 'received/.incomplete').exists()
        if name == 'normal':
            good = good and receiver.verify_bundle(path / 'received', receipt['manifest_sha256'])['status'] == 'logical_evidence_verified'
            for filename, data in initial.items():
                if filename != 'credential.txt':
                    good = good and (path / 'received/files' / filename).read_bytes() == data
            good = good and (path / 'received/nvs/cur_seq.42.bin').read_bytes() == b'\xa5' * 65
            good = good and (path / 'received/nvs/word_deck_gen.08.bin').read_bytes() == struct.pack('<Q', (1 << 60) + 7)
            good = good and (path / 'received/nvs/word_deck_chg.21.bin').read_bytes() == b'bad\0tail\0'
            good = good and (path / 'received/nvs/word_seq_idx.18.bin').read_bytes() == struct.pack('<q', -(1 << 60) + 1)
            captured = forensic.capture(path / 'received/files', path / 'forensics', 'word', True)
            verified = forensic.verify(path / 'forensics', captured['manifest_sha256'])
            good = good and not verified['repair_authorized']
            if save is not None:
                save.mkdir(mode=0o700)
                shutil.copy2(stream, save / 'stream.bin')
                (save / 'stream.bin').chmod(0o600)
                shutil.copytree(path / 'received', save / 'received')
                shutil.copytree(path / 'forensics', save / 'forensics')
    else:
        good = good and (path / 'received/.incomplete').exists() and not (path / 'received/manifest.json').exists()
    return bool(good), stream if name == 'normal' else None


def altered(stream, change):
    chunks = list(receiver.frames(stream))
    return change(chunks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('ignore-mode', 'skip-recheck', 'write-open', 'trust-transcript'))
    parser.add_argument('--save-example', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    failures = errors = checks = 0
    with tempfile.TemporaryDirectory(prefix='wqn-storage-evidence-') as temp:
        directory = Path(temp)
        if args.mutation == 'trust-transcript':
            source = (root / 'scripts/storage_evidence_receive.py').read_text()
            old = 'if digest != self.digest.digest():'
            if source.count(old) != 1:
                raise RuntimeError('receiver mutation boundary drift')
            modified = directory / 'receiver_mutated.py'
            modified.write_text(source.replace(old, 'if False:'))
            spec = importlib.util.spec_from_file_location('receiver_mutated', modified)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            global receiver
            receiver = module
        binary = build(root, directory, args.mutation)
        scenarios = ('normal', 'not-mode', 'off-owner', 'unmounted', 'temp-lease', 'pack-lease',
            'deadline', 'deadline-during-yield', 'cancel-during-yield', 'cancel-early', 'cancel-mid', 'sink-error', 'late-sink-error', 'oom',
            'namespace-missing', 'namespace-error', 'key-missing', 'nvs-empty', 'nvs-error',
            'nvs-oversize', 'nvs-unsupported', 'read-error', 'bigfile', 'many', 'symlink', 'fifo',
            'drift-file', 'drift-nvs', 'drift-inventory', 'drift-context')
        normal = None
        for name in scenarios:
            try:
                good, stream = native_case(binary, directory, name, args.save_example if name == 'normal' else None)
                if stream is not None:
                    normal = stream
                checks += 1
                if not good:
                    failures += 1
                print(('PASS' if good else 'FAIL') + ' evidence: ' + name, flush=True)
            except Exception:
                errors += 1
                raise
        chunks = list(receiver.frames(normal))
        variants = {
            'missing-finish': chunks[:-1],
            'duplicate-frame': chunks[:2] + chunks[1:],
            'reordered-frames': [chunks[0], chunks[2], chunks[1]] + chunks[3:],
            'wrong-transcript': [chunks[0][:11+54] + bytes([chunks[0][11+54] ^ 1]) + chunks[0][11+55:]] + chunks[1:],
            'trailing-frame': chunks + [chunks[-1]],
            'truncated-payload': chunks[:-1] + [chunks[-1][:-1]],
        }
        for name, frames in variants.items():
            stream = directory / (name + '.bin')
            stream.write_bytes(b''.join(frames))
            rejected = False
            try:
                receiver.receive(stream, directory / (name + '-out'), *PINS)
            except ValueError:
                rejected = True
            checks += 1
            if not rejected:
                failures += 1
            print(('PASS' if rejected else 'FAIL') + ' evidence: ' + name, flush=True)
        for name, pins in [('wrong-capture', ('44'*16, PINS[1], PINS[2])),
                           ('wrong-mac', (PINS[0], '44'*6, PINS[2])),
                           ('wrong-firmware', (PINS[0], PINS[1], '44'*32))]:
            rejected = False
            try:
                receiver.receive(normal, directory / name, *pins)
            except ValueError:
                rejected = True
            checks += 1
            if not rejected:
                failures += 1
            print(('PASS' if rejected else 'FAIL') + ' evidence: ' + name, flush=True)
        serial = directory / 'serial.log'
        serial.write_bytes(b'ordinary diagnostic log\n' + b''.join(
            b'\x1b[0;32mI (123) evidence: WQN_EVIDENCE ' + base64.b64encode(frame) + b'\x1b[0m\r\n' for frame in chunks))
        serial_result = receiver.receive(serial, directory / 'serial-out', *PINS, serial_log=True)
        checks += 1
        good = serial_result['status'] == 'logical_evidence_received'
        failures += not good
        print(('PASS' if good else 'FAIL') + ' evidence: saved serial envelope', flush=True)
        paths = list((directory / 'normal/received').rglob('*'))
        private = all(stat.S_IMODE(p.stat().st_mode) == (0o700 if p.is_dir() else 0o600) for p in paths)
        checks += 1
        failures += not private
        print(('PASS' if private else 'FAIL') + ' evidence: private output permissions', flush=True)
        original = directory / 'normal/received'
        original_manifest = (original / 'manifest.json').read_bytes()
        original_pin = hashlib.sha256(original_manifest).hexdigest()
        for name in ('modified-file', 'forged-authority', 'uncertain-marker'):
            bundle = directory / name
            shutil.copytree(original, bundle)
            expected_pin = original_pin
            if name == 'modified-file':
                (bundle / 'files/wout.v1').write_bytes(b'changed')
            if name == 'forged-authority':
                manifest = json.loads(original_manifest)
                manifest['repair_authorized'] = True
                data = (json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n').encode()
                (bundle / 'manifest.json').write_bytes(data)
                expected_pin = hashlib.sha256(data).hexdigest()
            if name == 'uncertain-marker':
                (bundle / '.incomplete').write_bytes(b'uncertain')
            rejected = False
            try:
                receiver.verify_bundle(bundle, expected_pin)
            except ValueError:
                rejected = True
            checks += 1
            failures += not rejected
            print(('PASS' if rejected else 'FAIL') + ' evidence: independent verify ' + name, flush=True)
        uncertain = directory / 'finalization-uncertain'
        sync = receiver.sync_dir
        root_syncs = 0
        def fail_final_sync(path):
            nonlocal root_syncs
            if Path(path) == uncertain:
                root_syncs += 1
                if root_syncs == 2:
                    raise OSError('injected final directory sync failure')
            return sync(path)
        failed = False
        with patch.object(receiver, 'sync_dir', fail_final_sync):
            try:
                receiver.receive(normal, uncertain, *PINS)
            except OSError:
                failed = True
        checks += 1
        good = failed and (uncertain / '.incomplete').exists() and (uncertain / 'partial.json').exists()
        failures += not good
        print(('PASS' if good else 'FAIL') + ' evidence: uncertain host finalization remains incomplete', flush=True)
        for name in ('symlink-input', 'fifo-input', 'symlink-wire', 'fifo-wire'):
            target = directory / name
            if name.endswith('input'):
                if name.startswith('symlink'):
                    target.symlink_to(normal)
                else:
                    os.mkfifo(target)
                operation = lambda: receiver.receive(target, directory / (name + '-out'), *PINS)
            else:
                shutil.copytree(original, target)
                (target / 'wire.bin').unlink()
                if name.startswith('symlink'):
                    (target / 'wire.bin').symlink_to(normal)
                else:
                    os.mkfifo(target / 'wire.bin')
                operation = lambda: receiver.verify_bundle(target, original_pin)
            rejected = False
            try:
                operation()
            except (ValueError, OSError):
                rejected = True
            checks += 1
            failures += not rejected
            print(('PASS' if rejected else 'FAIL') + ' evidence: reject ' + name, flush=True)
    print(f'Storage evidence: {checks} scenarios, {failures} failures, {errors} errors (host ports; NOT HIL)', flush=True)
    return 1 if failures or errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
