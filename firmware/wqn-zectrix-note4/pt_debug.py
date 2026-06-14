import struct

# Read partition table using correct offset
import subprocess
result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x8000', '4096', 'flash_8k.bin'
], capture_output=True, text=True, timeout=15)

with open('flash_8k.bin', 'rb') as f:
    data = f.read()

# Partition table header is 8 bytes (ESP-IDF standard)
# Offset 0-1: magic (0xAA 0x50)
# Offset 2-3: version
# Offset 4-5: count
# Offset 6-7: mini header CRC
# Then entries at offset 8 (each 32 bytes)

magic = struct.unpack('<H', data[0:2])[0]
version = struct.unpack('<H', data[2:4])[0]
count = struct.unpack('<H', data[4:6])[0]
mini_crc = struct.unpack('<H', data[6:8])[0]

print(f"Magic: 0x{magic:04X} (expected 0xAA50)")
print(f"Version: 0x{version:04X}")
print(f"Count: {count}")
print(f"Mini CRC: 0x{mini_crc:04X}")

print(f"\nFirst 64 bytes (hex):")
print(' '.join(f'{b:02X}' for b in data[:64]))

print(f"\n--- Entry 0 at offset 8 ---")
entry0 = data[8:40]
print(f"type={entry0[0]} subtype={entry0[1]} addr=0x{struct.unpack('<I',entry0[4:8])[0]:08X} size=0x{struct.unpack('<I',entry0[8:12])[0]:08X}")
print(f"label: {entry0[16:32]}")

# Read build partition table for comparison
import os
build_pt = 'build-ai-local-s3/partition_table/partition-table.bin'
if os.path.exists(build_pt):
    with open(build_pt, 'rb') as f:
        pt_data = f.read()
    print(f"\n--- BUILD partition table (first 64 bytes) ---")
    print(' '.join(f'{b:02X}' for b in pt_data[:64]))
