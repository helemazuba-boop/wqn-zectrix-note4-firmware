import sys
import serial
import time
import threading

PORT = 'COM7'
BAUD = 115200

stop_event = threading.Event()

def reader(ser):
    print("Reader started. Waiting for data...")
    sys.stdout.flush()
    while not stop_event.is_set():
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            try:
                text = data.decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()
            except Exception as e:
                print(f"Decode error: {e}")
        time.sleep(0.02)
    # Drain remaining data
    if ser.in_waiting > 0:
        data = ser.read(ser.in_waiting)
        try:
            text = data.decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()
        except:
            pass
    print("\nReader stopped.")

try:
    ser = serial.Serial(PORT, BAUD, timeout=3)
    print(f"Connected to {PORT} at {BAUD} baud. Reading for 30 seconds...")
    print("-" * 60)
    sys.stdout.flush()
    
    reader_thread = threading.Thread(target=reader, args=(ser,))
    reader_thread.start()
    
    # Read for 30 seconds
    reader_thread.join(timeout=30)
    stop_event.set()
    reader_thread.join(timeout=2)
    
    print("\n" + "-" * 60)
    print("Done.")
    ser.close()
except serial.SerialException as e:
    print(f"Error: {e}")
    sys.exit(1)
except KeyboardInterrupt:
    stop_event.set()
    print("\nInterrupted.")
    sys.exit(0)
