@echo off
setlocal EnableExtensions

:: ========== Configuration ==========
set "PROJECT_DIR=D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"
set "BUILD_DIR=build-ai-local-s3"
set "COM_PORT=COM7"
set "IDF_EXPORT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
:: ===================================

cd /d "%PROJECT_DIR%"

echo.
echo ========================================
echo  WQN Note4 Flash Deploy (Unified Script)
echo ========================================
echo.

:: Step 0: Kill any existing monitor_serial.bat / monitor_serial.ps1
::          so the COM port is free for flashing.
echo [Step 0] Stopping existing serial monitor...
taskkill /F /FI "WINDOWTITLE eq monitor_serial*" >nul 2>&1
:: Also kill any lingering PowerShell monitor by script name
for /f "tokens=2" %%P in ('tasklist /FI "IMAGENAME eq powershell.exe" /FO CSV /NH 2^>nul ^| findstr /I "monitor_serial"') do (
    taskkill /F /PID %%P >nul 2>&1
)
:: Kill by the actual process command line (most reliable)
wmic process where "commandline like '%%monitor_serial%%'" call terminate >nul 2>&1
echo   Done.

:: Step 1: Activate ESP-IDF and build
echo.
echo [Step 1] Building project...
call "%IDF_EXPORT%"
if errorlevel 1 (
    echo   ERROR: Failed to load ESP-IDF environment.
    echo   Check that IDF is installed at %IDF_EXPORT%
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

:: Step 2: COM port
echo.
echo [Step 2] Using COM port: %COM_PORT%
echo.

:: Step 3: Flash (SKIP erase-flash so NVS pairing token survives)
echo [Step 3] Flashing all partitions...
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

:: Step 4: Open serial monitor (new window, won't close when deploy.bat exits)
echo [Step 4] Opening serial monitor on %COM_PORT%...
start "monitor_serial" cmd /c ""%~dp0scripts\monitor_serial.bat" %COM_PORT%"
echo   Monitor opened in new window.
echo.
echo Deploy complete. You can close this window.

:: Keep window open so user sees the result
pause
endlocal
