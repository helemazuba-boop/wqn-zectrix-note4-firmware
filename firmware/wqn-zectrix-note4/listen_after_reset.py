import serial
import time
import sys
import threading

PORT = 'COM7'
BAUD = 115200

print("Starting serial listener first...")
sys.stdout.flush()

# Start reading in background thread
buffer = []
buffer_lock = threading.Lock()

def read_loop(ser):
    while True:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            with buffer_lock:
                buffer.append(text)
        time.sleep(0.05)

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    
    # Drain buffer
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    ser.close()
    print("Port closed. Waiting 1 second...")
    sys.stdout.flush()
    time.sleep(1)
    
    # Open fresh
    print("Reopening port...")
    sys.stdout.flush()
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    
    # Start reading thread
    reader = threading.Thread(target=read_loop, args=(ser,), daemon=True)
    reader.start()
    
    print("Waiting 20 seconds for any output...")
    sys.stdout.flush()
    time.sleep(20)
    
    with buffer_lock:
        output = ''.join(buffer)
    
    if output:
        print("OUTPUT RECEIVED:")
        print(output)
    else:
        print("NO OUTPUT received in 20 seconds.")
    
    ser.close()
    
except serial.SerialException as e:
    print(f"Error: {e}")
    sys.exit(1)
