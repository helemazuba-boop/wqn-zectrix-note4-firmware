@echo off
setlocal EnableExtensions

:: ========== Configuration ==========
:: Firmware lives in WSL now. Build runs in WSL (ESP-IDF is there);
:: flash + monitor stay Windows-native (device on COM7).
::
:: Build uses `bash -c` (NOT bash -ic): `bash -ic` sources .bashrc which
:: auto-activates conda base (python 3.13), but the IDF venv is
:: idf5.5_py3.12_env -> export.sh would abort with "venv not found".
:: `bash -c` keeps system python 3.12, which matches the venv.
set "WSL_DISTRO=Ubuntu"
set "WSL_FW_DIR=/home/unknow/projects/firmware/firmware/wqn-zectrix-note4"
set "WSL_IDF_EXPORT=~/esp/esp-idf-v5.5/export.sh"
set "BUILD_DIR=build-ai-local-s3"
set "BUILD_UNC=\\wsl.localhost\Ubuntu\home\unknow\projects\firmware\firmware\wqn-zectrix-note4\build-ai-local-s3"
set "COM_PORT=COM7"
set "BAUD=460800"
:: esptool reset mode. default-reset is what ESP-IDF's own `idf.py flash`
:: emits for this board (see build output) and is accepted by esptool v5.2.0.
:: usb_jtag_serial_reset is not a valid v5.2.0 --before value.
set "RESET_BEFORE=default-reset"
set "RESET_AFTER=hard-reset"
:: ===================================

echo.
echo ========================================
echo  WQN Note4 Flash Deploy (Unified Script)
echo  Build: WSL  ^|  Flash+Monitor: Windows
echo ========================================
echo.

:: Preflight: refuse COM5 (official-firmware checkpoint)
if /I "%COM_PORT%"=="COM5" (
    echo ERROR: COM5 is the official-firmware checkpoint. Refusing to flash COM5.
    pause
    exit /b 1
)

:: Step 0: Kill any existing monitor_serial.bat / monitor_serial.ps1
::          so the COM port is free for flashing.
echo [Step 0] Stopping existing serial monitor...
taskkill /F /FI "WINDOWTITLE eq monitor_serial*" >nul 2>&1
powershell.exe -NoProfile -Command ^
    "Get-CimInstance Win32_Process ^| Where-Object { $_.CommandLine -like '*monitor_serial*' -and $_.ProcessId -ne $PID } ^| ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
echo   Done.

:: Step 1: Build in WSL. Incremental builds are fast and guarantee the flashed
:: image matches the working tree. Set SKIP_BUILD=1 only when intentionally
:: re-flashing the existing artifacts.
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

:: Step 2: COM port + esptool preflight
echo.
echo [Step 2] Using COM port: %COM_PORT% @ %BAUD% baud
where esptool >nul 2>nul
if errorlevel 1 (
    echo   ERROR: 'esptool' not found on Windows PATH.
    echo          Install with: pip install esptool
    pause
    exit /b 1
)
echo.

:: Step 3: Flash (Windows esptool, NO erase so NVS pairing token survives)
echo [Step 3] Flashing all partitions via esptool...
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
pause
endlocal
