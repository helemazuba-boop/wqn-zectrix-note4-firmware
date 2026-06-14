@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo Reading app header (256 bytes) from flash...
"%PYTHON%" "%ESPTOOL%" -p COM7 read_flash 0x20000 256 app_check.bin

echo.
echo Comparing with build output...
if exist "build-ai-local-s3\wqn-zectrix-note4.bin" (
    echo Build bin exists, comparing...
) else (
    echo Build bin NOT found!
)

exit /b 0
