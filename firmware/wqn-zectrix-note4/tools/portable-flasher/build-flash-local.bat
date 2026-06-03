@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "PROJECT_DIR=%%~fI"
cd /d "%PROJECT_DIR%" || exit /b 1

if "%BUILD_DIR%"=="" set "BUILD_DIR=build-ai-local-s3"
if "%PORT%"=="" set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"
if "%BAUD%"=="" set "BAUD=921600"
if "%IDF_ROOT%"=="" set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
if "%IDF_TOOLS_PATH%"=="" set "IDF_TOOLS_PATH=D:\Program\Espressif"

if /I "%PORT%"=="COM5" (
  echo ERROR: COM5 is the official firmware checkpoint. Refusing to flash COM5.
  exit /b 1
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
  exit /b 1
)
call "%IDF_ROOT%\export.bat"
if errorlevel 1 exit /b 1

echo [2/3] Building firmware...
idf.py --no-ccache -B "%BUILD_DIR%" build
if errorlevel 1 exit /b 1

echo [3/3] Flashing %PORT%...
idf.py -p "%PORT%" -b "%BAUD%" -B "%BUILD_DIR%" flash
if errorlevel 1 exit /b 1

echo Local deploy complete.

