import subprocess

result = subprocess.run([
    'D:\\Program\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe',
    'D:\\Program\\Espressif\\frameworks\\esp-idf-v5.5.4\\components\\esptool_py\\esptool\\esptool.py',
    '-p', 'COM7', 'read_flash', '0x8000', '4096', 'pt_full.bin'
], capture_output=True, text=True, timeout=10)

if result.returncode == 0:
    with open('pt_full.bin', 'rb') as f:
        data = f.read()
    
    import struct
    print(f"Partition table at 0x8000 ({len(data)} bytes):")
    print(f"Magic: 0x{data[0]:02X} 0x{data[1]:02X} (expected 0xAA 0x50)")
    
    # Parse partition entries (each 32 bytes)
    for i in range(8):
        offset = i * 32
        entry = data[offset:offset+32]
        if entry[0] == 0xFF:
            print(f"  Entry {i}: unused")
            continue
        
        ptype = entry[0]
        psubtype = entry[1]
        addr = struct.unpack('<I', entry[4:8])[0]
        size = struct.unpack('<I', entry[8:12])[0]
        
        try:
            name = entry[16:16+16].rstrip(b'\x00\xFF').decode('utf-8', errors='replace')
        except:
            name = "(encoding error)"
        
        type_names = {0: 'app', 1: 'data', 2: 'ospi', 0x40: 'esp-radio'}
        subtype_names = {
            0: 'app', 1: 'ota_0', 2: 'ota_1', 3: 'ota_2',
            16: 'nvs', 17: 'otadata', 18: 'phy', 19: 'coredump'
        }
        tname = type_names.get(ptype, f'{ptype}')
        sname = subtype_names.get(psubtype, f'{psubtype}')
        print(f"  Entry {i}: {name:16s} type={tname}({ptype}) subtype={sname}({psubtype}) addr=0x{addr:08X} size=0x{size:08X}")
        
        if i > 5 and ptype == 0xFF:
            break
else:
    print(f"Error: {result.stderr}")
