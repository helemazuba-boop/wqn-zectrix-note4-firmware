@echo off
setlocal EnableExtensions

:: ========== Configuration ==========
set "PROJECT_DIR=D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"
set "BUILD_DIR=build-ai-local-s3"
:: ===================================

cd /d "%PROJECT_DIR%"

echo.
echo ========================================
echo  WQN Note4 Flash Deploy (Unified Script)
echo ========================================
echo.

:: Step 1: Build the project (always do this to pick up any code changes)
echo [Step 1] Building project...
call "D:\Program\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
if errorlevel 1 (
    echo   ERROR: Failed to load ESP-IDF environment.
    echo   Check that IDF is installed at D:\Program Files\Espressif\frameworks\esp-idf-v5.5.4
    pause
    exit /b 1
)
idf.py -B "%BUILD_DIR%" build
if errorlevel 1 (
    echo   ERROR: Build failed^!
    pause
    exit /b 1
)
echo   Done.

:: Step 2: Prompt for COM port
echo.
set "COM_PORT=COM7"

echo.
echo [Step 2] Using COM port: %COM_PORT%
echo.

:: Step 3: Erase entire flash (this takes ~30 seconds)...
echo [Step 3] Erasing entire flash (this takes ~30 seconds)...
idf.py -p %COM_PORT% -B "%BUILD_DIR%" erase-flash
if errorlevel 1 (
    echo   ERROR: Erase failed^!
    pause
    exit /b 1
)
echo   Done.

:: Step 4: Flash all partitions
echo.
echo [Step 4] Flashing all partitions...
idf.py -p %COM_PORT% -B "%BUILD_DIR%" flash
if errorlevel 1 (
    echo   ERROR: Flash failed^!
    pause
    exit /b 1
)
echo   Done.

echo.
echo ========================================
echo  Flash complete^!
echo ========================================
echo.

:: Step 5: Open a plain serial listener (idf.py monitor is broken on Native USB-Serial-JTAG)
echo [Step 5] Opening plain serial listener on %COM_PORT% @ 115200...
echo   Reading from %COM_PORT% and writing to wqn.log.
echo   Press Ctrl+C in this window to stop.
echo.

python "%~dp0listen_usb.py" -p %COM_PORT% -b 115200 -o "%~dp0wqn.log"
exit /b %ERRORLEVEL%
