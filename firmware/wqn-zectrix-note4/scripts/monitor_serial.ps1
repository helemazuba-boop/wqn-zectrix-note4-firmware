# Serial monitor for the Note4 USB-Serial-JTAG, intended as a replacement for
# `idf.py monitor` (broken on this board's native USB-Serial-JTAG path per
# BUILD_HERE.md) and the legacy `listen_usb.py`. Pure PowerShell, no Python
# or ESP-IDF deps. Auto-reopens the port when the device resets and the JTAG
# briefly vanishes.
[CmdletBinding()]
param(
    [string]$Port = 'COM7',
    [int]$Baud = 115200,
    [switch]$ResetOnStart
)

# UTF-8 so Chinese log strings render correctly in the console.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "Monitoring $Port @ $Baud bps. Press Ctrl+C to quit." -ForegroundColor Cyan
Write-Host "(Port will auto-reopen if the device resets.)" -ForegroundColor DarkGray
Write-Host ""

$resetPending = $ResetOnStart.IsPresent

while ($true) {
    $sp = $null
    try {
        $sp = New-Object System.IO.Ports.SerialPort `
            $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
        # We only want to *read* — do not let DTR/RTS toggle the boot pins on
        # open. ESP32-S3 USB-Serial-JTAG can re-enter download mode otherwise.
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        $sp.ReadTimeout = 500
        $sp.Encoding = [System.Text.Encoding]::UTF8
        $sp.NewLine = "`n"
        $sp.Open()

        $ts = Get-Date -Format 'HH:mm:ss'
        Write-Host "[$ts] opened $Port" -ForegroundColor Green

        if ($resetPending) {
            # With DTR held inactive, a short RTS pulse performs the same
            # application reset used by esptool without selecting download
            # mode. Keep the port open so the complete boot log is captured.
            $resetPending = $false
            $sp.RtsEnable = $true
            Start-Sleep -Milliseconds 100
            $sp.RtsEnable = $false
            Write-Host "[$ts] reset pulse sent" -ForegroundColor DarkGray
        }

        while ($sp.IsOpen) {
            try {
                $line = $sp.ReadLine()
                # ESP-IDF logs use CRLF; strip the lone \r so the console
                # doesn't double-space.
                Write-Host ($line.TrimEnd("`r"))
            }
            catch [System.TimeoutException] {
                # No data this tick — keep polling. Ctrl+C still interrupts
                # cleanly between reads.
                continue
            }
            catch {
                # Other I/O errors (port disappeared on device reset, USB
                # re-enumeration, etc.) — fall through to the outer retry loop.
                throw
            }
        }
    }
    catch {
        $ts = Get-Date -Format 'HH:mm:ss'
        Write-Host "[$ts] $Port unavailable: $($_.Exception.Message). Retrying in 1s..." -ForegroundColor Yellow
    }
    finally {
        if ($sp) {
            if ($sp.IsOpen) { try { $sp.Close() } catch {} }
            $sp.Dispose()
        }
    }
    Start-Sleep -Seconds 1
}
