import struct
import subprocess

result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x8000', '4096', 'flash_8k.bin'
], capture_output=True, text=True, timeout=15)

if result.returncode != 0:
    print(f"Error: {result.stderr}")
    exit(1)

with open('flash_8k.bin', 'rb') as f:
    data = f.read()

print(f"Flash 0x8000-{0x8000+len(data):X} ({len(data)} bytes):")

magic = b'\xAA\x50'
pos = data.find(magic)
if pos >= 0:
    print(f"\nPartition table magic found at offset 0x{pos:04X} (absolute 0x{0x8000+pos:08X})")
else:
    print("\nWARNING: Standard partition table magic (0xAA 0x50) NOT found!")
    for i in range(min(64, len(data) - 1)):
        if data[i] == 0xAA and data[i+1] == 0x50:
            print(f"  0xAA 0x50 at offset 0x{i:04X}")
            break

print("\nParsing partition table at 0x8000:")
for i in range(8):
    offset = i * 32
    if offset + 32 > len(data):
        break
    entry = data[offset:offset+32]

    if entry[0] == 0xFF and entry[1] == 0xFF:
        print(f"  Entry {i}: empty")
        continue

    ptype = entry[0]
    psubtype = entry[1]
    addr = struct.unpack('<I', entry[4:8])[0]
    size = struct.unpack('<I', entry[8:12])[0]
    flags = struct.unpack('<I', entry[12:16])[0]

    try:
        name_bytes = entry[16:32].rstrip(b'\x00\xFF')
        name = name_bytes.decode('ascii', errors='replace')
    except:
        name = "(non-ascii)"

    print(f"  Entry {i}: name='{name}' type={ptype} subtype={psubtype} addr=0x{addr:08X} size=0x{size:08X} flags=0x{flags:08X}")

    if i > 4 and entry[0] == 0xFF:
        break
