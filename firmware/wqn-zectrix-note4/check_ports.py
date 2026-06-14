import serial.tools.list_ports
for p in serial.tools.list_ports.comports():
    print(f"{p.device} | {p.hwid} | {p.description}")
