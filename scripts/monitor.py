#!/usr/bin/env python3
"""
OUI Spy Unified Blue -- Serial Monitor

Lightweight serial monitor that works without a TTY (unlike pio device monitor).
Useful for headless environments, CI, and Claude Code sessions.

    python scripts/monitor.py              # auto-detect port, stream forever
    python scripts/monitor.py --timeout 10 # capture 10 seconds of output
    python scripts/monitor.py --port /dev/cu.usbmodem2101

Requirements:  uv sync  (or: pip install pyserial)
"""

import argparse
import sys
import time

import serial
import serial.tools.list_ports

# Known USB VID:PID pairs for ESP32-S3 / common UART bridges
ESP_VIDS = {0x303A, 0x1A86, 0x10C4, 0x0403}

DEFAULT_BAUD = 115200


def find_port():
    """Auto-detect the first ESP32 serial port."""
    for p in serial.tools.list_ports.comports():
        if p.vid and p.vid in ESP_VIDS:
            return p.device
        if "usbmodem" in (p.device or "").lower():
            return p.device
        if "usbserial" in (p.device or "").lower():
            return p.device
        if "ttyACM" in (p.device or "") or "ttyUSB" in (p.device or ""):
            return p.device
    return None


def monitor(port, baud, timeout=None):
    """Stream serial output to stdout."""
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening {port}: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[monitor] {port} @ {baud} baud", file=sys.stderr)
    if timeout:
        print(f"[monitor] capturing for {timeout}s", file=sys.stderr)

    deadline = time.time() + timeout if timeout else None

    try:
        while True:
            if deadline and time.time() >= deadline:
                break
            line = ser.readline()
            if line:
                try:
                    print(line.decode("utf-8", errors="replace").rstrip(), flush=True)
                except UnicodeDecodeError:
                    print(repr(line), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("\n[monitor] closed", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Serial monitor for ESP32 debugging")
    parser.add_argument("--port", "-p", help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    parser.add_argument("--timeout", "-t", type=float, default=None, help="Stop after N seconds (default: run forever)")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No ESP32 detected. Is the board plugged in?", file=sys.stderr)
        sys.exit(1)

    monitor(port, args.baud, args.timeout)


if __name__ == "__main__":
    main()
