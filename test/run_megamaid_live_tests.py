#!/usr/bin/env python3
"""Run Mega Maid dashboard live tests. Joins megamaid AP, tests APIs, restores primary WiFi."""
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test" / "megamaid_live_test_results.txt"
REF = ROOT / "test" / "megamaid_test_detections.json"
BASE = "http://192.168.4.1"
TIMEOUT = 90
DEVICE_AP = os.environ.get("MEGAMAID_AP_SSID", "megamaid")
DEVICE_AP_PASS = os.environ.get("MEGAMAID_AP_PASS", "megamaid123")
RESTORE_WIFI = os.environ.get("MEGAMAID_WIFI_RESTORE", "The Shack")
RESTORE_PASS = os.environ.get("MEGAMAID_WIFI_RESTORE_PASS", "")


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


def fetch(path: str, timeout: int = TIMEOUT) -> bytes:
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read()


def main() -> int:
    lines = []
    lines.append("=== MEGA MAID LIVE TEST ===")
    iface = wifi_iface()
    lines.append(f"iface: {iface}")

    try:
        lines.append(f"device_ap: {wifi_join(DEVICE_AP, DEVICE_AP_PASS)}")
        time.sleep(5)
    except Exception as e:
        lines.append(f"FAIL device_ap: {e}")
        Path(OUT).write_text("\n".join(lines) + "\n")
        return 1

    fails = 0

    def restore_wifi():
        if not iface:
            return
        try:
            msg = wifi_join(RESTORE_WIFI, RESTORE_PASS)
            lines.append(f"wifi_restore: {msg}")
            time.sleep(3)
        except Exception as e:
            lines.append(f"wifi_restore_fail: {e}")

    try:
        stats = json.loads(fetch("/api/stats", timeout=20))
        lines.append(
            f"STATS OK profile={stats.get('radio_profile')} total={stats.get('total')}"
        )
        if stats.get("radio_profile") != "dashboard":
            lines.append("FAIL stats: expected radio_profile=dashboard")
            fails += 1
    except Exception as e:
        lines.append(f"FAIL stats: {e}")
        fails += 1
        restore_wifi()
        Path(OUT).write_text("\n".join(lines) + "\n")
        return 1

    try:
        html = fetch("/", timeout=30).decode("utf-8", errors="replace")
        for token in [
            "modeBadge",
            "exportSummary",
            "COPY JSON",
            "copyModal",
            "copyExec",
            "ClipboardItem",
            "radio_profile",
        ]:
            ok = token in html
            lines.append(("OK" if ok else "FAIL") + f" html:{token}")
            if not ok:
                fails += 1
    except Exception as e:
        lines.append(f"FAIL html: {e}")
        fails += 1

    try:
        export = json.loads(fetch("/api/export/json", timeout=TIMEOUT))
        lines.append(f"EXPORT OK len={len(export)}")
    except Exception as e:
        lines.append(f"FAIL export: {e}")
        fails += 1

    hist = None
    try:
        hist = json.loads(fetch("/api/history", timeout=TIMEOUT))
        n = len(hist) if isinstance(hist, list) else 0
        lines.append(f"HISTORY OK len={n}")
        prev_json = fetch("/api/history/json", timeout=TIMEOUT)
        json.loads(prev_json)
        lines.append(f"HISTORY_JSON OK bytes={len(prev_json)}")
    except Exception as e:
        lines.append(f"FAIL history: {e}")
        fails += 1

    try:
        with REF.open(encoding="utf-8") as f:
            ref = json.load(f)
        lines.append(f"REF methods={sorted(ref.get('expected_methods', []))}")
    except Exception as e:
        lines.append(f"FAIL ref: {e}")
        fails += 1

    restore_wifi()

    lines.append("OVERALL PASS" if fails == 0 else f"OVERALL FAIL count={fails}")
    Path(OUT).write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
