import struct

build_app = 'build-ai-local-s3/wqn-zectrix-note4.bin'

print("=== Build output ===")
with open(build_app, 'rb') as f:
    data = f.read()
print(f"Build bin size: {len(data)} bytes")
print(f"Magic: 0x{data[0]:02X}")
print(f"Segments: {data[2]}")
print(f"Flash size: 0x{data[3]:02X}")

# ESP32 app header: first segment info at offset 0x04
seg1_addr = struct.unpack('<I', data[4:8])[0]
seg1_len = struct.unpack('<I', data[8:12])[0]
print(f"Segment 1: addr=0x{seg1_addr:08X}, len=0x{seg1_len:08X}")

# Entry point (for SB2, this is signature pointer)
entry = struct.unpack('<I', data[12:16])[0]
print(f"Entry (SB2 sig ptr): 0x{entry:08X}")

# Hash
sha_start = data[16:48]
print(f"SHA256: {sha_start.hex()[:32]}...")

# Check for ELF header (if app is ELF)
if data[:4] == b'\x7fELF':
    print("WARNING: This is an ELF file, not a raw binary!")
else:
    print("Binary format confirmed (not ELF)")

# Now try reading from flash
import subprocess
result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x20000', '256', 'flash_app.bin'
], capture_output=True, text=True, timeout=10)

if result.returncode == 0:
    with open('flash_app.bin', 'rb') as f:
        flash = f.read()
    print(f"\n=== Flash content at 0x20000 ===")
    print(f"Magic: 0x{flash[0]:02X}")
    if data[:256] == flash[:256]:
        print("MATCH - App header correctly stored in flash!")
    else:
        print("MISMATCH!")
        for i in range(256):
            if data[i] != flash[i]:
                print(f"  Byte {i}: build=0x{data[i]:02X} flash=0x{flash[i]:02X}")
                if i > 5:
                    break
else:
    print(f"esptool error: {result.stderr}")
