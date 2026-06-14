import serial
import time
import sys

PORT = 'COM7'
BAUD = 115200

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
    print(f"Connected to {PORT}. Draining buffer for 5 seconds...")
    sys.stdout.flush()
    
    # Drain any existing data
    for i in range(50):
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()
        time.sleep(0.1)
    
    print("\nNo buffered data found. Trying raw read mode...")
    
    # Try to read raw bytes
    ser.close()
    ser = serial.Serial(PORT, BAUD, timeout=1)
    
    for i in range(100):
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            # Print as hex to see if there's any data
            print(f"Raw bytes ({len(data)}): {data.hex(' ')}")
            # Try decoding
            text = data.decode('utf-8', errors='replace')
            print(f"As text: {repr(text)}")
            sys.stdout.flush()
        time.sleep(0.1)
    
    print("Done reading.")
    ser.close()
except serial.SerialException as e:
    print(f"Error: {e}")
    sys.exit(1)
