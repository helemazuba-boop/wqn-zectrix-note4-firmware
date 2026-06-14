#!/usr/bin/env python3
"""
listen_usb.py — plain serial listener for ESP32-S3 native USB-Serial-JTAG.

Avoids the PermissionError(13) noise from `idf.py monitor` on Windows when the
USB CDC-ACM device does not support EscapeCommFunction/SetCommState.

Usage:
    python listen_usb.py --port COM7 --baud 115200 --outfile wqn.log
    python listen_usb.py -p COM7 -b 115200

Press Ctrl+C to stop. Auto-reopens the port when the device reboots.
"""

import argparse
import sys
import time

try:
    import serial  # pyserial, comes with esptool / idf.py
except ImportError:
    sys.stderr.write(
        "ERROR: pyserial not found. It should already be installed with ESP-IDF.\n"
        "Try:  python -m pip install pyserial\n"
    )
    sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plain serial listener (no EscapeCommFunction calls)."
    )
    parser.add_argument("-p", "--port", default="COM7",
                        help="Serial port (default: COM7)")
    parser.add_argument("-b", "--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("-o", "--outfile", default="",
                        help="Optional raw output file (binary append)")
    parser.add_argument("--max-open-retries", type=int, default=8,
                        help="Give up opening after this many failures")
    parser.add_argument("--read-chunk", type=int, default=4096,
                        help="Read chunk size in bytes")
    parser.add_argument("--reopen-delay", type=float, default=1.0,
                        help="Seconds to wait before reopening after a close")
    args = parser.parse_args()

    out_fp = None
    if args.outfile:
        try:
            out_fp = open(args.outfile, "ab", buffering=0)
        except OSError as e:
            sys.stderr.write(f"ERROR: cannot open outfile {args.outfile!r}: {e}\n")
            return 1

    open_retries = 0
    total_bytes = 0
    try:
        while True:
            ser = None
            try:
                ser = serial.Serial(
                    port=args.port,
                    baudrate=args.baud,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    timeout=0.2,
                    # Critical: do NOT touch DTR/RTS or any control lines.
                    # ESP32-S3 native USB-Serial-JTAG does not implement them
                    # over USB CDC-ACM, and pyserial's set_control() would
                    # raise PermissionError(13) on Windows.
                    dsrdtr=False,
                    rtscts=False,
                    xonxoff=False,
                )
            except (serial.SerialException, OSError) as e:
                open_retries += 1
                if open_retries > args.max_open_retries:
                    sys.stderr.write(
                        f"[listen_usb] Failed to open {args.port} after "
                        f"{args.max_open_retries} attempts: {e}\n"
                    )
                    if out_fp is not None:
                        out_fp.close()
                    return 1
                sys.stdout.write(
                    f"[listen_usb] open attempt {open_retries} failed: {e}; "
                    f"retrying in {args.reopen_delay:.0f}s...\n"
                )
                sys.stdout.flush()
                time.sleep(args.reopen_delay)
                continue

            open_retries = 0
            banner = (
                f"[listen_usb] Opened {args.port} @ {args.baud} 8-N-1 "
                f"(Ctrl+C to stop)\n"
            )
            sys.stdout.write(banner)
            sys.stdout.flush()
            if out_fp is not None:
                out_fp.write(banner.encode("utf-8"))

            try:
                while True:
                    try:
                        data = ser.read(args.read_chunk)
                    except (serial.SerialException, OSError) as e:
                        sys.stdout.write(
                            f"\n[listen_usb] read error: {e}\n"
                        )
                        sys.stdout.flush()
                        break
                    if not data:
                        continue
                    total_bytes += len(data)
                    text = data.decode("utf-8", errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    if out_fp is not None:
                        out_fp.write(data)
            finally:
                try:
                    ser.close()
                except Exception:
                    pass

            sys.stdout.write(
                f"\n[listen_usb] Port closed (device reboot?). "
                f"Reopening in {args.reopen_delay:.0f}s... "
                f"(total bytes read this session: {total_bytes})\n"
            )
            sys.stdout.flush()
            time.sleep(args.reopen_delay)
    except KeyboardInterrupt:
        sys.stdout.write("\n[listen_usb] Stopped (Ctrl+C).\n")
        sys.stdout.flush()
        if out_fp is not None:
            out_fp.close()
        return 0


if __name__ == "__main__":
    sys.exit(main())
