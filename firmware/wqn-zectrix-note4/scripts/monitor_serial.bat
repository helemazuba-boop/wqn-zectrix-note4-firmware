@echo off
REM Native USB-Serial-JTAG monitor using the ESP-IDF --no-reset DTR/RTS order.
REM USB_UART_CHIP_RESET (0x15) destroys the ESP32-S3 RTC slow-memory history.
REM Usage:
REM   scripts\monitor_serial.bat                (defaults: COM7 @ 115200)
REM   scripts\monitor_serial.bat COM7
REM   scripts\monitor_serial.bat COM7 921600
setlocal
chcp 65001 >nul

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=115200"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor_serial.ps1" -Port "%PORT%" -Baud %BAUD%
endlocal
