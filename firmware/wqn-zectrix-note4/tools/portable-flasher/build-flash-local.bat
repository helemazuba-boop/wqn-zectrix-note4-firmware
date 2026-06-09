@echo off
setlocal EnableExtensions

set "EXIT_CODE=0"

set "SCRIPT_DIR=%~dp0"
set "PACKAGE_SCRIPT=%SCRIPT_DIR%build-package-upload.bat"
for %%I in ("%SCRIPT_DIR%..\..") do set "PROJECT_DIR=%%~fI"
cd /d "%PROJECT_DIR%" || goto fail

if "%BUILD_DIR%"=="" set "BUILD_DIR=build-ai-local-s3"
if "%PORT%"=="" set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"
if "%BAUD%"=="" set "BAUD=921600"
if "%IDF_ROOT%"=="" set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
if "%IDF_TOOLS_PATH%"=="" set "IDF_TOOLS_PATH=D:\Program\Espressif"

if /I "%PORT%"=="COM5" (
  echo ERROR: COM5 is the official firmware checkpoint. Refusing to flash COM5.
  goto fail
)

echo Local WQN Note4 deploy
echo Project: %PROJECT_DIR%
echo Build dir: %BUILD_DIR%
echo Port: %PORT%
echo Baud: %BAUD%
echo.

echo [1/3] Loading ESP-IDF environment...
if not exist "%IDF_ROOT%\export.bat" (
  echo ERROR: ESP-IDF export.bat not found: "%IDF_ROOT%\export.bat"
  goto fail
)
call "%IDF_ROOT%\export.bat"
if errorlevel 1 goto fail

echo [2/3] Building firmware...
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

if /I "%BUILD_ONLY%"=="1" (
  echo Build only requested. Flash step skipped.
  goto success
)

echo [3/3] Flashing %PORT%...
idf.py -p "%PORT%" -b "%BAUD%" -B "%BUILD_DIR%" flash
if errorlevel 1 goto fail

goto success

:success
echo Local deploy complete.
echo.
echo To build and upload a portable Windows flasher bundle now, type PACKAGE and press Enter.
echo Press Enter to skip.
set "PACKAGE_CONFIRM="
set /p PACKAGE_CONFIRM=Package upload: 
if /I "%PACKAGE_CONFIRM%"=="PACKAGE" (
  if not exist "%PACKAGE_SCRIPT%" (
    echo ERROR: Package upload script not found: "%PACKAGE_SCRIPT%"
    goto fail
  )
  echo.
  echo Starting portable package upload...
  call "%PACKAGE_SCRIPT%"
  if errorlevel 1 goto fail
)
set "EXIT_CODE=0"
goto done

:fail
set "EXIT_CODE=1"
echo.
echo ERROR: Local deploy failed.
goto done

:done
echo.
pause
exit /b %EXIT_CODE%
