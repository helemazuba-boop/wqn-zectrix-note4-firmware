@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "PROJECT_DIR=%%~fI"
cd /d "%PROJECT_DIR%" || exit /b 1

if "%BUILD_DIR%"=="" set "BUILD_DIR=build-ai-local-s3"
if "%SSH_HOST%"=="" set "SSH_HOST=aliyun"
if "%REMOTE_DIR%"=="" set "REMOTE_DIR=/www/wwwroot/alist_storage/WQN Deck"
if "%IDF_ROOT%"=="" set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
if "%IDF_TOOLS_PATH%"=="" set "IDF_TOOLS_PATH=D:\Program\Espressif"

echo [1/4] Loading ESP-IDF environment...
if not exist "%IDF_ROOT%\export.bat" (
  echo ERROR: ESP-IDF export.bat not found: "%IDF_ROOT%\export.bat"
  exit /b 1
)
call "%IDF_ROOT%\export.bat"
if errorlevel 1 exit /b 1

echo [2/4] Building firmware into %BUILD_DIR%...
idf.py --no-ccache -B "%BUILD_DIR%" build
if errorlevel 1 exit /b 1

echo [3/4] Preparing portable esptool.exe...
python "%SCRIPT_DIR%ensure_portable_esptool.py" --output "%SCRIPT_DIR%.cache\esptool.exe"
if errorlevel 1 exit /b 1

echo [4/4] Packaging and uploading flasher bundle...
python "%SCRIPT_DIR%package_flasher.py" --build-dir "%BUILD_DIR%" --esptool-exe "%SCRIPT_DIR%.cache\esptool.exe" --ssh-host "%SSH_HOST%" --remote-dir "%REMOTE_DIR%" --upload
if errorlevel 1 exit /b 1

echo Done.

