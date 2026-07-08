@echo off
cd /d D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4
call D:\Program\Espressif\frameworks\esp-idf-v5.5.4\export.bat
idf.py -B build-ai-local-s3 build > logs\build.log 2>&1
echo ERRORLEVEL=%ERRORLEVEL% >> logs\build.log
exit /b %ERRORLEVEL%
