#!/usr/bin/env python3
"""Exercise the production PowerShell pump with a fake device backend. No port access."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from capture_recovery_evidence import ProcessLines

FAKE = '''function Get-MonitorPortSource {
@'
namespace Wqn.SerialMonitor {
    public sealed class NativePort : System.IDisposable {
        private string pending = "OFFER\\n";
        public NativePort(string port, int baud) {}
        public void OpenNoReset() {}
        public string ReadExisting() { string data = pending; pending = ""; return data; }
        public void WriteAscii(string data) { pending = "ECHO " + data; }
        public void Dispose() {}
    }
}
'@
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--powershell', default='pwsh')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wqn-recovery-pump-') as temporary:
        directory = Path(temporary)
        shutil.copy2(Path(__file__).with_name('recovery_serial_proxy.ps1'), directory/'proxy.ps1')
        (directory/'monitor_serial.ps1').write_text(FAKE)
        proxy = str(directory/'proxy.ps1')
        if args.powershell.endswith('.exe'):
            proxy = subprocess.check_output(['wslpath', '-w', proxy], text=True, timeout=5).strip()
        process = subprocess.Popen([args.powershell, '-NoProfile', '-NonInteractive', '-ExecutionPolicy',
            'Bypass', '-File', proxy, '-Port', 'COM7'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        io = ProcessLines(process)
        def read():
            end = time.monotonic()+15
            while time.monotonic() < end:
                line = io.read_line(0.05)
                if line is not None: return line
            raise TimeoutError('proxy pump stalled')
        try:
            assert read() == b'OFFER\n', 'device output must flow while host input is idle'
            io.write(b'HOST-ACK\n')
            assert read() == b'ECHO HOST-ACK\n', 'host ACK must flow while device is idle'
        finally:
            io.close()
        errors = process.stderr.read(); process.stderr.close(); process.stdout.close()
        assert process.returncode == 0 and errors == b'', errors
    print('Recovery serial proxy: 3 checks, 0 failures (production pump; fake backend; NOT HIL)')


if __name__ == '__main__':
    main()
