@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"
set "BUILD_DIR=build-ai-local-s3"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo.
echo Attempting to RUN the flashed app via esptool...
echo If this shows serial output, the app works but boot-from-flash is broken.
echo If this also shows nothing, there's a deeper hardware issue.
echo.
"%PYTHON%" "%ESPTOOL%" -p COM7 run

exit /b %ERRORLEVEL%
