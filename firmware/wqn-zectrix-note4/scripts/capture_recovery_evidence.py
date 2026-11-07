#!/usr/bin/env python3
"""Receive a physically confirmed recovery export via Windows native USB, without resetting it.

Only the explicit CLI opens a port. No device repair, upload or flashing occurs.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading

from evidence_transport import Session


class ProcessLines:
    def __init__(self, process):
        self.process = process
        self.lines = queue.Queue(maxsize=64)
        self.closed = threading.Event()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        while not self.closed.is_set():
            try:
                line = self.process.stdout.readline(1025)
            except OSError:
                line = b''
            while not self.closed.is_set():
                try:
                    self.lines.put(line, timeout=0.05)
                    break
                except queue.Full:
                    pass
            if not line:
                return

    def read_line(self, timeout):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def write(self, data):
        if len(data) > 512 or not data.endswith(b'\n'):
            raise ValueError('invalid host protocol line')
        # At most one small ACK is outstanding; the proxy has a 250 ms native write limit.
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def close(self):
        self.closed.set()
        try:
            self.process.stdin.close()
            self.process.wait(timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait(timeout=2)
        self.reader.join(timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--windows-port', required=True)
    parser.add_argument('--kind', choices=('raw', 'logical'), required=True)
    parser.add_argument('--mac', required=True)
    parser.add_argument('--firmware-elf-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True, help='new private directory')
    parser.add_argument('--sensitive-main-flash', action='store_true', help='explicit raw-Flash sensitive-data consent')
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--idle-timeout', type=int, default=180)
    args = parser.parse_args()
    if not re.fullmatch(r'COM[1-9][0-9]*', args.windows_port, re.I):
        parser.error('expected a Windows port such as COM7')
    if not 1 <= args.timeout <= 1800 or not 1 <= args.idle_timeout <= 180:
        parser.error('timeout outside bounds')
    proxy = Path(__file__).resolve().with_name('recovery_serial_proxy.ps1')
    if os.name == 'nt':
        powershell, proxy_path = 'powershell.exe', str(proxy)
    else:
        powershell = 'powershell.exe'
        proxy_path = subprocess.check_output(['wslpath', '-w', str(proxy)], text=True, timeout=5).strip()
    session = Session(args.output, args.mac, args.firmware_elf_sha256,
                      'R' if args.kind == 'raw' else 'L', args.sensitive_main_flash)
    io = None
    try:
        process = subprocess.Popen([powershell, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', proxy_path, '-Port', args.windows_port], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=None, bufsize=0)
        io = ProcessLines(process)
        result = session.run(io, args.timeout, args.idle_timeout)
        print(json.dumps(result, sort_keys=True))
        return 2 if not result['device_received_commit_ack'] else 0 if result['source_complete'] else 1
    except KeyboardInterrupt:
        if io is not None and session.capture is not None:
            io.write(f'WQNT1 Q {session.capture}\n'.encode())
        print('Export cancelled; preserve the output directory and its incomplete marker.', file=sys.stderr)
        return 130
    finally:
        if io is not None:
            io.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, TimeoutError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(status='export_incomplete', error=str(error), repair_authorized=False)), file=sys.stderr)
        raise SystemExit(1)
