@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ========== Configuration ==========
:: Firmware lives in WSL now. Build runs in WSL (ESP-IDF is there);
:: flash + monitor stay Windows-native (device on COM7).
::
:: Build uses `bash -c` (NOT `bash -ic`): `bash -ic` sources .bashrc which
:: auto-activates conda base (python 3.13), but the IDF venv is
:: idf5.5_py3.12_env -> export.sh would abort with "venv not found".
:: `bash -c` keeps system python 3.12, which matches the venv.
::
:: Delayed expansion is on: the tree path is normalised with !VAR:old=new!,
:: because %VAR:old=new% cannot contain the %WSL_DISTRO% it has to strip.
set "WSL_DISTRO=Ubuntu"
set "WSL_IDF_EXPORT=~/esp/esp-idf-v5.5/export.sh"
set "BUILD_DIR=build-ai-local-s3"
:: COM7 is the dev port. scripts/deploy_com5.bat presets this to COM5.
if not defined COM_PORT set "COM_PORT=COM7"
set "BAUD=460800"
:: esptool reset mode. default-reset is what ESP-IDF's own `idf.py flash`
:: emits for this board (see build output) and is accepted by esptool v5.2.0.
:: usb_jtag_serial_reset is not a valid v5.2.0 --before value.
set "RESET_BEFORE=default-reset"
set "RESET_AFTER=hard-reset"
:: ===================================

:: ---------- Which tree? ----------
:: Priority: %1 (WSL path or \\wsl.localhost\... UNC) > %FW_DIR% from the
:: environment > interactive menu > this checkout.
::
:: The menu is skipped when stdin is redirected (scheduler, piped runs,
:: cmd /c deploy.bat < nul): timeout fails on redirected input, which is the
:: only reliable console test available in batch.
set "SELF_WSL=%~dp0"
set "SELF_WSL=!SELF_WSL:\\wsl.localhost\%WSL_DISTRO%=!"
set "SELF_WSL=!SELF_WSL:\=/!"
if "!SELF_WSL:~-1!"=="/" set "SELF_WSL=!SELF_WSL:~0,-1!"
set "TARGETS="
set "TREE_COUNT=0"

if not "%~1"=="" (
    set "FW_DIR=%~1"
    goto :resolve_single
)
if defined FW_DIR goto :resolve_single
if /I "%WQN_NO_MENU%"=="1" goto :use_self
timeout /t 0 >nul 2>&1
if errorlevel 1 goto :use_self
call :pick
if errorlevel 1 goto :fail
if !TREE_COUNT! GTR 1 goto :multi
for /f "usebackq delims=" %%P in ("!TARGETS!") do set "FW_DIR=%%P"
goto :resolve_single

:use_self
set "FW_DIR=!SELF_WSL!"

:resolve_single
call :normalize "!FW_DIR!"
if errorlevel 1 goto :fail

echo.
echo ========================================
echo  WQN Note4 Flash Deploy (Unified Script)
echo  Build: WSL  ^|  Flash+Monitor: Windows
echo  Port:  %COM_PORT%
echo  Tree:  !FW_DIR!
echo ========================================
echo.

if /I "%PLAN_ONLY%"=="1" (
    echo   build dir: !BUILD_UNC!
    echo   monitor:   !MONITOR_BAT!
    endlocal
    exit /b 0
)

:: Preflight: refuse COM5 (official-firmware checkpoint) unless the caller
:: opted in. Only scripts/deploy_com5.bat sets ALLOW_COM5, so an ordinary
:: deploy.bat run still cannot overwrite the checkpoint by accident.
if /I "%COM_PORT%"=="COM5" if not "%ALLOW_COM5%"=="1" (
    echo ERROR: COM5 is the official-firmware checkpoint. Refusing to flash COM5.
    echo        Use scripts\deploy_com5.bat if that is really what you want.
    call :pause_if_interactive
    exit /b 1
)

:: Step 0: Kill any existing serial monitor so the COM port is free for flashing.
::          The monitor is a "monitor_serial" cmd window whose CHILD PowerShell
::          actually holds COM open, so we must terminate the whole tree:
::            - taskkill /T matches the window title and kills its children too
::              (plain /F without /T left the port-holding child alive).
::            - The CIM sweep is a fallback that matches cmd.exe/powershell.exe by
::              command line (excluding this script's own PowerShell via $PID).
::          NOTE: the pipes below MUST stay as literal '|' inside the quoted
::          -Command string. A caret '^|' is not an escape inside cmd double
::          quotes; it would be passed verbatim to PowerShell and abort the whole
::          command with a parse error (that was the previous bug).
echo [Step 0] Stopping existing serial monitor...
taskkill /F /T /FI "WINDOWTITLE eq monitor_serial*" >nul 2>&1
powershell.exe -NoProfile -Command "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*monitor_serial*' -and $_.ProcessId -ne $PID -and @('cmd.exe','powershell.exe','pwsh.exe') -contains $_.Name } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
echo   Done.

:: Step 1: Build in WSL. Incremental builds are fast and guarantee the flashed
:: image matches the working tree. Set SKIP_BUILD=1 only when intentionally
:: re-flashing the existing artifacts.
echo.
if "%SKIP_BUILD%"=="1" (
    if not exist "%BUILD_UNC%\flash_args" (
        echo [Step 1] ERROR: SKIP_BUILD=1 but flash_args does not exist.
        call :pause_if_interactive
        exit /b 1
    )
    echo [Step 1] SKIP_BUILD=1; reusing existing build artifacts.
    goto :build_done
)
:do_build
echo [Step 1] Building in WSL ^(ESP-IDF %WSL_IDF_EXPORT%^)...
if not exist "%BUILD_UNC%\CMakeCache.txt" (
    echo         Initializing clean build directory for ESP32-S3...
    wsl -d %WSL_DISTRO% -- bash -c "cd %FW_DIR% && . %WSL_IDF_EXPORT% && idf.py --no-ccache -B %BUILD_DIR% set-target esp32s3"
    if errorlevel 1 (
        echo   ERROR: WSL target initialization failed^!
        call :pause_if_interactive
        exit /b 1
    )
)
:: Build through the release tooling so the firmware gets its content-derived
:: version (WQN_RELEASE_VERSION) and a source diff is recorded under dist/.
:: It runs `idf.py reconfigure` first: a plain build skips cmake when no
:: CMakeLists changed, which would bake in whatever version was configured last.
wsl -d %WSL_DISTRO% -- bash -c "cd %FW_DIR% && . %WSL_IDF_EXPORT% && python3 tools/release/release.py build --build-dir %BUILD_DIR%"
if errorlevel 1 (
    echo   ERROR: WSL build failed^!
    call :pause_if_interactive
    exit /b 1
)
if not exist "%BUILD_UNC%\flash_args" (
    echo   ERROR: build finished but flash_args not found at %BUILD_UNC%\flash_args
    call :pause_if_interactive
    exit /b 1
)
echo   Done.
:build_done

:: Step 1b: Refuse to flash an artifact whose version stamp does not describe it.
::          PROJECT_VER lives in the cmake cache and only changes when cmake
::          reconfigures, which only release.py does on purpose. A bare
::          `idf.py build` therefore relinks new code under the previous
::          version, and the flashed firmware reports a version describing
::          different sources with nothing downstream able to tell. The check
::          reads the artifacts themselves, so it holds whichever build path
::          produced them. Deliberately after :build_done so it also covers
::          SKIP_BUILD=1, which is the case where a stale stamp is likeliest.
echo.
echo [Step 1b] Verifying build artifacts against their baked version...
wsl -d %WSL_DISTRO% -- bash -c "cd %FW_DIR% && . %WSL_IDF_EXPORT% && python3 tools/release/release.py verify-build --build-dir %BUILD_DIR%"
if errorlevel 1 (
    echo   ERROR: build artifacts failed verification -- refusing to flash^!
    echo          Re-run without SKIP_BUILD so the release tooling rebuilds them.
    call :pause_if_interactive
    exit /b 1
)
echo   Done.

:: Step 2: COM port + esptool preflight
echo.
echo [Step 2] Using COM port: %COM_PORT% @ %BAUD% baud
where esptool >nul 2>nul
if errorlevel 1 (
    echo   ERROR: 'esptool' not found on Windows PATH.
    echo          Install with: pip install esptool
    call :pause_if_interactive
    exit /b 1
)
echo.

:: Step 3: Flash (Windows esptool, NO erase so NVS pairing token survives)
echo [Step 3] Flashing all partitions via esptool...
pushd "%BUILD_UNC%"
if errorlevel 1 (
    echo   ERROR: cannot access build dir %BUILD_UNC%
    call :pause_if_interactive
    exit /b 1
)
esptool --chip esp32s3 --port %COM_PORT% --baud %BAUD% --before %RESET_BEFORE% --after %RESET_AFTER% write_flash @flash_args
set "FLASH_RC=%ERRORLEVEL%"
popd
if not "%FLASH_RC%"=="0" (
    echo   ERROR: Flash failed ^(esptool exit %FLASH_RC%^)^!
    wsl -d %WSL_DISTRO% -- bash -c "cd %FW_DIR% && . %WSL_IDF_EXPORT% && python3 tools/release/release.py record-flash --build-dir %BUILD_DIR% --port %COM_PORT% --status failed"
    call :pause_if_interactive
    exit /b 1
)
:: Append to dist/flash-history.jsonl so we can always answer "which source is
:: on this device right now" -- the version alone is not enough, the tree is
:: usually dirty when we flash.
wsl -d %WSL_DISTRO% -- bash -c "cd %FW_DIR% && . %WSL_IDF_EXPORT% && python3 tools/release/release.py record-flash --build-dir %BUILD_DIR% --port %COM_PORT% --status ok"
echo   Done.

echo.
echo ========================================
echo  Flash complete^!
echo ========================================
echo.

:: Step 4: Open serial monitor (new window, won't close when deploy.bat exits).
::         Skipped for the extra trees of a multi-tree run: one monitor on the
::         port is enough and the last image is the one worth watching.
if defined WQN_CHILD goto :done
echo [Step 4] Opening serial monitor on %COM_PORT%...
if not exist "%MONITOR_BAT%" (
    echo   WARNING: %MONITOR_BAT% not found; open the monitor manually.
    goto :done
)
start "monitor_serial" cmd /c ""%MONITOR_BAT%" %COM_PORT%"
echo   Monitor opened in new window.
echo.

:done
echo Deploy complete. You can close this window.
call :pause_if_interactive
endlocal
exit /b 0

:fail
echo.
echo Deploy aborted.
call :pause_if_interactive
endlocal
exit /b 1

:: ---------- subroutines ----------

:pause_if_interactive
if not defined WQN_CHILD pause
exit /b 0

:normalize
:: Accepts a WSL path (/home/...) or a \\wsl.localhost\<distro>\... UNC, and
:: fills in the Windows-side build dir plus the monitor launcher.
set "FW_DIR=%~1"
set "FW_DIR=!FW_DIR:\\wsl.localhost\%WSL_DISTRO%=!"
set "FW_DIR=!FW_DIR:\=/!"
if "!FW_DIR:~-1!"=="/" set "FW_DIR=!FW_DIR:~0,-1!"
if not "!FW_DIR:~0,1!"=="/" set "FW_DIR=/!FW_DIR!"
set "FW_UNC=\\wsl.localhost\%WSL_DISTRO%!FW_DIR:/=\!"
set "BUILD_UNC=!FW_UNC!\%BUILD_DIR%"
set "MONITOR_BAT=!FW_UNC!\scripts\monitor_serial.bat"
if not exist "!FW_UNC!\CMakeLists.txt" (
    echo.
    echo ERROR: not a firmware tree: !FW_DIR!
    echo        missing !FW_UNC!\CMakeLists.txt
    exit /b 1
)
exit /b 0

:pick
:: Choose one or more trees from `git worktree list`, most recently modified
:: first. The helper both renders the menu and resolves the numbers, so the
:: index the user sees is the index that gets resolved.
set "TARGETS=%TEMP%\wqn-deploy-targets-%RANDOM%%RANDOM%.txt"
set "HELPER=!SELF_WSL!/scripts/list_worktrees.py"
echo.
echo Scanning git worktrees...
wsl -d %WSL_DISTRO% -- python3 "!HELPER!" > "!TARGETS!" 2>&1 < nul
if errorlevel 1 (
    echo.
    echo ERROR: cannot list worktrees:
    type "!TARGETS!"
    exit /b 1
)
echo.
type "!TARGETS!"
echo.
echo Several may be picked (e.g. 2,4): they are built and flashed in order,
echo and the device ends up running the last one.
set "PICK="
set /p "PICK=Select tree [1-9, comma for several, Enter = 1]: "
if not defined PICK set "PICK=1"
wsl -d %WSL_DISTRO% -- python3 "!HELPER!" --resolve "!PICK!" > "!TARGETS!" 2>&1 < nul
if errorlevel 1 (
    echo.
    echo ERROR: bad selection:
    type "!TARGETS!"
    exit /b 1
)
set "TREE_COUNT=0"
for /f "usebackq delims=" %%P in ("!TARGETS!") do set /a TREE_COUNT+=1
if !TREE_COUNT! LSS 1 (
    echo.
    echo ERROR: no tree selected.
    exit /b 1
)
exit /b 0

:multi
:: Several trees: run this script once per tree, in the order shown. Each run
:: is a full build + flash, so the device ends up on the last one.
echo.
echo Will build and flash !TREE_COUNT! trees in this order:
set "N=0"
for /f "usebackq delims=" %%P in ("!TARGETS!") do (
    set /a N+=1
    set "SHOWN=%%P"
    echo   !N!. !SHOWN!
)
echo.
echo The device ends up running the LAST one. Each tree is a full build.
echo Press Ctrl+C now to stop.
set "WQN_CHILD=1"
for /f "usebackq delims=" %%P in ("!TARGETS!") do (
    echo.
    echo ==================== %%P ====================
    call "%~f0" "%%P"
    if errorlevel 1 (
        set "WQN_CHILD="
        echo.
        echo ERROR: stopped after a failure on %%P
        goto :fail
    )
)
set "WQN_CHILD="
echo.
echo All !TREE_COUNT! trees flashed; the device is running the last one.
goto :done
