import serial
import time
import sys

PORT = 'COM7'
BAUD = 115200

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    print(f"Opened {PORT}, listening for 30 seconds...")
    sys.stdout.flush()
    
    start = time.time()
    last_data = time.time()
    
    while time.time() - start < 30:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()
            last_data = time.time()
        elif time.time() - last_data > 3 and time.time() - start > 5:
            print(f"\n[Silence for {int(time.time()-last_data)}s]")
            sys.stdout.flush()
            last_data = time.time()
        time.sleep(0.05)
    
    print(f"\nDone. Total: {time.time()-start:.1f}s")
    ser.close()
except serial.SerialException as e:
    print(f"Error: {e}")
    sys.exit(1)
