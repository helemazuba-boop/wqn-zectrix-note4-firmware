import serial
import time
import sys

DEFAULT_PORT = "COM7"
baud = 115200
log_file = "serial_log.txt"

if len(sys.argv) > 1:
    port = sys.argv[1]
else:
    port = DEFAULT_PORT

print(f"Listening on {port} at {baud} baud...")
print("Press Ctrl+C to stop. Log will be saved to", log_file)
print("=" * 40)

with open(log_file, "w", encoding="utf-8") as f:
    while True:
        try:
            ser = serial.Serial(port, baud, timeout=0.1)
            print("\n[Connected] Reading log...\n")
            while True:
                data = ser.read(1024)
                if data:
                    text = data.decode("utf-8", errors="replace")
                    print(text, end="")
                    f.write(text)
                    f.flush()
        except (serial.SerialException, OSError):
            sys.stdout.write(".")
            sys.stdout.flush()
            time.sleep(0.1)
