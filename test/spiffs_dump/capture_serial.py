#!/usr/bin/env python3
"""Capture SPIFFS dump markers from serial (binary-safe)."""
import sys
import time
from pathlib import Path

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem14401"
OUT_DIR = Path(__file__).resolve().parent / "extracted"
OUT_DIR.mkdir(parents=True, exist_ok=True)

import serial

ser = serial.Serial(PORT, 115200, timeout=0.2)
time.sleep(0.3)
ser.reset_input_buffer()

# Reset device to run dump sketch
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)

raw = bytearray()
deadline = time.time() + 300
print("Capturing serial...", flush=True)

while time.time() < deadline:
    chunk = ser.read(8192)
    if chunk:
        raw.extend(chunk)
        if b"===SPIFFS_DUMP_DONE===" in raw:
            break
    elif len(raw) > 0 and time.time() > deadline - 30:
        # allow trailing drain
        time.sleep(0.1)

ser.close()
text = raw.decode("utf-8", errors="replace")
(Path(__file__).parent / "extracted" / "_serial_raw.txt").write_text(text, encoding="utf-8")

files_written = {}
pos = 0
while True:
    start = text.find("===FILE:", pos)
    if start < 0:
        break
    nl = text.find("\n", start)
    if nl < 0:
        break
    path = text[start + 8 : nl].strip()
    body_start = nl + 1
    if text[body_start:body_start + 11] == "===MISSING=":
        print(f"missing: {path}")
        pos = body_start + 13
        continue
    if text[body_start:body_start + 12] == "===OPEN_FAIL=":
        print(f"open fail: {path}")
        pos = body_start + 14
        continue
    end = text.find("===END===", body_start)
    if end < 0:
        print(f"incomplete: {path} (no END marker)")
        break
    body = text[body_start:end]
    if body.startswith("\n"):
        body = body[1:]
    if body.endswith("\n"):
        body = body[:-1]
    safe = path.lstrip("/").replace("/", "_")
    out = OUT_DIR / safe
    out.write_text(body, encoding="utf-8")
    files_written[path] = len(body)
    print(f"wrote {out} ({len(body)} bytes)")
    pos = end + 9

print("\nSPIFFS listings:")
for line in text.splitlines():
    if line.startswith("LIST:"):
        print(f"  {line}")

print("\nSummary:")
for path, size in files_written.items():
    print(f"  {path}: {size} bytes")
if not files_written:
    print("  FAILED — see extracted/_serial_raw.txt")
    sys.exit(1)
