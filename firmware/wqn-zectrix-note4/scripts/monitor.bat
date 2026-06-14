@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=D:\Program\Espressif"
set "BUILD_DIR=build-ai-local-s3"
set "PORT=COM7"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

call "%IDF_ROOT%\export.bat"
if errorlevel 1 exit /b 1

echo Monitoring %PORT%...
idf.py -p "%PORT%" -B "%BUILD_DIR%" monitor
exit /b %ERRORLEVEL%
