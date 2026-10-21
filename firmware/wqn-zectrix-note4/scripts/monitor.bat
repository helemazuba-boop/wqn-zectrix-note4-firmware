@echo off
setlocal
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"

call "%~dp0monitor_serial.bat" "%PORT%"
exit /b %ERRORLEVEL%
