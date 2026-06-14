import struct

build_app = 'build-ai-local-s3/wqn-zectrix-note4.bin'

print("=== Build output ===")
with open(build_app, 'rb') as f:
    data = f.read()
    print(f"Size: {len(data)} bytes")
    magic = data[0]
    print(f"Magic: 0x{magic:02X}")
    entry = struct.unpack('<I', data[8:12])[0]
    print(f"Entry point: 0x{entry:08X}")
    
    # Check if magic is valid for ESP32
    if magic == 0xE9:
        print("Magic byte 0xE9 indicates valid ESP32 app")
        seg_count = data[2]
        flash_size = data[3]
        print(f"Segments: {seg_count}, Flash size byte: 0x{flash_size:02X}")
    else:
        print(f"WARNING: Magic byte 0x{magic:02X} is unexpected!")
        print(f"First 32 bytes: {data[:32].hex()}")

# Read the app header from flash (first 256 bytes)
print()
print("=== Flash content at 0x20000 ===")
with open('app_read.bin', 'rb') as f:
    flash_data = f.read()
    print(f"Header size: {len(flash_data)} bytes")
    flash_magic = flash_data[0]
    print(f"Magic: 0x{flash_magic:02X}")
    flash_entry = struct.unpack('<I', flash_data[8:12])[0]
    print(f"Entry point: 0x{flash_entry:08X}")
    
    # Compare headers
    build_header = data[:256]
    print()
    print("=== Header comparison ===")
    mismatches = 0
    for i in range(min(256, len(build_header), len(flash_data))):
        if build_header[i] != flash_data[i]:
            mismatches += 1
            if mismatches <= 5:
                print(f"  Byte {i}: build=0x{build_header[i]:02X} flash=0x{flash_data[i]:02X}")
    print(f"Total mismatches: {mismatches}/256")
