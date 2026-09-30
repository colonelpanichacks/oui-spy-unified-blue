#!/usr/bin/env python3
"""Join megamaid AP, run Playwright UI tests, restore WiFi."""
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PW_DIR = ROOT / "test" / "playwright"
OUT = ROOT / "test" / "megamaid_playwright_results.txt"
DEVICE_AP = os.environ.get("MEGAMAID_AP_SSID", "megamaid")
DEVICE_AP_PASS = os.environ.get("MEGAMAID_AP_PASS", "megamaid123")
RESTORE_WIFI = os.environ.get("MEGAMAID_WIFI_RESTORE", "The Shack")
RESTORE_PASS = os.environ.get("MEGAMAID_WIFI_RESTORE_PASS", "")
MEGAMAID_BASE = os.environ.get("MEGAMAID_BASE", "http://192.168.4.1")
SKIP_WIFI = os.environ.get("MEGAMAID_SKIP_WIFI", "") == "1"


def wifi_iface() -> str | None:
    out = subprocess.run(
        ["networksetup", "-listallhardwareports"],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = out.stdout.splitlines()
    for i, line in enumerate(lines):
        if "Wi-Fi" in line or "AirPort" in line:
            if i + 1 < len(lines) and "Device:" in lines[i + 1]:
                return lines[i + 1].split(":", 1)[1].strip()
    return None


def wifi_join(ssid: str, password: str = "") -> str:
    iface = wifi_iface()
    if not iface:
        return "no_wifi_iface"
    args = ["networksetup", "-setairportnetwork", iface, ssid]
    if password:
        args.append(password)
    subprocess.run(args, capture_output=True, check=False)
    return f"joined {ssid} on {iface}"


def main() -> int:
    lines = []
    lines.append("=== MEGA MAID PLAYWRIGHT UI TEST ===")
    iface = wifi_iface()
    lines.append(f"iface: {iface}")
    lines.append(f"base: {MEGAMAID_BASE}")

    if not SKIP_WIFI:
        try:
            lines.append(f"device_ap: {wifi_join(DEVICE_AP, DEVICE_AP_PASS)}")
            time.sleep(6)
        except Exception as e:
            lines.append(f"FAIL device_ap: {e}")
            Path(OUT).write_text("\n".join(lines) + "\n")
            return 1

    env = os.environ.copy()
    env["MEGAMAID_BASE"] = MEGAMAID_BASE

    if not (PW_DIR / "node_modules").exists():
        lines.append("npm install...")
        npm = subprocess.run(
            ["npm", "install"],
            cwd=PW_DIR,
            capture_output=True,
            text=True,
        )
        if npm.returncode != 0:
            lines.append("FAIL npm install")
            lines.append(npm.stderr or npm.stdout or "")
            Path(OUT).write_text("\n".join(lines) + "\n")
            return 1

    pw = subprocess.run(
        ["npx", "playwright", "test"],
        cwd=PW_DIR,
        env=env,
        capture_output=True,
        text=True,
    )
    lines.append(pw.stdout or "")
    if pw.stderr:
        lines.append(pw.stderr)

    if not SKIP_WIFI and iface:
        try:
            lines.append(f"wifi_restore: {wifi_join(RESTORE_WIFI, RESTORE_PASS)}")
        except Exception as e:
            lines.append(f"wifi_restore_fail: {e}")

    ok = pw.returncode == 0
    lines.append("OVERALL PASS" if ok else f"OVERALL FAIL exit={pw.returncode}")
    Path(OUT).write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
