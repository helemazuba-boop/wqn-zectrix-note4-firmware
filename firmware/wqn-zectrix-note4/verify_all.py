import struct

# Check app header
build_app = 'build-ai-local-s3/wqn-zectrix-note4.bin'

print("=== Build output ===")
with open(build_app, 'rb') as f:
    data = f.read()
print(f"Build bin size: {len(data)} bytes")
print(f"Magic byte: 0x{data[0]:02X}")
entry = struct.unpack('<I', data[8:12])[0]
print(f"Entry point: 0x{entry:08X}")

# Read app header from flash
print("\n=== Reading app header from flash (via esptool) ===")
import subprocess
import os

os.chdir(r'D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4')
result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x20000', '256', 'app_flash.bin'
], capture_output=True, text=True)

if result.returncode == 0:
    with open('app_flash.bin', 'rb') as f:
        flash_data = f.read()
    print(f"Flash header size: {len(flash_data)} bytes")
    print(f"Magic: 0x{flash_data[0]:02X}")
    flash_entry = struct.unpack('<I', flash_data[8:12])[0]
    print(f"Entry: 0x{flash_entry:08X}")
    
    # Compare
    if data[:256] == flash_data:
        print("HEADERS MATCH - App header is correct!")
    else:
        print("MISMATCH in headers!")
        for i in range(256):
            if data[i] != flash_data[i]:
                print(f"  Byte {i}: build=0x{data[i]:02X} flash=0x{flash_data[i]:02X}")
                if i > 10:
                    break
else:
    print(f"esptool error: {result.stderr}")
