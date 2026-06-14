@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "BUILD_DIR=build-ai-local-s3"
set "LOG_FILE=serial_log.txt"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo. > "%LOG_FILE%"

echo Waiting 3 seconds for device to stabilize...
timeout /t 3 /nobreak >nul

echo.
echo === Loading ESP-IDF environment ===
call "%IDF_ROOT%\export.bat"
if errorlevel 1 (
    echo ERROR: Failed to load ESP-IDF
    pause
    exit /b 1
)

echo.
echo === Starting serial monitor, logging to %LOG_FILE% ===
echo Press Ctrl+C to stop, then check %LOG_FILE%
echo.

idf.py -p COM7 -B "%BUILD_DIR%" monitor 2>&1 | tee "%LOG_FILE%"
