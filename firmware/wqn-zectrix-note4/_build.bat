@echo off
set MSYSTEM=
set MSYS=
call "D:\Program\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
cd /d "D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4"
idf.py -B build-ai-local-s3 build
