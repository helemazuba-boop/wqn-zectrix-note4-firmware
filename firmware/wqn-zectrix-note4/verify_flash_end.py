import struct

build_app = 'build-ai-local-s3/wqn-zectrix-note4.bin'
app_start = 'app_start.bin'
app_end = 'app_end.bin'

print("=== Comparing app beginning (first 32 bytes) ===")
with open(build_app, 'rb') as f:
    build_data = f.read()

with open(app_start, 'rb') as f:
    start_data = f.read()

print(f"Build first 32: {build_data[:32].hex()}")
print(f"Flash first 32: {start_data.hex()}")

if build_data[:32] == start_data:
    print("MATCH - App beginning is correct!")
else:
    print("MISMATCH - App beginning differs!")

print()
print("=== Comparing app end (last 256 bytes) ===")
# Last 256 bytes of build bin
build_end = build_data[-256:]
with open(app_end, 'rb') as f:
    end_data = f.read()

print(f"Build last 256: {build_end[:32].hex()}...{build_end[-32:].hex()}")
print(f"Flash last 256: {end_data[:32].hex()}...{end_data[-32:].hex()}")

if build_end == end_data:
    print("MATCH - App end is correct!")
else:
    print("MISMATCH - App end differs!")
    # Count matching bytes
    match_count = 0
    for i in range(256):
        if i < len(build_end) and i < len(end_data) and build_end[i] == end_data[i]:
            match_count += 1
    print(f"  {match_count}/256 bytes match")
    # Find first mismatch
    for i in range(256):
        if i < len(build_end) and i < len(end_data) and build_end[i] != end_data[i]:
            print(f"  First mismatch at byte {i}: build=0x{build_end[i]:02X} flash=0x{end_data[i]:02X}")
            break

print()
print(f"Build bin total size: {len(build_data)} bytes")
