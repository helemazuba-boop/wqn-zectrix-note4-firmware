@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo Checking chip on COM7...
"%PYTHON%" "%IDF_ROOT%\components\esptool_py\esptool\esptool.py" -p COM7 chip_id

exit /b %ERRORLEVEL%
