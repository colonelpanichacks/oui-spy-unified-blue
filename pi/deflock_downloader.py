#!/usr/bin/env python3
"""
deflock_downloader.py — Raspberry Pi Deflock DB proxy for Visual Scout

Downloads the full DeFlock camera list, filters it to Flock Safety cameras
only, and serves a compact binary file the ESP32 can fetch directly.

Binary format (same as deflock.bin on the ESP32):
  4 bytes   uint32_le  — camera count N
  N × 8     float32_le lat, float32_le lon

Usage:
  python3 deflock_downloader.py [--host 0.0.0.0] [--port 5000]

The ESP32's "Pi Proxy URL" should be set to:
  http://<this-pi-ip>:5000/flock_cameras.bin

The endpoint also serves:
  GET /flock_cameras.bin   — compact binary (ESP32 format)
  GET /flock_cameras.geojson — filtered GeoJSON for GIS tools
  GET /status              — JSON status (camera count, last sync, version)
  POST /sync               — trigger a manual re-download
"""

import argparse
import gzip
import json
import logging
import os
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from io import BytesIO
from pathlib import Path

import requests

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("deflock_dl")

# ── Config ────────────────────────────────────────────────────────────────────

CACHE_DIR = Path(__file__).parent / "cache"
CACHE_DIR.mkdir(exist_ok=True)

BIN_PATH     = CACHE_DIR / "flock_cameras.bin"
GEOJSON_PATH = CACHE_DIR / "flock_cameras.geojson"
STATUS_PATH  = CACHE_DIR / "status.json"

# Download sources — tried in order
SOURCES = [
    # Plain JSON array — fastest to parse, confirmed publicly accessible
    {
        "name": "data.dontgetflocked.com cameras-us.json",
        "url":  "https://data.dontgetflocked.com/cameras-us.json",
        "fmt":  "json_array",
    },
    # GeoJSON FeatureCollection (may be Cloudflare-gated for some clients)
    {
        "name": "data.dontgetflocked.com cameras.geojson.gz",
        "url":  "https://data.dontgetflocked.com/cameras.geojson.gz",
        "fmt":  "geojson_gz",
    },
    # Overpass query — authoritative but slowest
    {
        "name": "Overpass ALPR query",
        "url":  "https://overpass.deflock.org/api/interpreter",
        "fmt":  "overpass",
        "data": (
            '[out:json][timeout:55];'
            '('
            '  node["man_made"="surveillance"]["surveillance:type"="ALPR"]'
            '      ["brand"="Flock Safety"];'
            '  way["man_made"="surveillance"]["surveillance:type"="ALPR"]'
            '     ["brand"="Flock Safety"];'
            ');'
            'out center;'
        ),
    },
]

REQUEST_HEADERS = {
    "User-Agent": "OUI-Spy/VisualScout (ESP32 camera bridge; +https://github.com/colonelpanichacks/oui-spy-unified-blue)",
    "Accept": "application/json, application/geo+json, */*",
}

# Tags that indicate a Flock Safety camera
FLOCK_KEYWORDS = ("flock safety", "flock")


def _is_flock(tags: dict) -> bool:
    """Return True if any relevant tag contains a Flock Safety identifier."""
    check_keys = ("brand", "surveillance:brand", "surveillance:manufacturer",
                  "operator", "manufacturer")
    for k in check_keys:
        v = str(tags.get(k, "")).lower()
        if any(kw in v for kw in FLOCK_KEYWORDS):
            return True
    return False


# ── Downloader ────────────────────────────────────────────────────────────────

sync_lock  = threading.Lock()
sync_state = {"running": False, "msg": "Not synced yet", "cameras": 0, "ts": 0}


def _parse_json_array(raw: bytes) -> list[tuple[float, float]]:
    """Parse flat JSON array: [{id, lat, lon, tags:{...}}, ...]"""
    data = json.loads(raw)
    cameras = []
    for node in data:
        tags = node.get("tags", {})
        if not _is_flock(tags):
            continue
        lat = node.get("lat") or node.get("center", {}).get("lat")
        lon = node.get("lon") or node.get("center", {}).get("lon")
        if lat is None or lon is None:
            continue
        cameras.append((float(lat), float(lon)))
    return cameras


def _parse_geojson(raw: bytes) -> list[tuple[float, float]]:
    """Parse GeoJSON FeatureCollection."""
    fc = json.loads(raw)
    cameras = []
    for feat in fc.get("features", []):
        props = feat.get("properties", {})
        if not _is_flock(props):
            continue
        coords = feat.get("geometry", {}).get("coordinates", [])
        if len(coords) >= 2:
            cameras.append((float(coords[1]), float(coords[0])))
    return cameras


def _parse_overpass(raw: bytes) -> list[tuple[float, float]]:
    """Parse Overpass JSON (already filtered by the query)."""
    data = json.loads(raw)
    cameras = []
    for elem in data.get("elements", []):
        lat = elem.get("lat") or elem.get("center", {}).get("lat")
        lon = elem.get("lon") or elem.get("center", {}).get("lon")
        if lat is not None and lon is not None:
            cameras.append((float(lat), float(lon)))
    return cameras


def write_binary(cameras: list[tuple[float, float]]) -> None:
    """Write packed binary: uint32 count + N×{float32 lat, float32 lon}."""
    buf = BytesIO()
    buf.write(struct.pack("<I", len(cameras)))
    for lat, lon in cameras:
        buf.write(struct.pack("<ff", lat, lon))
    BIN_PATH.write_bytes(buf.getvalue())
    log.info("Wrote %d cameras to %s (%d KB)", len(cameras), BIN_PATH,
             len(buf.getvalue()) // 1024)


def write_geojson(cameras: list[tuple[float, float]]) -> None:
    """Write a filtered GeoJSON for use with QGIS / leaflet etc."""
    fc = {
        "type": "FeatureCollection",
        "features": [
            {"type": "Feature",
             "geometry": {"type": "Point", "coordinates": [lon, lat]},
             "properties": {"brand": "Flock Safety"}}
            for lat, lon in cameras
        ],
    }
    GEOJSON_PATH.write_text(json.dumps(fc))


def _do_sync():
    with sync_lock:
        if sync_state["running"]:
            return
        sync_state["running"] = True

    cameras: list[tuple[float, float]] = []
    last_err = ""

    for src in SOURCES:
        log.info("Trying source: %s", src["name"])
        try:
            if src["fmt"] == "overpass":
                resp = requests.post(src["url"], data={"data": src["data"]},
                                     headers=REQUEST_HEADERS, timeout=60)
            else:
                resp = requests.get(src["url"], headers=REQUEST_HEADERS, timeout=60)
            resp.raise_for_status()

            raw = resp.content
            if src["fmt"] == "geojson_gz":
                raw = gzip.decompress(raw)

            parsers = {
                "json_array": _parse_json_array,
                "geojson_gz": _parse_geojson,
                "geojson":    _parse_geojson,
                "overpass":   _parse_overpass,
            }
            cameras = parsers[src["fmt"]](raw)
            log.info("Source %s → %d Flock cameras", src["name"], len(cameras))
            if cameras:
                break
        except Exception as exc:
            last_err = str(exc)
            log.warning("Source %s failed: %s", src["name"], exc)

    if cameras:
        write_binary(cameras)
        write_geojson(cameras)
        msg = f"OK — {len(cameras):,} Flock Safety cameras"
    else:
        msg = f"All sources failed. Last error: {last_err}"
        log.error(msg)

    with sync_lock:
        sync_state.update({"running": False, "msg": msg,
                            "cameras": len(cameras), "ts": int(time.time())})

    STATUS_PATH.write_text(json.dumps(sync_state))


def sync_background():
    threading.Thread(target=_do_sync, daemon=True).start()


# ── HTTP server ───────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.debug("HTTP %s", fmt % args)

    def send_json(self, data: dict, code: int = 200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/flock_cameras.bin":
            if not BIN_PATH.exists():
                self.send_json({"error": "No DB yet — POST /sync first"}, 503)
                return
            data = BIN_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)

        elif path == "/flock_cameras.geojson":
            if not GEOJSON_PATH.exists():
                self.send_json({"error": "No GeoJSON yet — POST /sync first"}, 503)
                return
            data = GEOJSON_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/geo+json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)

        elif path == "/status":
            s = dict(sync_state)
            s["bin_exists"]   = BIN_PATH.exists()
            s["bin_bytes"]    = BIN_PATH.stat().st_size if BIN_PATH.exists() else 0
            self.send_json(s)

        else:
            self.send_json({"error": "Not found"}, 404)

    def do_POST(self):
        if self.path == "/sync":
            sync_background()
            self.send_json({"ok": True, "msg": "Sync started"})
        else:
            self.send_json({"error": "Not found"}, 404)


def main():
    ap = argparse.ArgumentParser(description="Deflock DB proxy for Visual Scout")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--sync-on-start", action="store_true", default=True,
                    help="Trigger a sync immediately on startup (default: on)")
    ap.add_argument("--no-sync-on-start", dest="sync_on_start", action="store_false")
    ap.add_argument("--sync-interval-h", type=float, default=6,
                    help="Re-sync interval in hours (default: 6)")
    args = ap.parse_args()

    # Load cached status
    if STATUS_PATH.exists():
        try:
            sync_state.update(json.loads(STATUS_PATH.read_text()))
        except Exception:
            pass

    if args.sync_on_start:
        log.info("Starting initial sync…")
        sync_background()

    # Background periodic re-sync
    def _periodic():
        while True:
            time.sleep(args.sync_interval_h * 3600)
            log.info("Periodic sync triggered")
            sync_background()
    threading.Thread(target=_periodic, daemon=True).start()

    srv = HTTPServer((args.host, args.port), Handler)
    log.info("Deflock proxy listening on http://%s:%d", args.host, args.port)
    log.info("  ESP32 URL: http://<this-ip>:%d/flock_cameras.bin", args.port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
