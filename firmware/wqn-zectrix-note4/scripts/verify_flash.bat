@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo Reading end of app partition (last 256 bytes at 0x196F00)...
"%PYTHON%" "%ESPTOOL%" -p COM7 read_flash 0x196F00 256 app_end.bin

echo Reading app beginning (0x20000, 32 bytes)...
"%PYTHON%" "%ESPTOOL%" -p COM7 read_flash 0x20000 32 app_start.bin

echo.
echo Done.
