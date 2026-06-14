@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"
set "BUILD_DIR=build-ai-local-s3"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo Flashing firmware WITHOUT auto-reset...
echo You must manually press the RESET button on the board AFTER flashing completes.
echo.

echo Step 1: Flashing bootloader...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0x1000 "%BUILD_DIR%\bootloader\bootloader.bin"
if errorlevel 1 (
    echo ERROR: Bootloader flash failed!
    exit /b 1
)

echo Step 2: Flashing partition table...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0x8000 "%BUILD_DIR%\partition_table\partition-table.bin"
if errorlevel 1 (
    echo ERROR: Partition flash failed!
    exit /b 1
)

echo Step 3: Flashing app...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0x20000 "%BUILD_DIR%\wqn-zectrix-note4.bin"
if errorlevel 1 (
    echo ERROR: App flash failed!
    exit /b 1
)

echo Step 4: Flashing ota_data...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0xd000 "%BUILD_DIR%\ota_data_initial.bin"
if errorlevel 1 (
    echo ERROR: OTA data flash failed!
    exit /b 1
)

echo.
echo ============================================================
echo ALL FLASHING COMPLETE!
echo.
echo NOW: Press the RESET button on the board to start the app.
echo Then monitor COM7 to see the boot output.
echo ============================================================
echo.

exit /b 0
