import struct

# Check bootloader header at 0x1000
with open('boot_read.bin', 'rb') as f:
    boot_data = f.read()

print(f"=== Bootloader (0x1000) ===")
print(f"Size: {len(boot_data)} bytes")

# ESP32 bootloader header structure
# Magic: 0xEL at offset 0
# Load phases: byte 1
# Load address: 4 bytes at offset 4
# Size: 4 bytes at offset 8
magic = boot_data[0]
load_phases = boot_data[1]
load_addr = struct.unpack('<I', boot_data[4:8])[0]
size = struct.unpack('<I', boot_data[8:12])[0]

print(f"Magic: 0x{magic:02X} (expected 0xEL for ESP32)")
print(f"Load phases: {load_phases}")
print(f"Load addr: 0x{load_addr:08X}")
print(f"Size: 0x{size:08X}")

# Check for ELF
if boot_data[:4] == b'\x7fELF':
    print("WARNING: Bootloader is ELF format, not binary!")

# Check partition table
import subprocess
result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x8000', '256', 'part_table.bin'
], capture_output=True, text=True, timeout=10)

if result.returncode == 0:
    with open('part_table.bin', 'rb') as f:
        pt = f.read()
    print(f"\n=== Partition Table (0x8000) ===")
    print(f"First 32 bytes: {pt[:32].hex()}")
    # Parse partition entries
    # Each entry is 32 bytes
    for i in range(4):  # Check first 4 entries
        offset = i * 32
        entry = pt[offset:offset+32]
        if entry[0] == 0xFF:  # Unused entry
            continue
        name = entry[0:16].rstrip(b'\xFF\x00').decode('utf-8', errors='replace')
        type_byte = entry[16]
        subtype = entry[17]
        addr = struct.unpack('<I', entry[12:16])[0]
        size_val = struct.unpack('<I', entry[20:24])[0]
        print(f"  Entry {i}: name='{name}' type={type_byte} subtype={subtype} addr=0x{addr:08X} size=0x{size_val:08X}")
