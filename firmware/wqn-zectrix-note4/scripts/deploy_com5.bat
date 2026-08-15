@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ============================================================
::  WQN Note4 Flash Deploy (COM5 Dedicated Script)
::  Build: WSL  |  Flash+Monitor: Windows  |  Auto Full Backup
:: ============================================================

set "WSL_DISTRO=Ubuntu"
set "WSL_FW_DIR=/home/unknow/projects/firmware/firmware/wqn-zectrix-note4"
set "WSL_IDF_EXPORT=~/esp/esp-idf-v5.5/export.sh"
set "BUILD_DIR=build-ai-local-s3"
set "BUILD_UNC=\\wsl.localhost\Ubuntu\home\unknow\projects\firmware\firmware\wqn-zectrix-note4\build-ai-local-s3"
set "BACKUP_DIR=\\wsl.localhost\Ubuntu\home\unknow\projects\firmware\firmware\wqn-zectrix-note4\backups"
set "COM_PORT=COM5"
set "BAUD=460800"
set "FLASH_SIZE=0x1000000"
set "RESET_BEFORE=default-reset"
set "RESET_AFTER=hard-reset"

echo.
echo ============================================================
echo   WQN Note4 Flash Deploy (COM5 Dedicated) + Full Backup
echo   Port: %COM_PORT%  ^|  Baud: %BAUD%  ^|  Flash: 16MB
echo ============================================================
echo.

:: Step 0: Stop existing serial monitor
echo [Step 0] Stopping existing serial monitor...
taskkill /F /T /FI "WINDOWTITLE eq monitor_serial*" >nul 2>&1
powershell.exe -NoProfile -Command "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*monitor_serial*' -and $_.ProcessId -ne $PID -and @('cmd.exe','powershell.exe','pwsh.exe') -contains $_.Name } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
echo   Done.

:: Step 1: Build in WSL
echo.
if "%SKIP_BUILD%"=="1" (
    if not exist "%BUILD_UNC%\flash_args" (
        echo [Step 1] ERROR: SKIP_BUILD=1 but flash_args does not exist.
        pause
        exit /b 1
    )
    echo [Step 1] SKIP_BUILD=1; reusing existing build artifacts.
    goto :build_done
)

:do_build
echo [Step 1] Building in WSL ^(ESP-IDF %WSL_IDF_EXPORT%^)...
if not exist "%BUILD_UNC%\CMakeCache.txt" (
    echo         Initializing clean build directory for ESP32-S3...
    wsl -d %WSL_DISTRO% -- bash -c "cd %WSL_FW_DIR% && . %WSL_IDF_EXPORT% && idf.py --no-ccache -B %BUILD_DIR% set-target esp32s3"
    if errorlevel 1 (
        echo   ERROR: WSL target initialization failed^!
        pause
        exit /b 1
    )
)
wsl -d %WSL_DISTRO% -- bash -c "cd %WSL_FW_DIR% && . %WSL_IDF_EXPORT% && idf.py --no-ccache -B %BUILD_DIR% build"
if errorlevel 1 (
    echo   ERROR: WSL build failed^!
    pause
    exit /b 1
)
if not exist "%BUILD_UNC%\flash_args" (
    echo   ERROR: build finished but flash_args not found at %BUILD_UNC%\flash_args
    pause
    exit /b 1
)
echo   Done.
:build_done

:: Step 2: esptool preflight
echo.
echo [Step 2] Checking esptool environment...
where esptool >nul 2>nul
if errorlevel 1 (
    echo   ERROR: 'esptool' not found on Windows PATH.
    echo          Install with: pip install esptool
    pause
    exit /b 1
)
echo   Done.

:: Step 2.5: Full Flash Backup before flashing
echo.
if not exist "%BACKUP_DIR%" (
    mkdir "%BACKUP_DIR%" >nul 2>&1
)

if "%SKIP_BACKUP%"=="1" (
    echo [Step 2.5] SKIP_BACKUP=1; skipping flash backup.
) else (
    for /f "usebackq tokens=*" %%i in (`powershell.exe -NoProfile -Command "Get-Date -Format 'yyyyMMdd_HHmmss'" 2^>nul`) do set "TIMESTAMP=%%i"
    if "!TIMESTAMP!"=="" (
        set "TIMESTAMP=%DATE:~0,4%%DATE:~5,2%%DATE:~8,2%_%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%"
        set "TIMESTAMP=!TIMESTAMP: =0!"
        set "TIMESTAMP=!TIMESTAMP::=!"
    )
    set "BACKUP_FILE=%BACKUP_DIR%\backup_com5_full_!TIMESTAMP!.bin"

    echo [Step 2.5] Performing full 16MB Flash backup for %COM_PORT%...
    echo            Target: !BACKUP_FILE!
    echo            Reading 16MB flash at %BAUD% baud, please wait...

    pushd "%BACKUP_DIR%" >nul 2>&1
    esptool --chip esp32s3 --port %COM_PORT% --baud %BAUD% read_flash 0 %FLASH_SIZE% "!BACKUP_FILE!"
    set "BACKUP_RC=!ERRORLEVEL!"
    popd >nul 2>&1

    if not "!BACKUP_RC!"=="0" (
        echo.
        echo ============================================================
        echo   [ERROR] Full Flash backup failed ^(exit code: !BACKUP_RC!^)^!
        echo   Flash deployment aborted to protect device data.
        echo ============================================================
        pause
        exit /b 1
    )
    echo   [OK] Flash backup completed successfully: !BACKUP_FILE!
)

:: Step 3: Flash all partitions
echo.
echo [Step 3] Flashing all partitions via esptool to %COM_PORT%...
pushd "%BUILD_UNC%"
if errorlevel 1 (
    echo   ERROR: cannot access build dir %BUILD_UNC%
    pause
    exit /b 1
)
esptool --chip esp32s3 --port %COM_PORT% --baud %BAUD% --before %RESET_BEFORE% --after %RESET_AFTER% write_flash @flash_args
set "FLASH_RC=%ERRORLEVEL%"
popd
if not "%FLASH_RC%"=="0" (
    echo   ERROR: Flash failed ^(esptool exit %FLASH_RC%^)^!
    pause
    exit /b 1
)
echo   Done.

echo.
echo ============================================================
echo  Deploy and Flash complete^! (Port: %COM_PORT%)
echo ============================================================
echo.

:: Step 4: Open serial monitor
echo [Step 4] Opening serial monitor on %COM_PORT%...
start "monitor_serial" cmd /c ""%~dp0monitor_serial.bat" %COM_PORT%"
echo   Monitor opened in new window.
echo.

echo All steps complete. You can close this window.
pause
endlocal
