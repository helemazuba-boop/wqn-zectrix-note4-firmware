import serial
import time
import sys

PORT = 'COM7'

# Try different baud rates
BAUD_RATES = [115200, 921600, 460800, 230400, 57600]

try:
    for BAUD in BAUD_RATES:
        print(f"\nTrying {BAUD} baud...")
        sys.stdout.flush()
        try:
            ser = serial.Serial(PORT, BAUD, timeout=2)
            # Discard stale data
            ser.read(1000)
            
            # Try to reset the chip and capture output
            print(f"  Opening/resetting port at {BAUD}...")
            sys.stdout.flush()
            
            # Close and reopen to trigger any reset behavior
            ser.close()
            time.sleep(0.5)
            ser.open()
            
            # Wait for data
            data_received = False
            start = time.time()
            while time.time() - start < 3:
                if ser.in_waiting > 0:
                    data = ser.read(ser.in_waiting)
                    text = data.decode('utf-8', errors='replace')
                    if text.strip():
                        print(f"  Got data at {BAUD}: {repr(text[:200])}")
                        data_received = True
                    else:
                        print(f"  Got binary data at {BAUD}: {data.hex(' ')}")
                        data_received = True
                    sys.stdout.flush()
                time.sleep(0.1)
            
            if not data_received:
                print(f"  No data at {BAUD}")
            
            ser.close()
            time.sleep(0.3)
        except serial.SerialException as e:
            print(f"  Error at {BAUD}: {e}")
            try:
                ser.close()
            except:
                pass

    print("\nDone trying baud rates.")
    
except Exception as e:
    print(f"Fatal error: {e}")
    sys.exit(1)
