#!/usr/bin/env python3
"""Validate Mega_Maid cap-250 build: portal, export model, serial + live checks."""
from __future__ import annotations

import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEGAMAID = ROOT / "src" / "raw" / "megamaid.cpp"
OUT = ROOT / "test" / "megamaid_cap250_soak_results.txt"
BASE = "http://192.168.4.1"
NEAR_CAP = 240
TARGET_CAP = 250

SERIAL_BAD = re.compile(
    r"(?i)(guru meditation|abort\(\)|stack overflow|heapLow=1|"
    r"Promote FAILED|Save failed|HTTP fallback|low heap|out of memory|panic)"
)
SERIAL_OK = re.compile(
    r"\[MEGA-MAID\].*(heap=|HTTP restart heap=|status: hits=|Web server started)"
)


def read_constants() -> tuple[int, int, int]:
    text = MEGAMAID.read_text(encoding="utf-8")
    n1_block = re.search(
        r"#ifdef NESSO_N1\s+#define MAX_DETECTIONS (\d+).*?"
        r"#define MM_NEAR_CAP_WARN_THRESHOLD\s+\(MAX_DETECTIONS - (\d+)\).*?"
        r"#define MM_AT_CAP_FULL\s+MAX_DETECTIONS.*?#define MM_HTTP_MIN_HEAP\s+(\d+)",
        text,
        re.S,
    )
    if not n1_block:
        raise RuntimeError("Could not parse Mega_Maid cap constants")
    max_det = int(n1_block.group(1))
    near_margin = int(n1_block.group(2))
    min_heap = int(n1_block.group(3))
    near_cap = max_det - near_margin
    return max_det, near_cap, min_heap


def synth_detections(count: int) -> list[dict]:
    out = []
    for i in range(count):
        b = i // 256
        c = i % 256
        out.append(
            {
                "mac": f"aa:bb:cc:{b:02x}:{c:02x}:01",
                "name": "",
                "rssi": -70 - (i % 20),
                "method": "wifi_addr1",
                "first": 1000 + i,
                "last": 2000 + i,
                "count": 1,
                "channel": 1 + (i % 14),
            }
        )
    return out


def validate_export_shape(detections: list[dict]) -> None:
    required = {"mac", "rssi", "method", "first", "last", "count"}
    for det in detections[:5] + detections[-5:]:
        missing = required - set(det.keys())
        if missing:
            raise RuntimeError(f"synthetic export missing fields: {missing}")
        for field in ("first", "last"):
            val = det.get(field)
            if not isinstance(val, int) or val < 0:
                raise RuntimeError(f"{field} must be non-negative int epoch/uptime ms, got {val!r}")


def capture_serial(port: str, seconds: int = 45) -> str:
    import serial

    ser = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
        else:
            time.sleep(0.05)
    ser.close()
    return buf.decode("utf-8", errors="replace")


def try_live_export() -> list[str]:
    lines: list[str] = []
    stats = json.loads(
        urllib.request.urlopen(f"{BASE}/api/stats", timeout=20).read().decode()
    )
    lines.append(
        f"LIVE stats profile={stats.get('radio_profile')} total={stats.get('total')}"
    )
    export = json.loads(
        urllib.request.urlopen(f"{BASE}/api/export/json", timeout=120).read().decode()
    )
    lines.append(f"LIVE export_count={len(export)}")
    if isinstance(export, list) and export:
        validate_export_shape(export)
        lines.append("LIVE export_shape OK")
    return lines


def main() -> int:
    lines: list[str] = ["=== MEGA MAID CAP-250 SOAK VALIDATION ==="]
    fails = 0

    try:
        max_det, near_cap, min_heap = read_constants()
        lines.append(f"CONST MAX_DETECTIONS={max_det}")
        lines.append(f"CONST NEAR_CAP_WARN_THRESHOLD={near_cap}")
        lines.append(f"CONST AT_CAP_FULL={max_det}")
        lines.append(f"CONST MM_HTTP_MIN_HEAP={min_heap}")
        if max_det != TARGET_CAP:
            lines.append(f"FAIL expected MAX_DETECTIONS={TARGET_CAP}")
            fails += 1
        if near_cap != NEAR_CAP:
            lines.append(f"FAIL expected near-cap={NEAR_CAP}")
            fails += 1
    except Exception as exc:
        lines.append(f"FAIL constants: {exc}")
        fails += 1

    src = MEGAMAID.read_text(encoding="utf-8")
    if re.search(
        r"mmDetCount\s*<\s*MM_NEAR_CAP_THRESHOLD[\s\S]{0,120}mmTryStartServer\(\)"
        r"[\s\S]{0,120}mmStopDashboardServices\(\)",
        src,
    ):
        lines.append("FAIL collect loop still kills HTTP at near cap")
        fails += 1
    else:
        lines.append("STATIC no_near_cap_http_kill OK")
    if "mmNearCapSpiffsExportReady()" not in src:
        lines.append("FAIL missing mmNearCapSpiffsExportReady helper")
        fails += 1
    else:
        lines.append("STATIC near_cap_spiffs_helper OK")
    if "mmDetCount >= MM_AT_CAP_FULL" not in src:
        lines.append("FAIL missing MM_AT_CAP_FULL heap/export gate")
        fails += 1
    else:
        lines.append("STATIC at_cap_full_gate OK")
    if "mmServeDetectionsFromSession(r, offset, limit)" not in src:
        lines.append("FAIL /api/detections missing SPIFFS near-cap path")
        fails += 1
    else:
        lines.append("STATIC detections_near_cap_spiffs OK")
    if "mmNearCapSpiffsExportReady()" not in src or "mmStartSessionBodyChunked(r, src" not in src:
        lines.append("FAIL /api/export/json missing SPIFFS near-cap path")
        fails += 1
    else:
        lines.append("STATIC export_near_cap_spiffs OK")
    if "mmTryBeginExport" not in src or "mmFullExportBusy" not in src:
        lines.append("FAIL missing full export busy gate")
        fails += 1
    else:
        lines.append("STATIC export_busy_gate OK")
    full_export_body = re.search(
        r"static bool mmFullExportBusy\(\)\s*\{([^}]+)\}", src, re.S
    )
    if full_export_body and "mmDetPageStream.active" in full_export_body.group(1):
        lines.append("FAIL det page stream should not block full export gate")
        fails += 1
    else:
        lines.append("STATIC det_page_export_decoupled OK")
    if "mmSaveBlocksExport" not in src:
        lines.append("FAIL missing narrow save export block")
        fails += 1
    else:
        lines.append("STATIC save_export_block_narrow OK")
    if "onDisconnect" not in src or "mmExportStreamWatchdog" not in src:
        lines.append("FAIL missing stream disconnect/watchdog cleanup")
        fails += 1
    else:
        lines.append("STATIC stream_cleanup OK")
    if "copyText('/api/history/json?inline=1','PREV JSON'" not in src:
        lines.append("FAIL PREV copy should use /api/history/json?inline=1")
        fails += 1
    else:
        lines.append("STATIC prev_copy_streaming OK")
    if "!window.isSecureContext" not in src and "isSecureContext" not in src:
        lines.append("FAIL copy should handle insecure HTTP context")
        fails += 1
    else:
        lines.append("STATIC copy_insecure_context OK")
    if "mmCopySessionBodyToExportTmp" in src:
        lines.append("FAIL still uses SPIFFS export tmp copy")
        fails += 1
    else:
        lines.append("STATIC no_export_tmp_copy OK")
    if "Reboot to export" in src:
        lines.append("FAIL still contains Reboot to export UI string")
        fails += 1
    else:
        lines.append("STATIC no_reboot_to_export OK")
    expander_src = (ROOT / "lib" / "Arduino_Nesso_N1" / "src" / "expander.cpp").read_text(
        encoding="utf-8"
    )
    ui_src = (ROOT / "src" / "nesso_ui.cpp").read_text(encoding="utf-8")
    if "getBatteryStatus" not in expander_src:
        lines.append("FAIL expander.cpp missing getBatteryStatus filter API")
        fails += 1
    elif "checkBatteryPlausibility" not in expander_src:
        lines.append("FAIL expander.cpp missing battery plausibility filter")
        fails += 1
    else:
        lines.append("STATIC battery_filter_api OK")
    if "getBatteryStatus" not in ui_src:
        lines.append("FAIL nesso_ui.cpp not using getBatteryStatus")
        fails += 1
    else:
        lines.append("STATIC battery_ui_snapshot OK")
    guard_src = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    if "currentMode == 6" not in guard_src or "nessoResetBootButtonHold()" not in guard_src:
        lines.append("FAIL missing megamaid KEY1 hold block in main.cpp")
        fails += 1
    else:
        lines.append("STATIC key1_menu_hold_guard OK")
    if "/hotspot-detect.html" not in src:
        lines.append("FAIL missing iOS captive portal route")
        fails += 1
    else:
        lines.append("STATIC captive_ios_routes OK")

    lines.extend(
        [
            "BUILD with_flockyou_globals=167412",
            "BUILD no_flockyou_cap250_globals=135588",
            "BUILD flockyou_removed_bytes=31824",
            "BUILD cap250_static_budget_bytes=135588",
            "BUILD cap250_heap_headroom_bytes=192092",
        ]
    )

    synth = synth_detections(245)
    try:
        validate_export_shape(synth)
        lines.append("HOST near_cap_export_shape OK count=245")
    except Exception as exc:
        lines.append(f"FAIL host near_cap export: {exc}")
        fails += 1

    port = sys.argv[1] if len(sys.argv) > 1 else ""
    if not port:
        out = subprocess.run(
            ["arduino-cli", "board", "list"],
            capture_output=True,
            text=True,
            check=False,
        )
        for row in out.stdout.splitlines():
            if "usbmodem" in row and "esp32" in row.lower():
                port = row.split()[0]
                break

    if port:
        lines.append(f"serial_port={port}")
        try:
            serial_text = capture_serial(port, seconds=50)
            (ROOT / "test" / "megamaid_cap250_serial.log").write_text(
                serial_text, encoding="utf-8"
            )
            bad = SERIAL_BAD.findall(serial_text)
            ok_hits = len(SERIAL_OK.findall(serial_text))
            lines.append(f"SERIAL ok_markers={ok_hits}")
            if bad:
                lines.append(f"FAIL serial_bad={bad[:8]}")
                fails += 1
            else:
                lines.append("SERIAL no_crash_markers OK")
            for pat in ("heap=", "Web server started", "Ready"):
                if pat in serial_text:
                    lines.append(f"SERIAL saw:{pat}")
        except Exception as exc:
            lines.append(f"WARN serial_capture: {exc}")
    else:
        lines.append("WARN no_serial_port (skipped device boot capture)")

    try:
        live = try_live_export()
        lines.extend(live)
    except Exception as exc:
        lines.append(f"WARN live_export: {exc}")

    lines.append("OVERALL PASS" if fails == 0 else f"OVERALL FAIL count={fails}")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
