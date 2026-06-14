import serial
import time
import sys
import threading

PORT = 'COM7'
BAUD = 115200
running = True

def signal_handler(sig, frame):
    global running
    running = False

try:
    import signal
    signal.signal(signal.SIGINT, signal_handler)
except:
    pass

try:
    # First open and drain
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
    print(f"Opened {PORT}, draining buffer...")
    sys.stdout.flush()
    time.sleep(1)
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    ser.close()
    time.sleep(0.5)
    
    # Now open fresh and wait
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    print("Port ready. Waiting for boot output...")
    sys.stdout.flush()
    
    start = time.time()
    last_time = time.time()
    
    while running and time.time() - start < 120:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()
            last_time = time.time()
        else:
            if time.time() - last_time > 5 and time.time() - start > 5:
                print(f"\n[Still listening... {(time.time()-start):.0f}s elapsed]")
                sys.stdout.flush()
                last_time = time.time()
        time.sleep(0.05)
    
    print(f"\nDone. Total time: {time.time()-start:.1f}s")
    ser.close()
except serial.SerialException as e:
    print(f"Serial error: {e}")
    sys.exit(1)
