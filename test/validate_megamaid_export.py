#!/usr/bin/env python3
"""Validate Mega Maid /api/export/json against test reference schema."""
import json
import sys
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.4.1"
REF = sys.argv[2] if len(sys.argv) > 2 else "test/megamaid_test_detections.json"

REQUIRED_DET_FIELDS = {"mac", "rssi", "method", "first", "last", "count"}


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=15) as r:
        return r.read()


def main() -> int:
    stats = json.loads(fetch(f"{BASE}/api/stats"))
    print(f"radio_profile={stats.get('radio_profile')} total={stats.get('total')}")
    print(f"spiffs_ready={stats.get('spiffs_ready')} save_busy={stats.get('save_busy')} last_save_count={stats.get('last_save_count')} session_file_count={stats.get('session_file_count')}")
    print(f"gps_time_valid={stats.get('gps_time_valid')} gps_time_ms={stats.get('gps_time_ms')}")

    export = json.loads(fetch(f"{BASE}/api/export/json"))
    if not isinstance(export, list):
        print("FAIL: export is not a JSON array")
        return 1
    print(f"export_count={len(export)}")

    total = stats.get("total", 0)
    if len(export) != total:
        print(f"FAIL: export_count={len(export)} != stats.total={total}")
        return 1
    last_save = stats.get("last_save_count", -1)
    session_file = stats.get("session_file_count", -1)
    if total > 0 and last_save >= 0 and last_save != total:
        print(f"WARN: last_save_count={last_save} != total={total}")
    if total >= 250 and session_file >= 0 and session_file != total:
        print(f"WARN: session_file_count={session_file} != total={total}")

    if export:
        sample = export[0]
        missing = REQUIRED_DET_FIELDS - set(sample.keys())
        if missing:
            print(f"FAIL: export detection missing fields: {missing}")
            return 1
        print(f"sample_mac={sample.get('mac')} method={sample.get('method')}")
        if stats.get("gps_time_valid"):
            min_epoch = 1_577_836_800_000  # 2020-01-01 UTC
            for field in ("first", "last"):
                val = sample.get(field, 0)
                if not isinstance(val, int) or val < min_epoch:
                    print(f"FAIL: {field}={val} not plausible UTC epoch ms with gps_time_valid")
                    return 1
        else:
            for field in ("first", "last"):
                if sample.get(field, 0) != 0:
                    print(f"WARN: {field}={sample.get(field)} before GPS time sync (expected 0)")

        sights = sample.get("gps_sights")
        if sights is not None:
            if not isinstance(sights, list):
                print("FAIL: gps_sights must be a JSON array when present")
                return 1
            for i, s in enumerate(sights):
                for key in ("lat", "lon", "acc", "t"):
                    if key not in s:
                        print(f"FAIL: gps_sights[{i}] missing '{key}'")
                        return 1
            print(f"sample_gps_sights={len(sights)}")

    multi_sight = [d for d in export if isinstance(d.get("gps_sights"), list) and len(d["gps_sights"]) > 1]
    if multi_sight:
        print(f"multi_sight_macs={len(multi_sight)} example={multi_sight[0].get('mac')}")

    with open(REF, encoding="utf-8") as f:
        ref = json.load(f)
    ref_methods = set(ref.get("expected_methods", []))
    if export:
        live_methods = {d.get("method") for d in export}
        print(f"live_methods={sorted(live_methods)}")
    print(f"ref_expected_methods={sorted(ref_methods)}")

    # If we have history, validate one prior detection shape
    try:
        hist = json.loads(fetch(f"{BASE}/api/history"))
        if isinstance(hist, list) and hist:
            h0 = hist[0]
            miss = REQUIRED_DET_FIELDS - set(h0.keys())
            if miss:
                print(f"FAIL: history detection missing fields: {miss}")
                return 1
            print(f"history_count={len(hist)} sample={h0.get('mac')}")
    except Exception as e:
        print(f"history_skip: {e}")

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
