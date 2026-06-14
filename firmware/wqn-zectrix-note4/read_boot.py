import serial
import time
import sys
import threading
import signal

PORT = 'COM7'
BAUD = 115200
running = True

def signal_handler(sig, frame):
    global running
    running = False

signal.signal(signal.SIGINT, signal_handler)

try:
    # First, reset the chip via DTR/RTS if possible, or just open the port
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    print(f"Opened {PORT}. Waiting for boot output (60 seconds)...")
    print("-" * 60)
    sys.stdout.flush()
    
    # Discard any stale data
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    
    start = time.time()
    last_activity = time.time()
    buffer = ""
    
    while running and time.time() - start < 60:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            buffer += text
            last_activity = time.time()
            # Print immediately
            sys.stdout.write(text)
            sys.stdout.flush()
        else:
            # Check if we should report idle
            if buffer and time.time() - last_activity > 2:
                print(f"\n[Idle for {int(time.time()-last_activity)}s, still listening...]")
                sys.stdout.flush()
                last_activity = time.time()
        time.sleep(0.05)
    
    print("\n" + "-" * 60)
    print(f"Total received: {len(buffer)} chars")
    ser.close()
    
except serial.SerialException as e:
    print(f"Serial error: {e}")
    sys.exit(1)
except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)
