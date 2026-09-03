#!/usr/bin/env python3
"""
flock_detector.py — Raspberry Pi 5 + Google Coral TPU visual Flock detector

Consumes the ESP32 Visual Scout's MJPEG camera stream, runs YOLO inference
(optionally accelerated by Coral Edge TPU), and records detection frames +
GPS metadata as labelled training data to the NVMe SSD.

Architecture:
  ┌─────────────────────┐       WiFi
  │  ESP32-S3 WROOM     │ ──────────────► :81/stream   (MJPEG)
  │  Visual Scout mode  │ ──────────────► :81/gps      (JSON GPS)
  └─────────────────────┘
           │
    ┌──────▼──────────────────────────────────────────────┐
    │  flock_detector.py  (Raspberry Pi 5)                │
    │                                                     │
    │  1. Pull MJPEG frames via requests streaming        │
    │  2. Decode JPEG → np.ndarray with cv2               │
    │  3. Infer with YOLOv8n (CPU) or YOLOv8-coral (TPU) │
    │  4. Any detection above confidence threshold:       │
    │     • Annotate frame                                │
    │     • Fetch current GPS from :81/gps               │
    │     • Save to training_data/ on NVMe SSD            │
    │     • Optionally upload to crowd-source server      │
    └──────────────────────────────────────────────────────┘

Requirements:
  pip install -r requirements.txt
  (For Coral TPU: also follow the PyCoral setup at coral.ai/docs/m2/get-started)

Usage:
  python3 flock_detector.py --esp32 http://192.168.4.1

  Flags:
    --esp32         ESP32 base URL          [default: http://192.168.4.1]
    --model         Path to YOLO model      [default: models/flock_yolov8n.pt]
    --conf          Detection confidence    [default: 0.45]
    --save-all      Save every frame (not just detections)
    --output        Training data directory [default: /mnt/ssd/training_data]
    --upload-url    Crowd-source upload URL [optional]
    --coral         Use Coral TPU (requires pycoral + Edge TPU runtime)
    --coral-model   Path to .tflite model   [default: models/flock_yolov8n_edgetpu.tflite]
    --show          Show live annotated preview (requires display)
"""

import argparse
import io
import json
import logging
import os
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
import requests
from PIL import Image

log = logging.getLogger("flock_detector")

# ── Detection record schema ───────────────────────────────────────────────────
# Each detection is saved as:
#   <output_dir>/
#     YYYYMMDD/
#       <timestamp>_<lat>_<lon>.jpg        — annotated frame
#       <timestamp>_<lat>_<lon>.json       — metadata sidecar
#
# The JSON sidecar contains:
#   ts_utc, lat, lon, gps_valid, gps_sats,
#   detections: [{class, confidence, bbox: [x1,y1,x2,y2]}]
#   model, esp32_url, frame_w, frame_h


def setup_logging(verbose: bool):
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )


def mjpeg_frames(url: str):
    """Generator that yields raw JPEG bytes from an MJPEG stream."""
    log.info("Connecting to MJPEG stream: %s", url)
    resp = requests.get(url, stream=True, timeout=30)
    resp.raise_for_status()

    boundary = None
    ct = resp.headers.get("Content-Type", "")
    for part in ct.split(";"):
        part = part.strip()
        if part.startswith("boundary="):
            boundary = part[len("boundary="):].encode()
            break

    buf = b""
    for chunk in resp.iter_content(chunk_size=16384):
        buf += chunk
        while True:
            # Find JPEG start
            soi = buf.find(b"\xff\xd8")
            if soi < 0:
                buf = buf[-3:]  # keep possible partial marker
                break
            eoi = buf.find(b"\xff\xd9", soi + 2)
            if eoi < 0:
                break
            yield buf[soi:eoi + 2]
            buf = buf[eoi + 2:]


def fetch_gps(base_url: str) -> dict:
    """Fetch current GPS from the ESP32's :81/gps endpoint (best-effort)."""
    try:
        r = requests.get(f"{base_url}:81/gps" if ":81" not in base_url else f"{base_url}/gps",
                         timeout=2)
        return r.json()
    except Exception:
        return {"valid": False, "lat": 0.0, "lon": 0.0, "sats": 0}


# ── Model loading ─────────────────────────────────────────────────────────────

def load_yolo_model(model_path: str):
    """Load a YOLOv8 model via ultralytics."""
    from ultralytics import YOLO
    log.info("Loading YOLO model: %s", model_path)
    model = YOLO(model_path)
    log.info("YOLO model ready (classes: %s)", list(model.names.values()))
    return model


def load_coral_model(tflite_path: str):
    """Load a compiled Edge TPU .tflite model via pycoral."""
    from pycoral.utils.edgetpu import make_interpreter
    from pycoral.adapters import common as coral_common
    log.info("Loading Coral model: %s", tflite_path)
    interp = make_interpreter(tflite_path)
    interp.allocate_tensors()
    log.info("Coral interpreter ready. Input: %s", coral_common.input_size(interp))
    return interp


def infer_yolo(model, frame: np.ndarray, conf: float) -> list[dict]:
    results = model(frame, conf=conf, verbose=False)
    detections = []
    for r in results:
        for box in r.boxes:
            detections.append({
                "class":      model.names[int(box.cls)],
                "confidence": float(box.conf),
                "bbox":       [float(x) for x in box.xyxy[0]],
            })
    return detections


def infer_coral(interp, frame: np.ndarray, conf: float,
                input_size: tuple[int, int]) -> list[dict]:
    """Run inference on a Coral Edge TPU."""
    from pycoral.adapters import common as coral_common
    from pycoral.adapters import detect as coral_detect

    img = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
    img = img.resize(input_size, Image.BILINEAR)
    coral_common.set_input(interp, img)
    interp.invoke()
    objs = coral_detect.get_objects(interp, score_threshold=conf)
    labels = getattr(interp, "_labels", {})
    return [
        {
            "class":      labels.get(obj.id, str(obj.id)),
            "confidence": float(obj.score),
            "bbox":       [obj.bbox.xmin, obj.bbox.ymin,
                           obj.bbox.xmax, obj.bbox.ymax],
        }
        for obj in objs
    ]


# ── Training data recorder ────────────────────────────────────────────────────

class TrainingRecorder:
    """
    Saves annotated frames + JSON metadata to the NVMe SSD.

    Designed for a Pi 5 with an M.2 HAT (NVMe SSD mounted at /mnt/ssd).
    Falls back gracefully to any writable directory.
    """

    def __init__(self, output_dir: str, upload_url: str | None = None):
        self.root       = Path(output_dir)
        self.upload_url = upload_url
        self._lock      = threading.Lock()
        self._queue: list[dict] = []
        if upload_url:
            threading.Thread(target=self._upload_worker, daemon=True).start()

    def record(self, frame: np.ndarray, detections: list[dict],
               gps: dict, model_name: str, esp32_url: str) -> Path:
        ts = datetime.now(timezone.utc)
        day_dir = self.root / ts.strftime("%Y%m%d")
        day_dir.mkdir(parents=True, exist_ok=True)

        lat_s = f"{gps['lat']:.6f}" if gps.get("valid") else "0.000000"
        lon_s = f"{gps['lon']:.6f}" if gps.get("valid") else "0.000000"
        stem  = f"{ts.strftime('%H%M%S_%f')}_{lat_s}_{lon_s}"

        # Annotate frame
        ann = frame.copy()
        for d in detections:
            x1, y1, x2, y2 = [int(v) for v in d["bbox"]]
            cv2.rectangle(ann, (x1, y1), (x2, y2), (0, 0, 255), 2)
            label = f"{d['class']} {d['confidence']:.2f}"
            cv2.putText(ann, label, (x1, y1 - 6),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 255), 1)

        jpg_path  = day_dir / f"{stem}.jpg"
        json_path = day_dir / f"{stem}.json"

        cv2.imwrite(str(jpg_path), ann, [cv2.IMWRITE_JPEG_QUALITY, 92])

        meta = {
            "ts_utc":     ts.isoformat(),
            "lat":        gps.get("lat", 0.0),
            "lon":        gps.get("lon", 0.0),
            "gps_valid":  gps.get("valid", False),
            "gps_sats":   gps.get("sats", 0),
            "detections": detections,
            "model":      model_name,
            "esp32_url":  esp32_url,
            "frame_w":    frame.shape[1],
            "frame_h":    frame.shape[0],
        }
        json_path.write_text(json.dumps(meta, indent=2))

        log.info("Saved: %s (%d detections)", jpg_path.name, len(detections))

        if self.upload_url:
            with self._lock:
                self._queue.append({"jpg": str(jpg_path), "meta": meta})

        return jpg_path

    def _upload_worker(self):
        """Background thread that uploads recordings to the crowd-source server."""
        while True:
            time.sleep(5)
            with self._lock:
                items = self._queue[:]
                self._queue.clear()
            for item in items:
                try:
                    jpg_path = Path(item["jpg"])
                    if not jpg_path.exists():
                        continue
                    with open(jpg_path, "rb") as f:
                        resp = requests.post(
                            self.upload_url,
                            files={"frame": (jpg_path.name, f, "image/jpeg")},
                            data={"meta": json.dumps(item["meta"])},
                            timeout=30,
                        )
                    if resp.ok:
                        log.info("Uploaded %s", jpg_path.name)
                    else:
                        log.warning("Upload failed %s: HTTP %d", jpg_path.name, resp.status_code)
                except Exception as exc:
                    log.warning("Upload error: %s", exc)
                    with self._lock:
                        self._queue.insert(0, item)   # retry
                    break


# ── Main detection loop ───────────────────────────────────────────────────────

def run(args):
    setup_logging(getattr(args, "verbose", False))

    # Resolve stream + GPS URLs
    base = args.esp32.rstrip("/")
    # MJPEG stream is on port 81
    if ":81" in base:
        stream_url = f"{base}/stream"
        gps_base   = base
    else:
        stream_url = f"{base}:81/stream"
        gps_base   = f"{base}:81"

    # Load model
    if args.coral:
        interp     = load_coral_model(args.coral_model)
        from pycoral.adapters import common as coral_common
        input_size = coral_common.input_size(interp)
        # Load label map if present alongside the model
        lbl_path = Path(args.coral_model).with_suffix("").parent / "labels.txt"
        if lbl_path.exists():
            interp._labels = {i: l.strip()
                              for i, l in enumerate(lbl_path.read_text().splitlines())}
        infer = lambda frame: infer_coral(interp, frame, args.conf, input_size)
        model_name = Path(args.coral_model).stem
    else:
        model      = load_yolo_model(args.model)
        infer      = lambda frame: infer_yolo(model, frame, args.conf)
        model_name = Path(args.model).stem

    recorder = TrainingRecorder(args.output, getattr(args, "upload_url", None))

    frame_count = 0
    det_count   = 0
    t0          = time.time()

    log.info("Starting detection loop (stream: %s)", stream_url)
    log.info("Training data → %s", args.output)

    for jpg_bytes in mjpeg_frames(stream_url):
        frame_count += 1

        # Decode
        arr   = np.frombuffer(jpg_bytes, dtype=np.uint8)
        frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if frame is None:
            continue

        # Infer
        detections = infer(frame)

        should_save = detections or args.save_all
        if should_save:
            gps = fetch_gps(gps_base)
            recorder.record(frame, detections, gps, model_name, base)
            det_count += len(detections)

        # Live preview
        if args.show:
            ann = frame.copy()
            for d in detections:
                x1, y1, x2, y2 = [int(v) for v in d["bbox"]]
                cv2.rectangle(ann, (x1, y1), (x2, y2), (0, 0, 255), 2)
                cv2.putText(ann, f"{d['class']} {d['confidence']:.2f}",
                            (x1, y1 - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 255), 1)
            fps = frame_count / max(1, time.time() - t0)
            cv2.putText(ann, f"FPS:{fps:.1f} dets:{det_count}",
                        (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
            cv2.imshow("Visual Scout", ann)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

        # Log stats every 100 frames
        if frame_count % 100 == 0:
            fps = frame_count / max(1, time.time() - t0)
            log.info("Frames: %d  Detections: %d  FPS: %.1f",
                     frame_count, det_count, fps)

    cv2.destroyAllWindows()


def main():
    p = argparse.ArgumentParser(description="Flock camera visual detector (Pi 5 + Coral)")
    p.add_argument("--esp32",        default="http://192.168.4.1",
                   help="ESP32 Visual Scout base URL")
    p.add_argument("--model",        default="models/flock_yolov8n.pt",
                   help="YOLOv8 model path (.pt or .onnx)")
    p.add_argument("--conf",         type=float, default=0.45,
                   help="Detection confidence threshold")
    p.add_argument("--output",       default="/mnt/ssd/training_data",
                   help="Training data output directory")
    p.add_argument("--save-all",     action="store_true",
                   help="Save every frame, not just detections")
    p.add_argument("--upload-url",   default=None,
                   help="POST URL for crowd-source training data upload")
    p.add_argument("--coral",        action="store_true",
                   help="Use Coral Edge TPU (requires pycoral + runtime)")
    p.add_argument("--coral-model",  default="models/flock_yolov8n_edgetpu.tflite",
                   help="Compiled Edge TPU .tflite model path")
    p.add_argument("--show",         action="store_true",
                   help="Show live annotated preview window")
    p.add_argument("--verbose",      action="store_true")
    args = p.parse_args()
    run(args)


if __name__ == "__main__":
    main()
