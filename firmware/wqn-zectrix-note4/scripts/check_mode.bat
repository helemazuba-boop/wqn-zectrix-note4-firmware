@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo.
echo Checking current chip status...
echo If chip shows "Features: WiFi, BLE" instead of "USB mode", it's in download mode.
"%PYTHON%" "%ESPTOOL%" -p COM7 chip_id

echo.
echo If the above shows USB mode, the ROM bootloader is waiting for download commands.
echo This is why no app output is seen.
echo.
echo Try pressing the RESET button on the board while monitoring COM7 with the Python reader.
echo Or, try this command to attempt a cold reset:
echo.
echo Manual cold reset command (run after pressing RESET on board):
echo   "%PYTHON%" "%ESPTOOL%" -p COM7 --before no_reset chip_id

exit /b 0
