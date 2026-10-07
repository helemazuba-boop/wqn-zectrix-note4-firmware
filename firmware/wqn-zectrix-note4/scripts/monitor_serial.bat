@echo off
REM Thin .bat launcher for monitor_serial.ps1.
REM Usage:
REM   scripts\monitor_serial.bat                        (COM7 @ 115200, log to scripts\..\logs\)
REM   scripts\monitor_serial.bat COM5
REM   scripts\monitor_serial.bat COM7 921600
REM   scripts\monitor_serial.bat COM7 921600 D:\logs\myrun.log
REM   scripts\monitor_serial.bat COM7 115200 D:\logs\myrun.log 240
REM The optional fourth argument bounds capture in seconds (0 = unlimited).
REM The log path is echoed at startup and repeated on exit, so it can be copied
REM without hunting for it. Device lines are written verbatim (no timestamp
REM prefix) - see the header comment in monitor_serial.ps1 for why.
setlocal
chcp 65001 >nul

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=115200"
set "DURATION=%~4"
if "%DURATION%"=="" set "DURATION=0"

if "%~3"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor_serial.ps1" -Port "%PORT%" -Baud %BAUD% -DurationSeconds "%DURATION%"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor_serial.ps1" -Port "%PORT%" -Baud %BAUD% -LogPath "%~3" -DurationSeconds "%DURATION%"
)
endlocal
