# Host-only regression tests. Never opens a real device port or resets a chip.
[CmdletBinding()]
param([string]$MonitorPath)

$ErrorActionPreference = 'Stop'
$monitor = Join-Path $PSScriptRoot 'monitor_serial.ps1'
if ($MonitorPath) { $monitor = $MonitorPath }
$psExe = (Get-Process -Id $PID).Path
$testDir = Join-Path ([System.IO.Path]::GetTempPath()) ('wqn-monitor-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir | Out-Null
$passed = 0
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Check([bool]$condition, [string]$name) {
    if (-not $condition) { throw "FAIL: $name" }
    Write-Host "PASS: $name"
    $script:passed++
}

try {
    $parseErrors = $null
    $tokens = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($monitor, [ref]$tokens, [ref]$parseErrors)
    Check ($parseErrors.Count -eq 0) 'PowerShell syntax'

    $missing = Join-Path $testDir 'missing-port.log'
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor `
        -Port 'COM98765' -DurationSeconds 2 -LogPath $missing 2>&1
    $exitCode = $LASTEXITCODE
    $timer.Stop()
    Check ($exitCode -eq 0 -and $timer.Elapsed.TotalSeconds -lt 10) 'missing port stops within bounded capture'
    $raw = [System.IO.File]::ReadAllBytes($missing)
    $content = [System.IO.File]::ReadAllText($missing)
    Check ($raw.Length -gt 3 -and -not ($raw[0] -eq 239 -and $raw[1] -eq 187 -and $raw[2] -eq 191)) 'UTF-8 without BOM'
    Check ($content -match 'unavailable:' -and $content -match 'capture duration limit reached: 2 s' `
        -and $content -match 'monitor stopped') 'retry cause and clean stop are recorded'
    Check ($content -notmatch '(?m)^[IWEDV] \(' -and $content -notmatch 'reset pulse sent|opened COM') 'listener status cannot invent a device boot or reset'

    $before = [Convert]::ToBase64String($raw)
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor `
        -Port 'COM98765' -DurationSeconds 1 -LogPath $missing 2>&1
    $exitCode = $LASTEXITCODE
    $after = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($missing))
    Check ($exitCode -ne 0 -and $before -ceq $after) 'existing evidence is not overwritten'

    $invalid = Join-Path $testDir 'invalid-duration.log'
    # Native stderr from expected parameter-validation failures must be data,
    # not a terminating PowerShell pipeline error on Windows PowerShell 5.1.
    $ErrorActionPreference = 'Continue'
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor `
        -Port 'COM98765' -DurationSeconds -1 -LogPath $invalid 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Check ($exitCode -ne 0 -and -not (Test-Path $invalid)) 'invalid duration fails before opening a log or port'

    # Load only the production functions, never the real-port entrypoint.
    foreach ($fn in $ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
    }, $false)) {
        . ([scriptblock]::Create($fn.Extent.Text))
    }
    $Port = 'COM7'
    $Baud = 115200
    $ResetOnStart = [switch]$false
    $DurationSeconds = 2
    $script:pendingLine = ''
    $script:logFailed = $false
    $script:writer = New-Object System.IO.StringWriter
    Check ((Get-MonitorLogDirectory) -eq '\\wsl.localhost\Ubuntu\home\unknow\projects\firmware\firmware\wqn-zectrix-note4\logs') 'default logs go to WSL regardless of launcher location'

    $outage = New-OutageState
    $missingError = New-Object System.ComponentModel.Win32Exception(2)
    Note-SerialFailure $outage $missingError 0
    foreach ($second in 1..59) { Note-SerialFailure $outage $missingError $second }
    Check (([regex]::Matches($script:writer.ToString(), 'COM7 unavailable:')).Count -eq 1) 'identical offline errors are quiet between summaries'
    Note-SerialFailure $outage $missingError 60
    Check ($script:writer.ToString() -match 'failed attempts=61; offline=60s; repeated failures suppressed=60') '60s heartbeat preserves attempts and suppressed count'
    $occupiedError = New-Object System.ComponentModel.Win32Exception(5)
    Note-SerialFailure $outage $occupiedError 61
    Check ($script:writer.ToString() -match 'reason changed') 'changed OS error is reported immediately'
    $wrappedError = New-Object System.Exception('changing wrapper text', $occupiedError)
    Note-SerialFailure $outage $wrappedError 62
    Check (([regex]::Matches($script:writer.ToString(), 'reason changed')).Count -eq 1) 'exception wrapper text does not defeat deduplication'
    foreach ($press in 1..3) { Note-OfflineReset $outage }
    Check (([regex]::Matches($script:writer.ToString(), 'F2 ignored:')).Count -eq 1 -and $outage.IgnoredF2 -eq 3) 'offline F2 requests are counted with one prompt'
    Note-ListenerAttached $outage 76
    Check ($script:writer.ToString() -match 'listener attached: COM7; offline=76s; failed attempts=63; ignored F2=3' -and -not $outage.Active) 'recovery summarizes outage and clears dedup state'
    Note-SerialFailure $outage $missingError 80
    Note-OfflineReset $outage
    Write-OutageSummary $outage 82 'final summary'
    Check (([regex]::Matches($script:writer.ToString(), 'F2 ignored:')).Count -eq 2 -and $script:writer.ToString() -match 'final summary') 'new outage prompts again and exit preserves final counts'

    $script:writer = New-Object System.IO.StringWriter
    $chinese = ([char]0x4E2D).ToString() + [char]0x6587
    Write-DeviceChunk 'I (1) same'
    Write-DeviceChunk ([string][char]13 + [char]10 + 'I (1) same' + [char]10 + 'I (2) ' + $chinese)
    Write-DeviceChunk ([string][char]10)
    Check (([regex]::Matches($script:writer.ToString(), '(?m)^I \(1\) same')).Count -eq 2 -and $script:writer.ToString().Contains($chinese)) 'partial and repeated device lines survive without dedup or prefixes'
    Write-DeviceChunk 'partial device text'
    Say 'listener status'
    Check ($script:writer.ToString() -match 'partial device text\r?\n\[') 'listener status cannot contaminate a partial device line'

    Add-Type -TypeDefinition (Get-MonitorKeyboardSource)
    $latch = New-Object Wqn.SerialMonitor.F2Latch
    Check ($latch.Feed($true, 0x71)) 'first F2 keydown requests reset'
    $repeats = 0
    foreach ($repeat in 1..20) { if ($latch.Feed($true, 0x71)) { $repeats++ } }
    Check ($repeats -eq 0) 'held F2 never requests repeated resets'
    [void]$latch.Feed($false, 0x71)
    Check ($latch.Feed($true, 0x71) -and -not $latch.Feed($true, 0x52)) 'release rearms F2 and other keys never reset'

    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        # Exercise the real Win32 event reader in an owned, hidden console.
        # Injected keyboard events never touch a real device.
        $keyboardTest = Join-Path $testDir 'native-keyboard.ps1'
        $keyboardResult = Join-Path $testDir 'native-keyboard-result.txt'
        $keyboardScript = @'
param([string]$MonitorPath, [string]$ResultPath)
$ErrorActionPreference = 'Stop'
try {
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($MonitorPath, [ref]$null, [ref]$null)
    $fn = $ast.Find({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-MonitorKeyboardSource'
    }, $false)
    . ([scriptblock]::Create($fn.Extent.Text))
    Add-Type -TypeDefinition (Get-MonitorKeyboardSource)
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ConsoleTestInput {
    [StructLayout(LayoutKind.Explicit, Size = 20)]
    public struct Record {
        [FieldOffset(0)] public ushort Type;
        [FieldOffset(4)] public int Down;
        [FieldOffset(8)] public ushort Repeat;
        [FieldOffset(10)] public ushort Key;
    }
    [DllImport("kernel32.dll")] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool AllocConsole();
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int show);
    [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int id);
    [DllImport("kernel32.dll", EntryPoint = "WriteConsoleInputW", SetLastError = true)]
    private static extern bool WriteInput(IntPtr input, Record[] events, uint count, out uint written);
    public static void Feed(bool down, ushort key, ushort repeats) {
        Record record = new Record();
        record.Type = 1; record.Down = down ? 1 : 0; record.Key = key; record.Repeat = repeats;
        uint written;
        if (!WriteInput(GetStdHandle(-10), new Record[] { record }, 1, out written) || written != 1)
            throw new Win32Exception();
    }
}
"@
    [void][ConsoleTestInput]::FreeConsole()
    if (-not [ConsoleTestInput]::AllocConsole()) { throw 'Cannot allocate test console' }
    [void][ConsoleTestInput]::ShowWindow([ConsoleTestInput]::GetConsoleWindow(), 0)
    $keyboard = New-Object Wqn.SerialMonitor.ConsoleKeyboard
    [void]$keyboard.Poll()
    [ConsoleTestInput]::Feed($true, 0x71, 20)
    if (-not $keyboard.Poll()) { throw 'F2 down was not detected' }
    [ConsoleTestInput]::Feed($true, 0x71, 1)
    if ($keyboard.Poll()) { throw 'Held F2 generated a second reset' }
    [ConsoleTestInput]::Feed($false, 0x71, 1)
    if ($keyboard.Poll()) { throw 'F2 release generated a reset' }
    [ConsoleTestInput]::Feed($true, 0x71, 1)
    if (-not $keyboard.Poll()) { throw 'F2 release did not rearm' }
    [ConsoleTestInput]::Feed($true, 0x52, 1)
    if ($keyboard.Poll()) { throw 'Other key generated a reset' }
    [System.IO.File]::WriteAllText($ResultPath, 'PASS')
}
catch {
    [System.IO.File]::WriteAllText($ResultPath, $_.ToString())
    exit 1
}
finally {
    if ($null -ne ('ConsoleTestInput' -as [type])) { [void][ConsoleTestInput]::FreeConsole() }
}
'@
        [System.IO.File]::WriteAllText($keyboardTest, $keyboardScript)
        $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
            ('"' + $keyboardTest + '"'), '-MonitorPath', ('"' + $monitor + '"'),
            '-ResultPath', ('"' + $keyboardResult + '"'))
        $child = Start-Process -FilePath $psExe -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
        $result = [System.IO.File]::ReadAllText($keyboardResult)
        Check ($child.ExitCode -eq 0 -and $result -eq 'PASS') "native Windows console F2 events ($result)"
    }

    # Compile the production backend with a fake Win32 boundary, not a fake port.
    Add-Type -TypeDefinition ((Get-MonitorPortSource) + @'
namespace Wqn.SerialMonitor.Tests {
    public sealed class CommApi : ICommApi {
        public readonly System.Collections.Generic.List<string> Calls = new System.Collections.Generic.List<string>();
        public readonly System.Collections.Generic.List<Dcb> Configurations = new System.Collections.Generic.List<Dcb>();
        public readonly System.Collections.Generic.Queue<byte[]> Chunks = new System.Collections.Generic.Queue<byte[]>();
        public Microsoft.Win32.SafeHandles.SafeFileHandle Handle;
        public CommTimeouts Timeouts;
        public bool Dtr, Rts, DisconnectOnRead, FailReset;
        public int ResetPulses;
        public string FailAt;
        private void Touch(string call) {
            Calls.Add(call);
            if (FailAt == call) throw new System.ComponentModel.Win32Exception(5);
        }
        public Microsoft.Win32.SafeHandles.SafeFileHandle Open(string path) {
            Touch("Open:" + path);
            Handle = new Microsoft.Win32.SafeHandles.SafeFileHandle(new System.IntPtr(123), false);
            return Handle;
        }
        public Dcb GetState(Microsoft.Win32.SafeHandles.SafeFileHandle handle) {
            Touch("GetState");
            Dcb state = new Dcb();
            state.Flags = 0x6be7u; // inherited hardware/software handshaking
            return state;
        }
        public void SetState(Microsoft.Win32.SafeHandles.SafeFileHandle handle, Dcb state) {
            Touch("SetState");
            Configurations.Add(state);
            Dtr = ((state.Flags >> 4) & 3) == 1;
            Rts = ((state.Flags >> 12) & 3) == 1;
        }
        public void SetTimeouts(Microsoft.Win32.SafeHandles.SafeFileHandle handle, CommTimeouts timeouts) {
            Touch("SetTimeouts");
            Timeouts = timeouts;
        }
        public void Escape(Microsoft.Win32.SafeHandles.SafeFileHandle handle, uint command) {
            Touch("Escape:" + command);
            if (command == 3) {
                if (!Rts && !Dtr) ResetPulses++;
                Rts = true;
                if (FailReset) throw new System.ComponentModel.Win32Exception(1117);
            }
            if (command == 4) Rts = false;
            if (command == 5) Dtr = true;
            if (command == 6) Dtr = false;
        }
        public int Read(Microsoft.Win32.SafeHandles.SafeFileHandle handle, byte[] buffer) {
            Touch("Read");
            if (DisconnectOnRead) {
                DisconnectOnRead = false;
                throw new System.ComponentModel.Win32Exception(1167);
            }
            if (Chunks.Count == 0) return 0;
            byte[] data = Chunks.Dequeue();
            System.Array.Copy(data, buffer, data.Length);
            return data.Length;
        }
    }
}
public sealed class FailingMonitorWriter : System.IO.StringWriter {
    public int Writes;
    public override void WriteLine(string line) {
        if (++Writes > 2) throw new System.IO.IOException("WSL log unavailable");
        base.WriteLine(line);
    }
}
'@)
    function New-TestPort($Api) {
        return New-Object Wqn.SerialMonitor.NativePort -ArgumentList 'COM7', 115200, $Api
    }
    Check ((New-MonitorPort) -is [Wqn.SerialMonitor.NativePort]) 'production factory selects the native backend'
    Check ([System.Runtime.InteropServices.Marshal]::SizeOf([type][Wqn.SerialMonitor.Dcb]) -eq 28 -and [System.Runtime.InteropServices.Marshal]::SizeOf([type][Wqn.SerialMonitor.CommTimeouts]) -eq 20) 'Win32 structures match native ABI sizes'
    $api = New-Object Wqn.SerialMonitor.Tests.CommApi
    $fake = New-TestPort $api
    Open-MonitorPort $fake
    $state = $api.Configurations[0]
    Check ($api.Configurations.Count -eq 1 -and ($state.Flags -band 0x3030) -eq 0x1010) 'first and only DCB submission asserts BOTH controls without an intermediate DTR disable'
    Check ($state.Length -eq 28 -and $state.BaudRate -eq 115200 -and $state.ByteSize -eq 8 -and $state.Parity -eq 0 -and $state.StopBits -eq 0 -and ($state.Flags -band 0x7f7f) -eq 0x1011 -and $state.XonChar -ne $state.XoffChar) 'native configuration establishes 8N1 and clears inherited flow control'
    Check (($api.Calls -join ',') -eq 'Open:\\.\COM7,GetState,SetState,SetTimeouts,Escape:4,Escape:5,Escape:6') 'native open configures first then releases RTS before DTR with the Windows workaround'
    Check ($api.Timeouts.ReadInterval -eq [uint32]::MaxValue -and $api.Timeouts.ReadMultiplier -eq 0 -and $api.Timeouts.ReadConstant -eq 0) 'native reads return immediately so keyboard polling stays responsive'
    Check ($api.ResetPulses -eq 0 -and -not $fake.DtrEnabled -and -not $fake.RtsEnabled) 'real backend opening emits no simulated reset pulse'
    Send-DeviceReset $fake 'manual'
    Check ($api.ResetPulses -eq 1 -and -not $api.Dtr -and -not $api.Rts -and $script:writer.ToString() -match 'manual reset requested: COM7') 'explicit manual reset sends one pulse and records its source'
    $api.FailReset = $true
    $failedReset = $false
    try { Send-DeviceReset $fake 'manual' } catch { $failedReset = $true }
    Check ($failedReset -and -not $api.Rts) 'failed reset still attempts to release RTS'
    $calls = $api.Calls.Count
    Close-MonitorPort $fake
    Check ($api.Handle.IsClosed -and $api.Calls.Count -eq $calls) 'close and dispose release the handle without extra DTR or RTS operations'

    foreach ($stage in @('GetState', 'SetState', 'SetTimeouts', 'Escape:4')) {
        $broken = New-Object Wqn.SerialMonitor.Tests.CommApi
        $broken.FailAt = $stage
        $portDouble = New-TestPort $broken
        $failed = $false
        try { Open-MonitorPort $portDouble } catch { $failed = $true }
        Check ($failed -and -not $portDouble.IsOpen -and $broken.Handle.IsClosed -and $broken.Calls[$broken.Calls.Count - 1] -eq $stage) "native setup failure closes its handle without implicit line changes ($stage)"
    }

    $api = New-Object Wqn.SerialMonitor.Tests.CommApi
    $fake = New-TestPort $api
    Open-MonitorPort $fake
    $api.Chunks.Enqueue([byte[]]@(0xe4))
    $api.Chunks.Enqueue([byte[]]@(0xb8, 0xad))
    Check ($fake.ReadExisting() -eq '' -and $fake.ReadExisting() -eq ([char]0x4e2d).ToString()) 'native decoder preserves UTF-8 characters split between byte reads'
    $api.Chunks.Enqueue([byte[]]@(0xe4))
    [void]$fake.ReadExisting()
    Close-MonitorPort $fake
    Open-MonitorPort $fake
    $api.Chunks.Enqueue([byte[]]@(65))
    Check ($fake.ReadExisting() -eq 'A') 'reopen clears incomplete bytes from the previous connection'
    Close-MonitorPort $fake

    $script:writer = New-Object System.IO.StringWriter
    $script:pendingLine = ''
    $script:apis = @((New-Object Wqn.SerialMonitor.Tests.CommApi), (New-Object Wqn.SerialMonitor.Tests.CommApi))
    $script:ports = @((New-TestPort $script:apis[0]), (New-TestPort $script:apis[1]))
    $script:apis[0].DisconnectOnRead = $true
    $script:apis[1].Chunks.Enqueue([System.Text.Encoding]::UTF8.GetBytes('I (3) recovered' + [char]10))
    $script:created = 0
    $script:keyCalls = 0
    $keys = New-Object PSObject
    $keys | Add-Member ScriptMethod Poll {
        $script:keyCalls++
        return ($script:keyCalls -le 2)
    }
    $script:captureTimer = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-MonitorLoop {
        $serial = $script:ports[$script:created]
        $script:created++
        return $serial
    } $keys
    Check ($script:created -eq 2 -and $script:apis[0].ResetPulses -eq 0 -and $script:apis[1].ResetPulses -eq 0) 'offline requests are discarded and real backend reconnect does not reset'
    Check ($script:writer.ToString() -match 'I \(3\) recovered' -and $script:writer.ToString() -notmatch 'reset pulse sent') 'reconnected device logs continue without a synthetic reset'

    $script:writer = New-Object System.IO.StringWriter
    $script:pendingLine = ''
    $DurationSeconds = 1
    $api = New-Object Wqn.SerialMonitor.Tests.CommApi
    $fake = New-TestPort $api
    $script:keyCalls = 0
    $keys = New-Object PSObject
    $keys | Add-Member ScriptMethod Poll {
        $script:keyCalls++
        return ($script:keyCalls -eq 3 -or $script:keyCalls -eq 4)
    }
    $script:captureTimer = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-MonitorLoop { return $fake } $keys
    Check ($api.ResetPulses -eq 1 -and ([regex]::Matches($script:writer.ToString(), 'source=manual')).Count -eq 1) 'connected F2 resets once and keys queued during the pulse are discarded'

    $script:writer = New-Object System.IO.StringWriter
    $script:pendingLine = ''
    $api = New-Object Wqn.SerialMonitor.Tests.CommApi
    $fake = New-TestPort $api
    $keys = New-Object PSObject
    $keys | Add-Member ScriptMethod Poll { throw [System.IO.IOException]::new('console input lost') }
    $script:captureTimer = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-MonitorLoop { return $fake } $keys
    Check (([regex]::Matches($script:writer.ToString(), 'listener attached:')).Count -eq 1 -and $script:writer.ToString() -match 'F2 disabled:' -and $script:writer.ToString() -notmatch 'unavailable:') 'keyboard input failure disables F2 without reopening the serial port'

    $script:writer = New-Object FailingMonitorWriter
    $script:pendingLine = ''
    $script:logFailed = $false
    $api = New-Object Wqn.SerialMonitor.Tests.CommApi
    $fake = New-TestPort $api
    $api.Chunks.Enqueue([System.Text.Encoding]::UTF8.GetBytes('I (4) first device line' + [char]10 + 'I (5) log write must fail' + [char]10))
    $script:captureTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $writeFailed = $false
    try { Invoke-MonitorLoop { return $fake } $null } catch { $writeFailed = $true }
    Check ($writeFailed -and $script:logFailed -and $script:writer.Writes -eq 3 -and -not $fake.IsOpen -and $api.Handle.IsClosed) 'WSL write failure during device output stops capture and releases the port'

    $badLog = Join-Path $testDir 'missing-directory\capture.log'
    $output = & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $monitor -Port 'COM98765' -DurationSeconds 1 -LogPath $badLog 2>&1
    Check ($LASTEXITCODE -ne 0 -and ($output -join [char]10) -notmatch 'unavailable:|listener attached:') 'log creation failure exits before attempting serial connection'

    Write-Host "$passed PASS / 0 FAIL; test artifacts: $testDir"
}
catch {
    Write-Host $_ -ForegroundColor Red
    Write-Host "test artifacts: $testDir"
    exit 1
}
