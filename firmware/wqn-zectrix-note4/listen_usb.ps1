# Read a serial port as a stream and write to the console.
# Avoids the PermissionError(13) noise from idf.py monitor on ESP32-S3 native USB-Serial-JTAG.
# We do NOT use System.IO.Ports.SerialPort (fails to load in some PS environments
# because System.IO.Ports is not a default loaded assembly). Instead we use the
# Win32 ReadFile/WriteFile API directly via P/Invoke — that path works on every
# Windows PowerShell and never needs the assembly.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File listen_usb.ps1 -Port COM7 -Baud 115200
#   powershell -ExecutionPolicy Bypass -File listen_usb.ps1 -Port COM7 -Baud 115200 -OutFile wqn.log

param(
    [string]$Port = "COM7",
    [int]$Baud = 115200,
    [string]$OutFile = "",
    [int]$MaxOpenRetries = 8,
    [int]$ReadChunkSize = 4096
)

$ErrorActionPreference = "Stop"

if ($OutFile -ne "") {
    $writer = New-Object System.IO.StreamWriter($OutFile, $true)
    $writer.AutoFlush = $true
}

# --- Win32 P/Invoke (works on every Windows PowerShell 5.x) ---------------
$signature = @"
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern System.IntPtr CreateFileW(
    string lpFileName, uint dwDesiredAccess, uint dwShareMode,
    System.IntPtr lpSecurityAttributes, uint dwCreationDisposition,
    uint dwFlagsAndAttributes, System.IntPtr hTemplateFile);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(System.IntPtr hObject);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool ReadFile(
    System.IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToRead,
    out uint lpNumberOfBytesRead, System.IntPtr lpOverlapped);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GetCommState(System.IntPtr hFile, ref DCB lpDCB);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetCommState(System.IntPtr hFile, ref DCB lpDCB);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetupComm(System.IntPtr hFile, uint dwInQueue, uint dwOutQueue);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetCommTimeouts(System.IntPtr hFile, ref COMMTIMEOUTS lpCommTimeouts);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool PurgeComm(System.IntPtr hFile, uint dwFlags);

[StructLayout(LayoutKind.Sequential)]
public struct DCB {
    public uint DCBLength;
    public uint BaudRate;
    public uint Flags;
    public uint wReserved;
    public ushort XonLim;
    public ushort XoffLim;
    public byte ByteSize;
    public byte Parity;
    public byte StopBits;
    public byte XonChar;
    public byte XoffChar;
    public byte ErrorChar;
    public byte EofChar;
    public byte EvtChar;
    public ushort wReserved1;
}

[StructLayout(LayoutKind.Sequential)]
public struct COMMTIMEOUTS {
    public uint ReadIntervalTimeout;
    public uint ReadTotalTimeoutMultiplier;
    public uint ReadTotalTimeoutConstant;
    public uint WriteTotalTimeoutMultiplier;
    public uint WriteTotalTimeoutConstant;
}
"@

$Win32 = Add-Type -MemberDefinition $signature -Name "Win32Serial" -Namespace "WQN" -PassThru -Using System.Runtime.InteropServices

$GENERIC_READ = 0x80000000
$GENERIC_WRITE = 0x40000000
$OPEN_EXISTING = 3
$INVALID_HANDLE_VALUE = [System.IntPtr]::new(-1)

function Open-SerialPort {
    param([string]$Name, [int]$BaudRate)
    $portName = "\\.\" + $Name   # required for COM10+ and harmless for COM1-9
    $h = [WQN.Win32Serial]::CreateFileW(
        $portName,
        ($GENERIC_READ -bor $GENERIC_WRITE),
        0,                       # dwShareMode = 0 means exclusive (avoid clashes with other tools)
        [System.IntPtr]::Zero,
        $OPEN_EXISTING,
        0,
        [System.IntPtr]::Zero
    )
    if ($h -eq $INVALID_HANDLE_VALUE -or $h -eq [System.IntPtr]::Zero) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "CreateFileW($portName) failed (win32 error $err)"
    }

    [WQN.Win32Serial]::SetupComm($h, 16384, 16384) | Out-Null

    $dcb = New-Object WQN.Win32Serial+DCB
    $dcb.DCBLength = [System.Runtime.InteropServices.Marshal]::SizeOf($dcb)
    if (-not [WQN.Win32Serial]::GetCommState($h, [ref]$dcb)) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [WQN.Win32Serial]::CloseHandle($h) | Out-Null
        throw "GetCommState failed (win32 error $err)"
    }
    $dcb.BaudRate = [uint32]$BaudRate
    $dcb.ByteSize = 8
    $dcb.Parity   = 0
    $dcb.StopBits = 0
    $dcb.Flags    = 0x00000001  # fBinary
    if (-not [WQN.Win32Serial]::SetCommState($h, [ref]$dcb)) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [WQN.Win32Serial]::CloseHandle($h) | Out-Null
        throw "SetCommState failed (win32 error $err)"
    }

    $timeouts = New-Object WQN.Win32Serial+COMMTIMEOUTS
    $timeouts.ReadIntervalTimeout         = 50
    $timeouts.ReadTotalTimeoutMultiplier  = 0
    $timeouts.ReadTotalTimeoutConstant    = 50
    $timeouts.WriteTotalTimeoutMultiplier = 0
    $timeouts.WriteTotalTimeoutConstant   = 0
    [WQN.Win32Serial]::SetCommTimeouts($h, [ref]$timeouts) | Out-Null
    [WQN.Win32Serial]::PurgeComm($h, 0x00000004) | Out-Null  # PURGE_RXCLEAR

    return $h
}

function Close-SerialPort {
    param([System.IntPtr]$h)
    if ($h -ne [System.IntPtr]::Zero -and $h -ne $INVALID_HANDLE_VALUE) {
        [WQN.Win32Serial]::CloseHandle($h) | Out-Null
    }
}

function Read-SerialPort {
    param([System.IntPtr]$h, [int]$Size)
    $buf = New-Object byte[] $Size
    $n = [uint32]0
    $ok = [WQN.Win32Serial]::ReadFile($h, $buf, [uint32]$Size, [ref]$n, [System.IntPtr]::Zero)
    if (-not $ok) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        # 995 = ERROR_OPERATION_ABORTED (handle was closed)
        # 6   = ERROR_INVALID_HANDLE
        if ($err -eq 995 -or $err -eq 6) { return $null }
        throw "ReadFile failed (win32 error $err)"
    }
    if ($n -eq 0) { return @() }
    return $buf[0..([int]$n - 1)]
}

$totalBytes = 0
$openRetries = 0

while ($true) {
    $h = [System.IntPtr]::Zero
    try {
        $h = Open-SerialPort -Name $Port -BaudRate $Baud
        $openRetries = 0
        $banner = "[listen_usb] Opened ${Port} @ ${Baud} 8-N-1 (Ctrl+C to stop)"
        Write-Host $banner -ForegroundColor Cyan
        if ($OutFile -ne "") { $writer.WriteLine($banner) }
    } catch {
        $openRetries++
        if ($openRetries -gt $MaxOpenRetries) {
            Write-Host "[listen_usb] Failed to open ${Port} after ${MaxOpenRetries} attempts: $($_.Exception.Message)" -ForegroundColor Red
            if ($OutFile -ne "") { $writer.WriteLine("[listen_usb] ABORT: $($_.Exception.Message)"); $writer.Close() }
            exit 1
        }
        Write-Host "[listen_usb] open attempt $openRetries failed: $($_.Exception.Message); retrying in 1s..." -ForegroundColor Yellow
        Start-Sleep -Seconds 1
        continue
    }

    try {
        while ($true) {
            $data = Read-SerialPort -h $h -Size $ReadChunkSize
            if ($data -eq $null) { break }   # handle closed
            if ($data.Count -eq 0) { continue }
            $totalBytes += $data.Count
            $chunk = [System.Text.Encoding]::UTF8.GetString($data)
            Write-Host -NoNewline $chunk
            if ($OutFile -ne "") { $writer.Write($chunk) }
        }
    } finally {
        Close-SerialPort -h $h
    }

    Write-Host ""
    Write-Host "[listen_usb] Port closed (device reboot?). Reopening in 1s..." -ForegroundColor Yellow
    Start-Sleep -Seconds 1
}

