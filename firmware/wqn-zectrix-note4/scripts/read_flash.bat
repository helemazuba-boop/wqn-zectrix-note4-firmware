@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"

cd /d "%IDF_ROOT%"
call "%IDF_ROOT%\export.bat"
if errorlevel 1 exit /b 1

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo Reading bootloader (0x1000, 8192 bytes)...
"%PYTHON%" "%ESPTOOL%" -p COM7 read_flash 0x1000 8192 boot_read.bin

echo Reading app header (0x20000, 256 bytes)...
"%PYTHON%" "%ESPTOOL%" -p COM7 read_flash 0x20000 256 app_read.bin

echo Done. Files saved.
dir *.bin

exit /b %ERRORLEVEL%
