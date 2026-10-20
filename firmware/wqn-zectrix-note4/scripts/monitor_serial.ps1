# Windows-native, no-reset listener for Note4 USB-Serial-JTAG.
# WSL and D: copies both save to WSL. Only listener status is deduplicated.
# Device lines remain unprefixed for hil_check.py.
[CmdletBinding()]
param(
    [string]$Port = 'COM7',
    [int]$Baud = 115200,
    [switch]$ResetOnStart,
    [string]$LogPath,
    [ValidateRange(0, 86400)]
    [int]$DurationSeconds = 0
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Get-MonitorLogDirectory {
    return '\\wsl.localhost\Ubuntu\home\unknow\projects\firmware\firmware\wqn-zectrix-note4\logs'
}
function Write-DeviceLine([string]$Line) {
    try {
        $script:writer.WriteLine($Line)
        $script:writer.Flush()
    }
    catch { $script:logFailed = $true; throw }
    Write-Host $Line
}
function Flush-DevicePartialLine {
    if ($script:pendingLine.Length -gt 0) {
        Write-DeviceLine ($script:pendingLine.TrimEnd([char]13))
        $script:pendingLine = ''
    }
}
function Write-DeviceChunk([string]$Text) {
    $script:pendingLine += $Text
    while (($end = $script:pendingLine.IndexOf([char]10)) -ge 0) {
        Write-DeviceLine ($script:pendingLine.Substring(0, $end).TrimEnd([char]13))
        $script:pendingLine = $script:pendingLine.Substring($end + 1)
    }
}
function Say([string]$Text) {
    # Listener events must not become part of an unfinished device line.
    Flush-DevicePartialLine
    Write-DeviceLine ('[' + (Get-Date -Format 'HH:mm:ss') + '] ' + $Text)
}
function New-OutageState {
    return @{
        Active = $false; StartedAt = 0.0; LastReportAt = 0.0
        Key = ''; Reason = ''; Attempts = 0; Suppressed = 0
        IgnoredF2 = 0; PendingIgnoredF2 = 0; F2Reported = $false
    }
}
function Get-SerialFailure([System.Exception]$Exception) {
    # Strip PowerShell invocation wrappers; key on stable OS codes, not their text.
    $root = $Exception
    while ($null -ne $root.InnerException) { $root = $root.InnerException }
    $code = $root.HResult
    if ($root -is [System.ComponentModel.Win32Exception]) { $code = $root.NativeErrorCode }
    return @{
        Key = "$Port|$($root.GetType().FullName)|$code"
        Reason = ($root.Message -replace '\s+', ' ').Trim()
    }
}
function Write-OutageSummary([hashtable]$State, [double]$Now, [string]$Context) {
    if (-not $State.Active) { return }
    $seconds = [int][Math]::Floor([Math]::Max(0, $Now - $State.StartedAt))
    Say ("$Port still unavailable: $($State.Reason); failed attempts=$($State.Attempts); " +
        "offline=$($seconds)s; repeated failures suppressed=$($State.Suppressed); " +
        "ignored F2=$($State.IgnoredF2); $Context")
    $State.Suppressed = 0
    $State.PendingIgnoredF2 = 0
    $State.LastReportAt = $Now
}
function Note-SerialFailure([hashtable]$State, [System.Exception]$Exception, [double]$Now) {
    $failure = Get-SerialFailure $Exception
    if (-not $State.Active) {
        $State.Active = $true
        $State.StartedAt = $Now
        $State.LastReportAt = $Now
        $State.Key = $failure.Key
        $State.Reason = $failure.Reason
        $State.Attempts = 1
        Say "$Port unavailable: $($State.Reason); retrying every 1s"
        return
    }
    if ($failure.Key -ne $State.Key) {
        if ($State.Suppressed -gt 0 -or $State.PendingIgnoredF2 -gt 0) {
            Write-OutageSummary $State $Now 'previous error summary'
        }
        $State.Key = $failure.Key
        $State.Reason = $failure.Reason
        $State.Attempts++
        $State.LastReportAt = $Now
        Say "$Port unavailable: $($State.Reason); reason changed; retrying every 1s"
    }
    else { $State.Attempts++; $State.Suppressed++ }
    if ($Now - $State.LastReportAt -ge 60) {
        Write-OutageSummary $State $Now 'retrying every 1s'
    }
}
function Note-OfflineReset([hashtable]$State) {
    $State.IgnoredF2++
    $State.PendingIgnoredF2++
    if (-not $State.F2Reported) {
        $State.F2Reported = $true
        Say "F2 ignored: $Port is not connected; reset request discarded"
    }
}
function Note-ListenerAttached([hashtable]$State, [double]$Now) {
    $text = "listener attached: $Port"
    if ($State.Active) {
        $seconds = [int][Math]::Floor([Math]::Max(0, $Now - $State.StartedAt))
        $text += "; offline=$($seconds)s; failed attempts=$($State.Attempts); ignored F2=$($State.IgnoredF2)"
    }
    elseif ($State.IgnoredF2 -gt 0) { $text += "; ignored F2=$($State.IgnoredF2)" }
    Say $text
    $fresh = New-OutageState
    foreach ($key in @($fresh.Keys)) { $State[$key] = $fresh[$key] }
}
function Get-MonitorKeyboardSource {
    # Console.ReadKey lacks key-up events and cannot identify a held F2 reliably.
    return @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace Wqn.SerialMonitor {
    public sealed class F2Latch {
        private bool held;
        public bool Feed(bool down, ushort key) {
            if (key != 0x71) return false;
            if (!down) { held = false; return false; }
            if (held) return false;
            held = true;
            return true;
        }
        public void LostFocus() { held = false; }
    }
    public sealed class ConsoleKeyboard {
        [StructLayout(LayoutKind.Explicit, Size = 20)]
        private struct InputRecord {
            [FieldOffset(0)] public ushort EventType;
            [FieldOffset(4)] public int KeyDown;
            [FieldOffset(4)] public int Focused;
            [FieldOffset(10)] public ushort VirtualKey;
        }
        [DllImport("kernel32.dll")] private static extern IntPtr GetStdHandle(int id);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetConsoleMode(IntPtr handle, out uint mode);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetNumberOfConsoleInputEvents(IntPtr handle, out uint count);
        [DllImport("kernel32.dll", EntryPoint = "ReadConsoleInputW", SetLastError = true)]
        private static extern bool ReadConsoleInput(IntPtr handle, [Out] InputRecord[] records,
            uint count, out uint read);
        private readonly IntPtr input;
        private readonly F2Latch latch = new F2Latch();
        public ConsoleKeyboard() {
            input = GetStdHandle(-10);
            uint mode;
            if (!GetConsoleMode(input, out mode)) throw new Win32Exception();
        }
        public bool Poll() {
            bool requested = false;
            for (int batch = 0; batch < 4; batch++) {
                uint count;
                if (!GetNumberOfConsoleInputEvents(input, out count)) throw new Win32Exception();
                if (count == 0) break;
                InputRecord[] records = new InputRecord[Math.Min(count, 64u)];
                uint read;
                if (!ReadConsoleInput(input, records, (uint)records.Length, out read))
                    throw new Win32Exception();
                for (int i = 0; i < read; i++) {
                    if (records[i].EventType == 1 &&
                        latch.Feed(records[i].KeyDown != 0, records[i].VirtualKey))
                        requested = true;
                    else if (records[i].EventType == 16 && records[i].Focused == 0)
                        latch.LostFocus();
                }
            }
            return requested;
        }
    }
}
'@
}
function New-MonitorKeyboard {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or
        [Console]::IsInputRedirected -or
        [Environment]::GetCommandLineArgs() -contains '-NonInteractive') { return $null }
    if ($null -eq ('Wqn.SerialMonitor.ConsoleKeyboard' -as [type])) {
        Add-Type -TypeDefinition (Get-MonitorKeyboardSource)
    }
    return New-Object Wqn.SerialMonitor.ConsoleKeyboard
}
function Get-MonitorPortSource {
    # Do not use System.IO.Ports.SerialPort here. Framework SerialStream.Open
    # first submits DTR_CONTROL_DISABLE, even when DtrEnable was preset true.
    # Keep the native boundary injectable so tests exercise this exact backend.
    return @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32.SafeHandles;
namespace Wqn.SerialMonitor {
    [StructLayout(LayoutKind.Sequential)]
    public struct Dcb {
        public uint Length, BaudRate, Flags;
        public ushort Reserved, XonLim, XoffLim;
        public byte ByteSize, Parity, StopBits, XonChar, XoffChar;
        public byte ErrorChar, EofChar, EvtChar;
        public ushort Reserved1;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct CommTimeouts {
        public uint ReadInterval, ReadMultiplier, ReadConstant;
        public uint WriteMultiplier, WriteConstant;
    }
    public interface ICommApi {
        SafeFileHandle Open(string path);
        Dcb GetState(SafeFileHandle handle);
        void SetState(SafeFileHandle handle, Dcb state);
        void SetTimeouts(SafeFileHandle handle, CommTimeouts timeouts);
        void Escape(SafeFileHandle handle, uint command);
        int Read(SafeFileHandle handle, byte[] buffer);
    }
    internal sealed class Win32CommApi : ICommApi {
        [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode,
            ExactSpelling = true, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(string name, uint access,
            uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetCommState(SafeFileHandle handle, ref Dcb state);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetCommState(SafeFileHandle handle, ref Dcb state);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetCommTimeouts(SafeFileHandle handle, ref CommTimeouts timeouts);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool EscapeCommFunction(SafeFileHandle handle, uint command);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadFile(SafeFileHandle handle, [Out] byte[] buffer,
            uint size, out uint read, IntPtr overlapped);
        public SafeFileHandle Open(string path) {
            // Synchronous reads use immediate-return COMMTIMEOUTS below.
            SafeFileHandle result = CreateFile(path, 0xc0000000u, 0, IntPtr.Zero,
                3, 0x80, IntPtr.Zero);
            if (result.IsInvalid) {
                int error = Marshal.GetLastWin32Error();
                result.Dispose();
                throw new Win32Exception(error);
            }
            return result;
        }
        public Dcb GetState(SafeFileHandle handle) {
            Dcb state = new Dcb();
            state.Length = (uint)Marshal.SizeOf(typeof(Dcb));
            if (!GetCommState(handle, ref state)) throw new Win32Exception();
            return state;
        }
        public void SetState(SafeFileHandle handle, Dcb state) {
            if (!SetCommState(handle, ref state)) throw new Win32Exception();
        }
        public void SetTimeouts(SafeFileHandle handle, CommTimeouts timeouts) {
            if (!SetCommTimeouts(handle, ref timeouts)) throw new Win32Exception();
        }
        public void Escape(SafeFileHandle handle, uint command) {
            if (!EscapeCommFunction(handle, command)) throw new Win32Exception();
        }
        public int Read(SafeFileHandle handle, byte[] buffer) {
            uint count;
            if (!ReadFile(handle, buffer, (uint)buffer.Length, out count, IntPtr.Zero))
                throw new Win32Exception();
            return checked((int)count);
        }
    }
    public sealed class NativePort : IDisposable {
        private readonly ICommApi api;
        private readonly string port;
        private readonly int baud;
        private SafeFileHandle handle;
        private bool dtr, rts;
        private readonly byte[] bytes = new byte[8192];
        private readonly char[] chars = new char[8193];
        private readonly Decoder decoder = Encoding.UTF8.GetDecoder();
        public NativePort(string port, int baud) : this(port, baud, new Win32CommApi()) {}
        public NativePort(string port, int baud, ICommApi api) {
            if (port == null || !Regex.IsMatch(port, @"\ACOM[1-9][0-9]*\z", RegexOptions.IgnoreCase))
                throw new ArgumentException("Expected a COM port name such as COM7", "port");
            if (baud <= 0) throw new ArgumentOutOfRangeException("baud");
            if (api == null) throw new ArgumentNullException("api");
            this.port = port;
            this.baud = baud;
            this.api = api;
        }
        public bool IsOpen { get { return handle != null && !handle.IsClosed && !handle.IsInvalid; } }
        public bool DtrEnabled { get { return dtr; } }
        public bool RtsEnabled { get { return rts; } }
        public void OpenNoReset() {
            if (IsOpen) throw new InvalidOperationException("Serial port already open");
            try {
                handle = api.Open(@"\\.\" + port);
                if (!IsOpen) throw new InvalidOperationException("Invalid serial handle");
                Dcb state = api.GetState(handle);
                state.Length = (uint)Marshal.SizeOf(typeof(Dcb));
                state.BaudRate = (uint)baud;
                state.ByteSize = 8; state.Parity = 0; state.StopBits = 0;
                // No flow control, parity/error replacement or null filtering.
                // Submit BOTH asserted controls in the FIRST SetCommState call;
                // never submit Framework's intermediate DTR_CONTROL_DISABLE.
                state.Flags = (state.Flags & ~0x7f7eu) | 0x1011u;
                if (state.XonChar == state.XoffChar) {
                    state.XonChar = 17; state.XoffChar = 19;
                }
                api.SetState(handle, state);
                dtr = rts = true;
                CommTimeouts timeouts = new CommTimeouts();
                timeouts.ReadInterval = UInt32.MaxValue;
                api.SetTimeouts(handle, timeouts);
                // Release RTS before DTR. Re-apply DTR when changing RTS for
                // Windows usbser.sys, matching the IDF Monitor ordering.
                SetRts(false);
                SetDtr(false);
            }
            catch { Close(); throw; }
        }
        private void RequireOpen() {
            if (!IsOpen) throw new InvalidOperationException("Serial port is not open");
        }
        public void SetDtr(bool asserted) {
            RequireOpen();
            api.Escape(handle, asserted ? 5u : 6u);
            dtr = asserted;
        }
        public void SetRts(bool asserted) {
            RequireOpen();
            api.Escape(handle, asserted ? 3u : 4u);
            rts = asserted;
            // Force the same DTR value after an RTS update (usbser.sys).
            api.Escape(handle, dtr ? 5u : 6u);
        }
        public string ReadExisting() {
            RequireOpen();
            int count = api.Read(handle, bytes);
            if (count < 0 || count > bytes.Length) throw new InvalidOperationException("Invalid read size");
            if (count == 0) return String.Empty;
            int decoded = decoder.GetChars(bytes, 0, count, chars, 0, false);
            return new String(chars, 0, decoded);
        }
        public void Close() {
            // Generic handle disposal only: never implicitly toggle DTR/RTS.
            if (handle != null) { handle.Dispose(); handle = null; }
            dtr = rts = false;
            decoder.Reset();
        }
        public void Dispose() { Close(); }
    }
}
'@
}
function Initialize-MonitorPortType {
    if ($null -eq ('Wqn.SerialMonitor.NativePort' -as [type])) {
        Add-Type -TypeDefinition (Get-MonitorPortSource)
    }
}
function New-MonitorPort {
    Initialize-MonitorPortType
    return New-Object Wqn.SerialMonitor.NativePort -ArgumentList $Port, $Baud
}
function Open-MonitorPort($Serial) {
    $Serial.OpenNoReset()
}
function Set-MonitorRts($Serial, [bool]$Asserted) {
    $Serial.SetRts($Asserted)
}
function Send-DeviceReset($Serial, [string]$Source) {
    Say "$Source reset requested: $Port"
    $Serial.SetDtr($false)
    try {
        Set-MonitorRts $Serial $true
        # Native-USB esptool hard reset allows 200 ms for each transition.
        Start-Sleep -Milliseconds 200
    }
    finally { Set-MonitorRts $Serial $false }
    Start-Sleep -Milliseconds 200
    Say "reset pulse sent: $Port; source=$Source"
}
function Close-MonitorPort($Serial) {
    if ($null -ne $Serial) {
        try { $Serial.Close() } catch {}
        try { $Serial.Dispose() } catch {}
    }
}
function Get-ResetKey([ref]$Keyboard) {
    if ($null -eq $Keyboard.Value) { return $false }
    try { return $Keyboard.Value.Poll() }
    catch {
        $Keyboard.Value = $null
        Say "keyboard input unavailable; F2 disabled: $($_.Exception.Message)"
        return $false
    }
}
function CaptureActive {
    return ($DurationSeconds -eq 0 -or $script:captureTimer.Elapsed.TotalSeconds -lt $DurationSeconds)
}
function Invoke-MonitorLoop([scriptblock]$CreatePort, $Keyboard) {
    $serial = $null
    $outage = New-OutageState
    $nextAttempt = 0.0
    $resetPending = $ResetOnStart.IsPresent
    try {
        while (CaptureActive) {
            $now = $script:captureTimer.Elapsed.TotalSeconds
            $resetKey = Get-ResetKey ([ref]$Keyboard)
            if ($null -eq $serial -and $resetKey) { Note-OfflineReset $outage }
            try {
                if ($null -eq $serial -and $now -ge $nextAttempt) {
                    $serial = & $CreatePort
                    Open-MonitorPort $serial
                    # Keys pressed during blocking Open belong to the offline interval.
                    if (Get-ResetKey ([ref]$Keyboard)) { Note-OfflineReset $outage }
                    Note-ListenerAttached $outage $script:captureTimer.Elapsed.TotalSeconds
                    if ($resetPending) {
                        $resetPending = $false
                        Send-DeviceReset $serial 'startup'
                        [void](Get-ResetKey ([ref]$Keyboard))
                    }
                    $resetKey = $false
                }
                if ($null -ne $serial) {
                    if (-not $serial.IsOpen) {
                        if ($resetKey) { Note-OfflineReset $outage }
                        throw [System.IO.IOException]::new('Serial port disconnected')
                    }
                    # The native backend retains incomplete UTF-8 characters; we
                    # retain partial lines while polling without blocking.
                    Write-DeviceChunk ($serial.ReadExisting())
                    if ($resetKey) {
                        Send-DeviceReset $serial 'manual'
                        # Consume presses queued during reset, including key-up events.
                        [void](Get-ResetKey ([ref]$Keyboard))
                    }
                }
            }
            catch {
                if ($script:logFailed -or
                    $_.Exception -is [System.Management.Automation.PipelineStoppedException]) { throw }
                Close-MonitorPort $serial
                $serial = $null
                Note-SerialFailure $outage $_.Exception $script:captureTimer.Elapsed.TotalSeconds
                $nextAttempt = $script:captureTimer.Elapsed.TotalSeconds + 1.0
            }
            Start-Sleep -Milliseconds 50
        }
        Say "capture duration limit reached: $DurationSeconds s"
    }
    finally {
        Close-MonitorPort $serial
        if (-not $script:logFailed) {
            Write-OutageSummary $outage $script:captureTimer.Elapsed.TotalSeconds 'final summary'
        }
    }
}
$script:writer = $null
$logStream = $null
$script:pendingLine = ''
$script:logFailed = $false
$exitCode = 0
try {
    if (-not $LogPath) {
        $logDir = Get-MonitorLogDirectory
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
        # FullName avoids provider-qualified paths rejected by System.IO.
        $logDir = (Get-Item $logDir).FullName
        $stamp = Get-Date -Format 'yyMMdd-HHmmss-fff'
        $LogPath = Join-Path $logDir "serial-$Port-$stamp-$PID.log"
    }
    else { $LogPath = [System.IO.Path]::GetFullPath($LogPath) }
    # Log creation must succeed before touching COM.
    $logStream = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $script:writer = New-Object System.IO.StreamWriter($logStream, $utf8NoBom)
    Say "monitoring $Port @ $Baud bps, backend=win32, log: $LogPath"
    Initialize-MonitorPortType
    $keyboard = $null
    try { $keyboard = New-MonitorKeyboard }
    catch { Say "keyboard input unavailable; F2 disabled: $($_.Exception.Message)" }
    if ($null -ne $keyboard) { Write-Host 'F2: Reset device | Ctrl+C: Exit' -ForegroundColor Cyan }
    else { Say 'F2 disabled: no interactive Windows console' }
    $script:captureTimer = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-MonitorLoop { New-MonitorPort } $keyboard
}
catch {
    $exitCode = 1
    Write-Host "Monitor stopped with error (log: '$LogPath'): $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    if ($null -ne $script:writer) {
        if (-not $script:logFailed) {
            try { Say 'monitor stopped' } catch { $exitCode = 1 }
        }
        try { $script:writer.Dispose() } catch { $exitCode = 1 }
    }
    if ($null -ne $logStream) {
        try { $logStream.Dispose() } catch { $exitCode = 1 }
    }
    if ($LogPath) { Write-Host "Log: $LogPath" -ForegroundColor DarkGray }
}
exit $exitCode
