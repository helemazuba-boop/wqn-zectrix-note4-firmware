# No-reset serial monitor for the Note4 native USB-Serial-JTAG. It mirrors the
# safe DTR/RTS open ordering used by `idf.py monitor --no-reset`, without Python
# or ESP-IDF dependencies. It auto-reopens the port after a device-initiated
# reset or USB re-enumeration without intentionally resetting the target.
#
# Every line is also written to a log file, so a session no longer has to be
# copied out of the console by hand.
# Optional -DurationSeconds bounds unattended capture (0 keeps the interactive
# default). The timer is monotonic and stopping capture does not reset the chip.
#
#   * Device lines are written **verbatim, with no prefix**. The judge
#     (`scripts/hil_check.py`) anchors its timestamp at column 0
#     (`LOG_PREFIX_RE = ^[IWEDV] \((\d+)\)`), so prefixing a device line here
#     would silently break every criterion while the console still looks fine.
#   * Only this script's own status lines carry a `[HH:mm:ss]` wall-clock
#     prefix. A judge *can* parse those, so every status line is worded to name
#     what it actually is: "listener attached: <port>", never a bare
#     "opened COM7". The boot-cycle judge once counted `opened COM` and read
#     listener attaches as cold boots -- silently, because in most logs the two
#     counts happen to be equal and only a crash log diverges.
#   * The file is UTF-8 **without** a BOM and is flushed after every line, so a
#     hard Ctrl+C or a crash costs at most one line.
#   * "Port unavailable" retries are logged too. Without them a run where the
#     port never opened produces a 0-byte log and a report that says "0 lines"
#     with no stated cause.
[CmdletBinding()]
param(
    [string]$Port = 'COM7',
    [int]$Baud = 115200,
    [switch]$ResetOnStart,
    [string]$LogPath,
    [ValidateRange(0, 86400)]
    [int]$DurationSeconds = 0
)

# UTF-8 so Chinese log strings render correctly in the console.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if (-not $LogPath) {
    $stamp = Get-Date -Format 'yyMMdd-HHmmss'
    $logDir = Join-Path $PSScriptRoot '..\logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    # FullName, not Resolve-Path().Path: the latter comes back provider-qualified
    # ("Microsoft.PowerShell.Core\FileSystem::\\wsl.localhost\...") and every
    # System.IO API then rejects it. Verified on 2026-10-06 by running this
    # against a nonexistent port.
    $logDir = (Get-Item $logDir).FullName
    $LogPath = Join-Path $logDir "serial-$Port-$stamp.log"
}
elseif ($PSBoundParameters.ContainsKey('LogPath')) {
    try { $LogPath = [System.IO.Path]::GetFullPath($LogPath) } catch {}
}

# No BOM: a BOM would put a stray character in front of the first device line.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$writer = $null
$logStream = $null
try {
    # Atomic create, not a Test-Path + overwrite race: evidence already on disk
    # must survive a typo, repeated command or concurrent listener.
    $logStream = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    $writer = New-Object System.IO.StreamWriter($logStream, $utf8NoBom)
}
catch {
    if ($null -ne $logStream) { $logStream.Dispose() }
    Write-Host "Cannot open log file '$LogPath': $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
# Without this a bad log path leaves the monitor running and the console looking
# normal while nothing is written anywhere - the exact failure this script
# exists to remove. Fail loudly instead.
if ($null -eq $writer) {
    Write-Host "Log handle is null for '$LogPath'. Refusing to monitor without a log." -ForegroundColor Red
    exit 1
}

function Say([string]$text) {
    $stamped = '[' + (Get-Date -Format 'HH:mm:ss') + '] ' + $text
    Write-Host $stamped
    $writer.WriteLine($stamped)
    $writer.Flush()
}

$captureTimer = [System.Diagnostics.Stopwatch]::StartNew()
function CaptureActive {
    return ($DurationSeconds -eq 0 -or $captureTimer.Elapsed.TotalSeconds -lt $DurationSeconds)
}

try {
    Say "monitoring $Port @ $Baud bps, log: $LogPath"
    Write-Host "Monitoring $Port @ $Baud bps. Press Ctrl+C to quit." -ForegroundColor Cyan
    Write-Host "(Port will auto-reopen if the device resets.)" -ForegroundColor DarkGray
    Write-Host ""

    $resetPending = $ResetOnStart.IsPresent

    while (CaptureActive) {
        $sp = $null
        try {
            $sp = New-Object System.IO.Ports.SerialPort `
                $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
            # Match ESP-IDF Monitor's --no-reset open sequence. On Windows native
            # USB-Serial-JTAG, opening with both lines deasserted causes
            # USB_UART_CHIP_RESET (0x15). Stage both asserted before Open(), then
            # release RTS first and DTR second; --no-reset skips the later pulse.
            $sp.DtrEnable = $true
            $sp.RtsEnable = $true
            $sp.ReadTimeout = 500
            $sp.Encoding = [System.Text.Encoding]::UTF8
            $sp.NewLine = "`n"
            $sp.Open()

            # usbser.sys applies DTR state when RTS changes. Re-apply DTR between
            # the ordered releases, mirroring ESP-IDF's Windows workaround.
            $sp.RtsEnable = $false
            $sp.DtrEnable = $true
            $sp.DtrEnable = $false

            # Deliberately NOT "opened $Port". A bare "opened COM7" looks exactly
            # like a device event, and a judge that counts boot cycles with
            # text.count('opened COM') then counts listener attaches instead --
            # silently, because in most logs the two numbers happen to be equal.
            # The judge only diverges on a crash log. Name it for what it is.
            Say "listener attached: $Port"

            if ($resetPending) {
                # With DTR held inactive, a short RTS pulse performs the same
                # application reset used by esptool without selecting download
                # mode. Keep the port open so the complete boot log is captured.
                $resetPending = $false
                $sp.RtsEnable = $true
                Start-Sleep -Milliseconds 100
                $sp.RtsEnable = $false
                Say "reset pulse sent"
            }

            while ($sp.IsOpen -and (CaptureActive)) {
                try {
                    $line = $sp.ReadLine()
                }
                catch [System.TimeoutException] {
                    # No data this tick - keep polling. Ctrl+C still interrupts
                    # cleanly between reads.
                    continue
                }
                catch {
                    # Other I/O errors (port disappeared on device reset, USB
                    # re-enumeration, etc.) - fall through to the outer retry loop.
                    throw
                }
                # ESP-IDF logs use CRLF; strip the lone \r so the console and the
                # log file don't double-space. Write verbatim: no timestamp, no
                # tag, nothing that would break a column-0 anchored parser.
                $clean = $line.TrimEnd("`r")
                Write-Host $clean
                $writer.WriteLine($clean)
                $writer.Flush()
            }
        }
        catch {
            Say "$Port unavailable: $($_.Exception.Message). Retrying in 1s..."
        }
        finally {
            if ($sp) {
                if ($sp.IsOpen) { try { $sp.Close() } catch {} }
                $sp.Dispose()
            }
        }
        if (CaptureActive) { Start-Sleep -Seconds 1 }
    }
    Say "capture duration limit reached: $DurationSeconds s"
}
finally {
    # Runs on a clean exit. A hard Ctrl+C kill may not reach this, which is why
    # every line is flushed as it is written instead of at the end.
    Say "monitor stopped"
    try { $writer.Flush() } catch {}
    try { $writer.Close() } catch {}
    try { $writer.Dispose() } catch {}
    Write-Host "Log: $LogPath" -ForegroundColor DarkGray
}
