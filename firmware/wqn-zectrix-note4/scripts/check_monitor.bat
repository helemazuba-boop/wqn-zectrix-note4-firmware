@echo off
setlocal EnableExtensions
set "LOG_FILE=serial_log.txt"

pushd "%~dp0.."
if errorlevel 1 exit /b 1

echo. > "%LOG_FILE%"

echo Waiting 3 seconds for device to stabilize...
timeout /t 3 /nobreak >nul

echo.
echo === Starting serial monitor, logging to %LOG_FILE% ===
echo Press Ctrl+C to stop, then check %LOG_FILE%
echo.

call "%~dp0monitor_serial.bat" COM7 2>&1 | tee "%LOG_FILE%"
set "MONITOR_RC=%ERRORLEVEL%"
popd
exit /b %MONITOR_RC%
