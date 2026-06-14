import serial
import sys
import time

port = "COM7"
baud = 115200
log_file = "serial_log.txt"

print(f"Connecting to {port} at {baud} baud...")
print("Press Ctrl+C to stop and view serial_log.txt")
print()

try:
    ser = serial.Serial(port, baud, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    with open(log_file, "w", encoding="utf-8", errors="replace") as f:
        while True:
            try:
                if ser.in_waiting:
                    line = ser.readline()
                    try:
                        text = line.decode("utf-8", errors="replace")
                        print(text, end="")
                        f.write(text)
                        f.flush()
                    except:
                        pass
                time.sleep(0.01)
            except KeyboardInterrupt:
                break
except serial.SerialException as e:
    print(f"Error: {e}")
    sys.exit(1)
finally:
    try:
        ser.close()
    except:
        pass

print()
print(f"Log saved to {log_file}")
