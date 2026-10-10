# No-reset serial monitor for the Note4 native USB-Serial-JTAG. It mirrors the
# safe DTR/RTS open ordering used by `idf.py monitor --no-reset`, without Python
# or ESP-IDF dependencies. It auto-reopens the port after a device-initiated
# reset or USB re-enumeration without intentionally resetting the target.
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
