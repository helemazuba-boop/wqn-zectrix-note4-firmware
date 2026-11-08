#!/usr/bin/env python3
"""Actual export worker/transport/raw collector over subprocess pipes; faults, not HIL."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from unittest.mock import patch

from capture_recovery_evidence import ProcessLines
from evidence_transport import Session
import raw_flash_evidence_receive as raw
from test_binary_outbox_recovery import ERR
from test_storage_evidence_capture import SHA, build as build_logical, native_case
import storage_evidence_receive as logical

PINS = ('11'*16, '22'*6, '33'*32)


def build(root, directory, mutation):
    headers = {
        'esp_err.h': ERR+'\nconstexpr int ESP_ERR_TIMEOUT=263, ESP_ERR_NOT_SUPPORTED=262;\n',
        'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
        'esp_flash.h': '#pragma once\n#include <cstdint>\n#include "esp_err.h"\nstruct esp_flash_t {};\nesp_err_t esp_flash_get_size(esp_flash_t*,uint32_t*);\nesp_err_t esp_flash_get_physical_size(esp_flash_t*,uint32_t*);\nesp_err_t esp_flash_read(esp_flash_t*,void*,uint32_t,uint32_t);\n',
        'esp_flash_encrypt.h': '#pragma once\nbool esp_flash_encryption_enabled();\n',
        'esp_heap_caps.h': '#pragma once\n#include <cstddef>\nconstexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=4;\nvoid* heap_caps_malloc(size_t,unsigned);void heap_caps_free(void*);\n',
        'esp_mac.h': '#pragma once\n#include <cstdint>\n#include "esp_err.h"\nconstexpr int ESP_MAC_WIFI_STA=0;\nesp_err_t esp_read_mac(uint8_t*,int);\n',
        'esp_random.h': '#pragma once\n#include <cstddef>\nvoid esp_fill_random(void*,size_t);\n',
        'esp_app_desc.h': '#pragma once\n#include <cstdint>\nstruct esp_app_desc_t {uint8_t app_elf_sha256[32];};\nconst esp_app_desc_t* esp_app_get_description();\n',
        'freertos/FreeRTOS.h': '#pragma once\n',
        'freertos/task.h': '#pragma once\nvoid vTaskDelay(unsigned);\n',
        'mbedtls/sha256.h': SHA+'\ninline int mbedtls_sha256(const unsigned char* p,size_t n,unsigned char* out,int){unsigned size=0;return EVP_Digest(p,n,out,&size,EVP_sha256(),nullptr)==1&&size==32?0:-1;}\n',
        'mbedtls/base64.h': '#pragma once\n#include <openssl/evp.h>\ninline int mbedtls_base64_encode(unsigned char* out,size_t max,size_t* length,const unsigned char* data,size_t size){if(max<4*((size+2)/3)+1)return -1;*length=EVP_EncodeBlock(out,data,size);return 0;}\n',
    }
    for name, content in headers.items():
        path = directory/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content)
    source = (root/'main/runtime/evidence_transfer.cpp').read_text()
    if mutation and mutation != 'allow-manual-logical':
        old, new = {
            'trust-ack': ('std::strcmp(line_, expected) == 0) {', 'std::strncmp(line_, "WQNT1 A ", 8) == 0) {'),
            'allow-nul': ("if (c == '\\0') overflow = true;", 'if (false) overflow = true;'),
            'skip-ready': ('if (std::strcmp(line_, expected) == 0) return result_.error = ESP_OK;',
                           'return result_.error = ESP_OK;'),
        }[mutation]
        if source.count(old) != 1: raise RuntimeError('transport mutation boundary drift')
        source = source.replace(old, new)
        # keep the expected variable used when weakening READY in the control
        if mutation == 'skip-ready': source = source.replace('return result_.error = ESP_OK;\n        }', '(void)expected; return result_.error = ESP_OK;\n        }', 1)
    (directory/'transfer.cpp').write_text(source)
    worker = (root/'main/services/storage_recovery_export.cpp').read_text()
    if mutation == 'allow-manual-logical':
        old = 'boot.preservation_only && kind != StorageRecoveryExportKind::kRaw'
        if worker.count(old) != 1: raise RuntimeError('preservation worker mutation drift')
        worker = worker.replace(old, 'false')
    (directory/'worker.cpp').write_text(worker)
    binary = directory/'native'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
        '-I'+str(directory), '-I'+str(root/'main'), '-I'+str(root/'components/recovery_usb/include'),
        str(directory/'transfer.cpp'), str(root/'main/runtime/raw_flash_evidence.cpp'),
        str(root/'main/runtime/storage_evidence_mode.cpp'), str(directory/'worker.cpp'),
        str(root/'scripts/testdata/evidence_transfer.cpp'), '-lcrypto', '-o', str(binary)], check=True, timeout=60)
    return binary


class FaultLines(ProcessLines):
    def __init__(self, process, fault):
        super().__init__(process); self.fault = fault; self.changed = False; self.last = None
    def read_line(self, timeout):
        line = super().read_line(timeout)
        if not line: return line
        if self.fault == 'nul-final' and line.startswith(b'WQNT1 D '):
            fields = line.split()
            if len(fields) == 7 and base64.b64decode(fields[6], validate=True)[4] == 5:
                time.sleep(0.4)  # Host finalization latency precedes the bad C line.
        if self.fault == 'lose-finish' and line.startswith(b'WQNT1 F '): return b''
        if self.fault in ('corrupt', 'drop-data') and not self.changed and line.startswith(b'WQNT1 D ') and line.split()[3] == b'00000002':
            self.changed = True
            if self.fault == 'drop-data': return None
            return line[:-3]+b'!\n'
        return line
    def write(self, data):
        nul_kind = {'nul-ready': b'WQNT1 R ', 'nul-ack': b'WQNT1 A ',
                    'nul-ready-once': b'WQNT1 R ', 'nul-ack-once': b'WQNT1 A ', 'nul-final': b'WQNT1 C '}.get(self.fault)
        if nul_kind is not None and data.startswith(nul_kind) and (not self.fault.endswith('-once') or not self.changed):
            self.changed = True
            data = data[:-1] + b'\0trailing-garbage\n'
        if self.fault == 'wrong-ready' and data.startswith(b'WQNT1 R '): data = b'WQNT1 R '+b'44'*16+b'\n'
        if self.fault == 'peer-cancel' and data.startswith(b'WQNT1 R '): data = b'WQNT1 Q '+b'11'*16+b'\n'
        if self.fault in ('drop-ack', 'wrong-ack') and not self.changed and data.startswith(b'WQNT1 A '):
            self.changed = True
            if self.fault == 'drop-ack': return
            fields = data.split(); fields[4] = b'44'*32; data = b' '.join(fields)+b'\n'
        if self.fault == 'drop-final' and not self.changed and data.startswith(b'WQNT1 C '):
            self.changed = True; return
        if self.fault == 'drop-all-acks' and data.startswith((b'WQNT1 A ', b'WQNT1 C ')): return
        super().write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('trust-ack', 'skip-ready', 'allow-manual-logical', 'allow-nul'))
    args = parser.parse_args()
    checks = failures = 0
    def check(good, name):
        nonlocal checks, failures
        checks += 1; failures += not good
        print(('PASS transfer: ' if good else 'FAIL transfer: ')+name, flush=True)
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='wqn-transfer-') as temporary:
        directory = Path(temporary); binary = build(root, directory, args.mutation)
        logical_build = directory/'logical-build'; logical_build.mkdir()
        logical_binary = build_logical(root, logical_build, None)
        logical_good, logical_wire = native_case(logical_binary, logical_build, 'normal', None)
        check(logical_good, 'production logical collector fixture')
        for name, mode, fault in (
            ('normal', 'normal', ''), ('final-retry', 'final-retry', 'drop-final'),
            ('logical-success', 'logical-success', ''), ('manual-raw', 'manual-raw', ''),
            ('manual-error', 'manual-error', ''), ('manual-logical', 'manual-logical', ''), ('partial', 'partial', ''), ('large', 'large', ''),
            ('drop-ack', 'normal', 'drop-ack'), ('corrupt', 'normal', 'corrupt'),
            ('drop-data', 'normal', 'drop-data'), ('wrong-ack', 'timeout-ack', 'wrong-ack'),
            ('no-ack', 'timeout-ack', 'drop-all-acks'), ('wrong-ready', 'no-ready', 'wrong-ready'),
            ('no-ready', 'no-ready', 'drop-all-acks'), ('cancel-peer', 'normal', 'peer-cancel'),
            ('nul-ready', 'nul-ready', 'nul-ready'), ('nul-ack', 'nul-ack', 'nul-ack'),
            ('nul-final', 'nul-final', 'nul-final'),
            ('nul-ready-retry', 'normal', 'nul-ready-once'), ('nul-ack-retry', 'normal', 'nul-ack-once'),
            ('late-fail', 'late-fail', ''), ('lose-finish', 'normal', 'lose-finish'),
            ('source-error', 'source-error', ''), ('open-error', 'open-error', ''),
            ('zero-write', 'zero-write', ''), ('write-error', 'write-error', ''),
            ('not-mode', 'not-mode', ''), ('off-owner', 'off-owner', ''),
            ('oom', 'oom', ''), ('invalid-kind', 'invalid-kind', ''), ('expired', 'expired', ''),
            ('cancel-early', 'cancel-early', ''), ('logical', 'logical', ''),
        ):
            process = subprocess.Popen([str(binary), mode, str(logical_wire)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, bufsize=0)
            io = FaultLines(process, fault)
            session = Session(directory/name, PINS[1], PINS[2], 'L' if mode.startswith('logical') or mode == 'manual-logical' else 'R', True)
            answer = None; error = None
            if name == 'no-ready': io.write(b'not a READY\n')
            try:
                # Deliberately no host READY until expiry in this scenario.
                if name == 'no-ready':
                    io.write = lambda data: None
                answer = session.run(io, timeout=25, idle_timeout=20)
            except (ValueError, OSError, TimeoutError) as problem:
                error = problem
            finally:
                io.close()
            diagnostic = process.stderr.read().decode(); process.stderr.close(); process.stdout.close()
            if process.returncode != 0 or not diagnostic.startswith('{'):
                raise RuntimeError('native infrastructure failure '+diagnostic[-4096:])
            state = json.loads(diagnostic)
            success = name in ('normal', 'partial', 'large', 'drop-ack', 'corrupt', 'drop-data', 'lose-finish', 'final-retry', 'logical-success', 'manual-raw', 'manual-error', 'nul-ready-retry', 'nul-ack-retry')
            committed = success or name in ('late-fail', 'source-error')
            check((state['error'] == 0) == success, name+' device result')
            check(state['ack'] == committed and state['leaks'] == 0, name+' commit/heap')
            if committed or name == 'nul-final':
                check(answer is not None and answer['source_complete'] == (name != 'source-error'), name+' host durable result')
                if answer is None:
                    print('DETAIL transfer: '+name+' '+json.dumps(state, sort_keys=True)+' '+repr(error), flush=True)
                    continue  # Keep this failure and complete the other scenarios.
                if name == 'logical-success':
                    receipt = logical.verify_bundle(session.evidence, answer['manifest_sha256'])
                    check((session.evidence/'wire.bin').read_bytes() == logical_wire.read_bytes(), 'logical transcript exact')
                    check(not receipt['repair_authorized'] and answer['device_received_commit_ack'], 'logical durable delivery/authority')
                    continue
                receipt = raw.verify_bundle(session.evidence, answer['manifest_sha256'], True)
                image = (session.evidence/'main-flash.bin').read_bytes()
                expected_size = 384 if name == 'source-error' else 1024*1024 if name == 'large' else 32769
                check(image == bytes((17*i+29)&255 for i in range(expected_size)), name+' exact source prefix')
                check(not receipt['repair_authorized'] and not answer['identity_authenticated'], name+' no inferred authority')
                check(answer['device_received_commit_ack'] == (name not in ('late-fail', 'lose-finish', 'nul-final')), name+' final delivery certainty')
                if name == 'nul-final':
                    check(0 < state['bytes'] < expected_size and state['progress'] == state['bytes'], name+' unacknowledged final window stays pending')
                    check((directory/name/'.transport-incomplete').exists(), name+' host commit does not imply device confirmation')
                else:
                    check(state['bytes'] == expected_size and state['progress'] == expected_size, name+' ACKed progress')
            else:
                check(error is not None and (directory/name/'.transport-incomplete').exists(), name+' preserve incomplete')
            if name in ('no-ready', 'wrong-ready', 'nul-ready', 'cancel-peer', 'not-mode', 'off-owner', 'oom', 'invalid-kind', 'expired', 'cancel-early', 'manual-logical'):
                check(state['reads'] == 0, name+' no source reads before admission')
            if name == 'manual-logical':
                check(state['opens'] == 0, name+' raw-only policy before reserving transport')
            if name in ('wrong-ack', 'no-ack', 'nul-ack'):
                check(state['bytes'] == 0, name+' invalid ACK cannot advance progress')
        # Host durability failures must prevent both group and final ACKs.
        for failure in ('checkpoint', 'verify', 'delivery-sync'):
            process = subprocess.Popen([str(binary), 'normal'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            io = FaultLines(process, ''); session = Session(directory/('fault-'+failure), PINS[1], PINS[2], 'R', True)
            written = []; original = io.write
            def record(data): written.append(data); original(data)
            io.write = record
            target = 'raw_flash_evidence_receive.Receiver.checkpoint' if failure == 'checkpoint' else 'raw_flash_evidence_receive.verify_bundle'
            try:
                sync = __import__('storage_evidence_receive').sync_dir
                def sync_failure(path):
                    if Path(path) == session.output and list(session.output.glob('delivery-*.json')):
                        raise OSError('injected delivery directory fsync failure')
                    return sync(path)
                injection = patch('evidence_transport.logical.sync_dir', side_effect=sync_failure) if failure == 'delivery-sync' else patch(target, side_effect=OSError('injected durable failure'))
                with injection:
                    session.run(io, timeout=3, idle_timeout=2)
            except OSError: pass
            else: check(False, failure+' must fail')
            finally: io.close()
            diagnostic = process.stderr.read().decode(); process.stderr.close(); process.stdout.close()
            if process.returncode != 0 or not diagnostic.startswith('{'): raise RuntimeError(diagnostic)
            state = json.loads(diagnostic)
            check(not state['ack'] and not any(x.startswith(b'WQNT1 C ') for x in written), failure+' forbids final ACK')
            check((session.output/'.transport-incomplete').exists(), failure+' retains uncertainty marker')
        try: Session(directory/'unconsented', PINS[1], PINS[2], 'R', False)
        except ValueError: check(not (directory/'unconsented').exists(), 'consent before any output')
        else: check(False, 'unconsented raw export refused')
    print(f'Evidence transfer: {checks} checks, {failures} failures (production worker/transport/raw; host SDK ports; NOT HIL)')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
