@echo off
setlocal EnableExtensions

set "IDF_ROOT=D:\Program\Espressif\frameworks\esp-idf-v5.5.4"
set "PYTHON=D:\Program\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "ESPTOOL=%IDF_ROOT%\components\esptool_py\esptool\esptool.py"
set "BUILD_DIR=build-ai-local-s3"

cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"

echo ============================================================
echo ESP32-S3 Flash Tool
echo ============================================================
echo.
echo INSTRUCTIONS:
echo 1. Press and HOLD the BOOT button on the board
echo 2. While holding BOOT, press and release RESET
echo 3. Release BOOT button
echo 4. Press ENTER here to start flashing
echo ============================================================
echo.
pause

echo.
echo Step 1: Erasing entire flash...
"%PYTHON%" "%ESPTOOL%" -p COM7 erase_flash
if errorlevel 1 (
    echo ERROR: Erase failed!
    pause
    exit /b 1
)

echo.
echo Step 2: Flashing bootloader...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0x1000 "%BUILD_DIR%\bootloader\bootloader.bin"
if errorlevel 1 (
    echo ERROR: Bootloader flash failed!
    pause
    exit /b 1
)

echo.
echo Step 3: Flashing partition table...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0x8000 "%BUILD_DIR%\partition_table\partition-table.bin"
if errorlevel 1 (
    echo ERROR: Partition flash failed!
    pause
    exit /b 1
)

echo.
echo Step 4: Flashing app...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0x20000 "%BUILD_DIR%\wqn-zectrix-note4.bin"
if errorlevel 1 (
    echo ERROR: App flash failed!
    pause
    exit /b 1
)

echo.
echo Step 5: Flashing ota_data...
"%PYTHON%" "%ESPTOOL%" -p COM7 --after no_reset write_flash 0xd000 "%BUILD_DIR%\ota_data_initial.bin"
if errorlevel 1 (
    echo ERROR: OTA data flash failed!
    pause
    exit /b 1
)

echo.
echo ============================================================
echo FLASHING COMPLETE!
echo.
echo Now press RESET on the board to start the app.
echo Then check the screen for output.
echo ============================================================
echo.
pause
