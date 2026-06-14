import struct
import subprocess
import sys

# Read flash
result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x8000', '4096', 'flash_8k.bin'
], capture_output=True, text=True, timeout=15)

if result.returncode != 0:
    print("Error reading flash")
    sys.exit(1)

with open('flash_8k.bin', 'rb') as f:
    data = f.read()

# The partition table has a header and then entries
# Header: 32 bytes
# Each entry: 32 bytes
# Magic: 0xAA 0x50 at offset 0

print("=== Partition Table Analysis ===")
print(f"Total size: {len(data)} bytes")
print(f"Magic: 0x{data[0]:02X} 0x{data[1]:02X}")

# Parse entries
print("\n=== Entries ===")
for i in range(10):
    offset = i * 32
    if offset + 32 > len(data):
        break
    entry = data[offset:offset+32]
    
    # Check if entry is empty
    if all(b == 0xFF for b in entry):
        print(f"  Entry {i}: EMPTY (all 0xFF)")
        continue
    
    # The partition table entry format (per IDF source):
    # type: uint8_t at offset 0
    # subtype: uint8_t at offset 1
    # reserved: uint8_t[2] at offset 2-3
    # addr: uint32_t at offset 4 (little-endian)
    # size: uint32_t at offset 8 (little-endian)
    # flags: uint32_t at offset 12
    # label: uint8_t[16] at offset 16 (null-terminated string)
    
    ptype = entry[0]
    psubtype = entry[1]
    addr = struct.unpack('<I', entry[4:8])[0]
    size = struct.unpack('<I', entry[8:12])[0]
    flags = struct.unpack('<I', entry[12:16])[0]
    
    label = entry[16:32]
    # Find null terminator or end
    label_len = 0
    for j in range(16):
        if label[j] == 0 or label[j] == 0xFF:
            label_len = j
            break
    name = label[:label_len].decode('utf-8', errors='replace') if label_len > 0 else ''
    
    # Type names (from esp_partition_type_t)
    type_names = {
        0: 'app', 1: 'data', 2: 'coredump', 0x40: 'esp-radio'
    }
    # Subtype names (from esp_partition_subtype_t)
    subtype_names_app = {
        0: 'app', 0x10: 'ota_0', 0x11: 'ota_1', 0x12: 'ota_2',
        0x20: 'test', 0x0F: 'factory'
    }
    subtype_names_data = {
        0: 'data', 0x00: 'nvs', 0x01: 'otadata', 0x02: 'phy',
        0x03: 'coredump', 0x04: 'efuse', 0x05: 'undefined'
    }
    
    tname = type_names.get(ptype, str(ptype))
    if ptype == 0:
        sname = subtype_names_app.get(psubtype, str(psubtype))
    elif ptype == 1:
        sname = subtype_names_data.get(psubtype, str(psubtype))
    else:
        sname = str(psubtype)
    
    print(f"  Entry {i}: {name:12s} type={tname}({ptype:3d}) subtype={sname}({psubtype:3d}) addr=0x{addr:08X} size=0x{size:08X}")

print("\n=== Key Findings ===")
print(f"App partition should be at 0x00020000, size 0x003F0000")
print(f"App starts at 0x00020000: magic 0x{data[0x20000-0x8000]:02X} (should be 0xE9)")
