@echo off
setlocal EnableExtensions

set "EXIT_CODE=0"

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "PROJECT_DIR=%%~fI"
cd /d "%PROJECT_DIR%" || goto fail

if "%BUILD_DIR%"=="" set "BUILD_DIR=build-ai-local-s3"
if "%SSH_HOST%"=="" set "SSH_HOST=aliyun"
if "%REMOTE_DIR%"=="" set "REMOTE_DIR=/www/wwwroot/alist_storage/WQN Deck"
if "%IDF_ROOT%"=="" set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
if "%IDF_TOOLS_PATH%"=="" set "IDF_TOOLS_PATH=D:\Program\Espressif"
if /I "%PACKAGE_BUILD_ONLY%"=="1" (
  set "UPLOAD_ARG="
) else (
  set "UPLOAD_ARG=--upload"
)

echo WQN Note4 portable flasher package
echo Project: %PROJECT_DIR%
echo Build dir: %BUILD_DIR%
echo SSH host: %SSH_HOST%
echo Remote dir: %REMOTE_DIR%
if defined UPLOAD_ARG (
  echo Upload: enabled
) else (
  echo Upload: disabled
)
echo.

echo [1/4] Loading ESP-IDF environment...
if not exist "%IDF_ROOT%\export.bat" (
  echo ERROR: ESP-IDF export.bat not found: "%IDF_ROOT%\export.bat"
  goto fail
)
call "%IDF_ROOT%\export.bat"
if errorlevel 1 goto fail

echo [2/4] Building firmware into %BUILD_DIR%...
idf.py --no-ccache -B "%BUILD_DIR%" build
if errorlevel 1 goto fail

set "SDKCONFIG_H=%BUILD_DIR%\config\sdkconfig.h"
if not exist "%SDKCONFIG_H%" (
  echo ERROR: sdkconfig.h not found after build: "%SDKCONFIG_H%"
  goto fail
)
findstr /C:"#define CONFIG_WQN_WIFI_STA_ENABLE 1" "%SDKCONFIG_H%" >nul
if errorlevel 1 (
  echo ERROR: CONFIG_WQN_WIFI_STA_ENABLE is not enabled in "%SDKCONFIG_H%".
  goto fail
)
findstr /C:"#define CONFIG_WQN_AI_ENABLE 1" "%SDKCONFIG_H%" >nul
if errorlevel 1 (
  echo ERROR: CONFIG_WQN_AI_ENABLE is not enabled in "%SDKCONFIG_H%".
  goto fail
)

echo [3/4] Preparing portable esptool.exe...
python "%SCRIPT_DIR%ensure_portable_esptool.py" --output "%SCRIPT_DIR%.cache\esptool.exe"
if errorlevel 1 goto fail

echo [4/4] Packaging and uploading flasher bundle...
python "%SCRIPT_DIR%package_flasher.py" --build-dir "%BUILD_DIR%" --esptool-exe "%SCRIPT_DIR%.cache\esptool.exe" --ssh-host "%SSH_HOST%" --remote-dir "%REMOTE_DIR%" %UPLOAD_ARG%
if errorlevel 1 goto fail

echo Portable package workflow complete.
set "EXIT_CODE=0"
goto done

:fail
set "EXIT_CODE=1"
echo.
echo ERROR: Portable package workflow failed.
goto done

:done
echo.
pause
exit /b %EXIT_CODE%
