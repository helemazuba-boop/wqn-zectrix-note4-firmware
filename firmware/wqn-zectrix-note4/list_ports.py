import serial.tools.list_ports

ports = serial.tools.list_ports.comports()
print("Available COM ports:")
for port in sorted(ports):
    print(f"  {port.device}: {port.description}")
    if port.hwid:
        print(f"    HWID: {port.hwid}")
